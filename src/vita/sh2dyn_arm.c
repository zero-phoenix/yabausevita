#include <string.h>
#include <psp2/kernel/sysmem.h>
#include "sh2core.h"
#include "sh2int.h"
#include "sh2idle.h"
#include "memory.h"

#define SH2DYN_ARM_ID 4

extern int vita_log(const char *fmt, ...);
extern int SH2InterpreterInit(void);

/* Variables y funciones externas necesarias para el Fallback al intérprete */
extern opcodefunc opcodes[0x10000];
extern fetchfunc fetchlist[0x100];

static SceUID jit_memblock = -1;
static void* jit_memory = NULL;
static uint32_t* jit_ptr = NULL; // Puntero de compilación (Code Emitter)

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX 0x0C20D060
#endif

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 1 — EMISOR DE CÓDIGO ARM (CODE EMITTER)
   ══════════════════════════════════════════════════════════════ */
static void emit_instruction(uint32_t inst) {
    if (jit_ptr) {
        *jit_ptr++ = inst;
    }
}

/* Macro/función rápida para emitir retorno */
static void emit_bx_lr(void) {
    emit_instruction(0xE12FFF1E);
}


/* ══════════════════════════════════════════════════════════════
   SECCIÓN 2 — DECODIFICADOR DE BLOQUES (BLOCK DECODER)
   ══════════════════════════════════════════════════════════════ */
/* 
 * Intenta compilar un bloque de instrucciones SH-2 en ARM.
 * Retorna 1 si tuvo éxito compilando algo, 0 si abortó por instrucción desconocida.
 */
static int decode_block(u32 pc) {
    u32 page = (pc >> 20) & 0x0FF;
    fetchfunc cached_fetch = fetchlist[page];
    if (!cached_fetch) return 0; // Memoria no válida
    
    u16 instruction = (u16)cached_fetch(pc);
    
    // Extraemos el "Nibble" (los 4 bits más altos) para identificar el grupo
    u8 opcode_type = (instruction >> 12) & 0x0F;
    
    switch (opcode_type) {
        case 0x0: 
            // Grupo 0000: NOP, MOV, etc.
            if (instruction == 0x0009) { // NOP
                // Generar equivalente ARM NOP (MOV r0, r0)
                emit_instruction(0xE1A00000); 
            } else {
                return 0; // Instrucción no soportada aún
            }
            break;
            
        case 0xE: 
            // Grupo MOV #imm, Rn
            // Próximamente se implementará
            return 0;
            
        default:
            return 0; // Instrucción no soportada, abortar compilación
    }
    
    // Cerramos el bloque con un salto de retorno al emulador
    emit_bx_lr();
    return 1;
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 3 — FUNCIONES NÚCLEO
   ══════════════════════════════════════════════════════════════ */
static int sh2dyn_arm_init(void)
{
    vita_log("[SH2DynARM] Initializing ARM JIT core...\n");
    jit_memblock = sceKernelAllocMemBlock("Yabause_JIT_Cache",
                                          SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX,
                                          8 * 1024 * 1024,
                                          NULL);
    if (jit_memblock >= 0) {
        sceKernelGetMemBlockBase(jit_memblock, &jit_memory);
        jit_ptr = (uint32_t*)jit_memory;
        vita_log("[SH2DynARM] Allocated 8MB JIT memory at %p\n", jit_memory);
    }
    return SH2InterpreterInit();
}

static void sh2dyn_arm_deinit(void)
{
    if (jit_memblock >= 0) {
        sceKernelFreeMemBlock(jit_memblock);
        jit_memblock = -1;
        jit_memory = NULL;
        jit_ptr = NULL;
    }
}

static int sh2dyn_arm_reset(void) { 
    jit_ptr = (uint32_t*)jit_memory; // Limpiar caché de compilación
    return 0; 
}


/* ══════════════════════════════════════════════════════════════
   SECCIÓN 4 — BUCLE PRINCIPAL Y FALLBACK
   ══════════════════════════════════════════════════════════════ */
static void FASTCALL sh2dyn_arm_exec(SH2_struct *restrict context, u32 cycles)
{
    if (!context) return;
    
    /* 
     * Optimización de inactividad (Idle Skip).
     * Vital para evitar caídas de cuadros extremas en Yabause.
     */
    if (__builtin_expect(context->isIdle, 0)) {
        SH2idleParse(context, cycles);
        return;
    }
    SH2idleCheck(context, cycles);

    u32 last_page = 0xFFFFFFFF;
    fetchfunc cached_fetch = NULL;
    opcodefunc *restrict local_opcodes = opcodes;

    while (__builtin_expect(context->cycles < cycles, 1))
    {
        u32 pc = context->regs.PC;
        
        // -------------------------------------------------------------------
        // PASO 1: INTENTAR COMPILAR/EJECUTAR JIT (Simulación)
        // -------------------------------------------------------------------
        // Más adelante, aquí comprobaremos si PC está en nuestra tabla de caché hash.
        // Si no está, compilamos usando decode_block(pc).
        // Si está, saltamos a la memoria ejecutable en ARM nativo.
        // (Desactivado para el gameplay general hasta tener registros ARM mapeados)
        // -------------------------------------------------------------------
        
        
        // -------------------------------------------------------------------
        // PASO 2: FALLBACK AL INTÉRPRETE RÁPIDO EN C
        // -------------------------------------------------------------------
        // Si el JIT aún no soporta la instrucción, la ejecutamos por C.
        // Esto garantiza que el juego NO se congele ni baje a 4 FPS inútilmente.
        
        u32 page = (pc >> 20) & 0x0FF;

        if (__builtin_expect(page != last_page, 0))
        {
            last_page = page;
            cached_fetch = fetchlist[page];
        }

        // Obtener el OpCode de Saturn
        if (__builtin_expect(cached_fetch != NULL, 1))
            context->instruction = (u16)cached_fetch(pc);
        else
            context->instruction = 0xFFFF;

        // Ejecutar función de C del Intérprete
        void (*handler)(SH2_struct *) = local_opcodes[context->instruction];
        if (__builtin_expect(handler != NULL, 1))
            handler(context);
        else
            context->regs.PC += 2; // Avanzar el contador si es instrucción inválida

        context->cycles++;
    }
}

static void sh2dyn_arm_write_notify(u32 start, u32 length) { 
    // Invalidar caché (SMC) - Se programará luego
    (void)start; (void)length; 
}

SH2Interface_struct SH2DynARM = {
    SH2DYN_ARM_ID,
    "SH2 ARM Dynamic Recompiler (WIP)",
    sh2dyn_arm_init,
    sh2dyn_arm_deinit,
    sh2dyn_arm_reset,
    sh2dyn_arm_exec,
    sh2dyn_arm_write_notify
};
