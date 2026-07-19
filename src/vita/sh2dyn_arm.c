#include <string.h>
#include <psp2/kernel/sysmem.h>
#include "sh2core.h"
#include "sh2int.h"
#include "sh2idle.h"
#include "memory.h"

#define SH2DYN_ARM_ID 4

extern int vita_log(const char *fmt, ...);
extern int SH2InterpreterInit(void);

extern opcodefunc opcodes[0x10000];
extern fetchfunc fetchlist[0x100];

static SceUID jit_memblock = -1;
static void* jit_memory = NULL;
static uint32_t* jit_ptr = NULL; 

#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX 0x0C20D060
#endif

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 0 — JIT HASH CACHE Y CONTEXTO
   ══════════════════════════════════════════════════════════════ */
/* 
 * El puntero de función JIT ahora recibe el contexto en el primer 
 * argumento (Registro r0 de ARM nativo), permitiendo leer/escribir estado.
 */
typedef void (*jit_block_t)(SH2_struct*);

#define JIT_HASH_SIZE 8192
typedef struct {
    u32 pc;
    jit_block_t block;
} JITCacheEntry;

static JITCacheEntry jit_hash[JIT_HASH_SIZE];

static inline void jit_cache_add(u32 pc, jit_block_t block) {
    // Hash extremadamente rápido alineado a las instrucciones de 16-bits
    jit_hash[(pc >> 1) % JIT_HASH_SIZE] = (JITCacheEntry){pc, block};
}

static inline jit_block_t jit_cache_lookup(u32 pc) {
    JITCacheEntry entry = jit_hash[(pc >> 1) % JIT_HASH_SIZE];
    if (entry.pc == pc) return entry.block;
    return NULL;
}


/* ══════════════════════════════════════════════════════════════
   SECCIÓN 1 — EMISOR DE CÓDIGO ARM (CODE EMITTER)
   ══════════════════════════════════════════════════════════════ */
static void emit_instruction(uint32_t inst) {
    if (jit_ptr) {
        *jit_ptr++ = inst;
    }
}

static void emit_bx_lr(void) {
    emit_instruction(0xE12FFF1E);
}


/* ══════════════════════════════════════════════════════════════
   SECCIÓN 2 — DECODIFICADOR DE BLOQUES (BLOCK DECODER)
   ══════════════════════════════════════════════════════════════ */
static int decode_block(u32 pc) {
    u32 page = (pc >> 20) & 0x0FF;
    fetchfunc cached_fetch = fetchlist[page];
    if (!cached_fetch) return 0;
    
    u16 instruction = (u16)cached_fetch(pc);
    u8 opcode_type = (instruction >> 12) & 0x0F;
    
    switch (opcode_type) {
        case 0x0: 
            if (instruction == 0x0009) { 
                // Instrucción de prueba NOP de Saturn (0x0009)
                // Emitir instrucción ARM NOP real (MOV R0, R0 equivalente a E1A00000)
                emit_instruction(0xE1A00000); 
            } else {
                return 0; 
            }
            break;
            
        case 0xE: 
            return 0;
            
        default:
            return 0; 
    }
    
    // Cierre obligatorio del bloque dinámico (Retorno seguro al motor C)
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
    memset(jit_hash, 0, sizeof(jit_hash));
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
    jit_ptr = (uint32_t*)jit_memory; 
    memset(jit_hash, 0, sizeof(jit_hash));
    return 0; 
}


/* ══════════════════════════════════════════════════════════════
   SECCIÓN 4 — BUCLE PRINCIPAL Y FALLBACK
   ══════════════════════════════════════════════════════════════ */
static void FASTCALL sh2dyn_arm_exec(SH2_struct *restrict context, u32 cycles)
{
    if (!context) return;
    
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
        // PASO 1: EJECUCIÓN JIT DINÁMICA
        // -------------------------------------------------------------------
        // Verificamos en microsegundos si esta instrucción ya fue compilada.
        jit_block_t block = jit_cache_lookup(pc);
        if (!block) {
            uint32_t* start_ptr = jit_ptr;
            if (decode_block(pc)) {
                // Compilación exitosa: limpiamos caché I/D e insertamos en el Hash map.
                __clear_cache((char*)start_ptr, (char*)jit_ptr);
                block = (jit_block_t)start_ptr;
                jit_cache_add(pc, block);
            }
        }
        
        if (block) {
            // ¡EJECUCIÓN NATIVA ARM ACTIVA!
            // Llamamos a nuestro bloque pasándole los registros de Saturn.
            block(context); 
            
            // TEMPORAL: Como las instrucciones apenas son stubs, avanzamos estado 
            // manualmente hasta que el código ARM contenga su propio R0->cycles++ interno.
            context->regs.PC += 2;
            context->cycles++;
            continue; // Saltamos directamente a la siguiente instrucción Saturn
        }

        // -------------------------------------------------------------------
        // PASO 2: FALLBACK AL INTÉRPRETE DE ALTA VELOCIDAD
        // -------------------------------------------------------------------
        u32 page = (pc >> 20) & 0x0FF;

        if (__builtin_expect(page != last_page, 0))
        {
            last_page = page;
            cached_fetch = fetchlist[page];
        }

        if (__builtin_expect(cached_fetch != NULL, 1))
            context->instruction = (u16)cached_fetch(pc);
        else
            context->instruction = 0xFFFF;

        void (*handler)(SH2_struct *) = local_opcodes[context->instruction];
        if (__builtin_expect(handler != NULL, 1))
            handler(context);
        else
            context->regs.PC += 2;

        context->cycles++;
    }
}

static void sh2dyn_arm_write_notify(u32 start, u32 length) { 
    // Todo: Programar limpieza del Hash Map si el juego borra código en memoria
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
