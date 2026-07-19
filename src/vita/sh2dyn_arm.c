#include <string.h>
#include <psp2/kernel/sysmem.h>
#include "sh2core.h"

#define SH2DYN_ARM_ID 4

extern int vita_log(const char *fmt, ...);
extern int SH2InterpreterInit(void);

static SceUID jit_memblock = -1;
static void* jit_memory = NULL;
static uint32_t* jit_ptr = NULL; // Puntero al código emitido
static int first_run = 1;

/* Tipo de memoria especial requerida para ejecutar código dinámico en PS Vita.
   En Homebrew típicamente corresponde a 0x0C20D060 (USER_RW_UNCACHE) cuando
   se parcha el kernel con kubridge para permitir ejecución. */
#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX 0x0C20D060
#endif

// --- JIT Emitter Core ---
// Función básica para escribir instrucciones de 32-bits en la memoria JIT
static void emit_instruction(uint32_t inst) {
    if (jit_ptr) {
        *jit_ptr++ = inst;
    }
}

// Tipo de puntero a función para saltar a nuestro bloque JIT
typedef void (*jit_block_t)(void);

static int sh2dyn_arm_init(void)
{
    vita_log("[SH2DynARM] Initializing ARM JIT core...\n");
    
    jit_memblock = sceKernelAllocMemBlock("Yabause_JIT_Cache",
                                          SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX,
                                          8 * 1024 * 1024, // 8MB de caché de instrucciones
                                          NULL);
                                          
    if (jit_memblock >= 0) {
        sceKernelGetMemBlockBase(jit_memblock, &jit_memory);
        jit_ptr = (uint32_t*)jit_memory;
        vita_log("[SH2DynARM] Allocated 8MB JIT memory at %p\n", jit_memory);
    } else {
        vita_log("[SH2DynARM] Failed to allocate JIT memory! Code: 0x%08X\n", jit_memblock);
    }
    
    first_run = 1;
    return SH2InterpreterInit();
}

static void sh2dyn_arm_deinit(void)
{
    vita_log("[SH2DynARM] De-initializing ARM JIT core...\n");
    if (jit_memblock >= 0) {
        sceKernelFreeMemBlock(jit_memblock);
        jit_memblock = -1;
        jit_memory = NULL;
        jit_ptr = NULL;
    }
}

static int sh2dyn_arm_reset(void) 
{ 
    jit_ptr = (uint32_t*)jit_memory; // Reset cache pointer
    first_run = 1;
    return 0; 
}

static void FASTCALL sh2dyn_arm_exec(SH2_struct *context, u32 cycles)
{
    if (!context) return;
    
    // Prueba de Emisión y Ejecución JIT en la primera corrida
    if (first_run && jit_memory) {
        vita_log("[SH2DynARM] Compiling first JIT block (Test)...\n");
        
        uint32_t* start_ptr = jit_ptr;
        
        // Escribir instrucción ARM: BX LR (Retornar / Branch and Exchange Link Register)
        // Hexadecimal en ARM: 0xE12FFF1E
        emit_instruction(0xE12FFF1E);
        
        // Limpiar caché de instrucciones (vital en ARM para no crashear)
        __clear_cache((char*)start_ptr, (char*)jit_ptr);
        
        vita_log("[SH2DynARM] Executing JIT block at %p...\n", start_ptr);
        jit_block_t test_block = (jit_block_t)start_ptr;
        
        // ¡Salto hacia la memoria dinámica!
        test_block(); 
        
        vita_log("[SH2DynARM] JIT block executed successfully!\n");
        first_run = 0;
    }
    
    // Fallback temporal al intérprete puro
    context->cycles += cycles;
}

static void sh2dyn_arm_write_notify(u32 start, u32 length) 
{ 
    // Invalida bloques cacheados si el juego modifica su propio código (Self-Modifying Code)
    (void)start; 
    (void)length; 
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
