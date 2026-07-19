#include <string.h>
#include <psp2/kernel/sysmem.h>
#include "sh2core.h"

#define SH2DYN_ARM_ID 4

extern int vita_log(const char *fmt, ...);
extern int SH2InterpreterInit(void);

static SceUID jit_memblock = -1;
static void* jit_memory = NULL;

/* Tipo de memoria especial requerida para ejecutar código dinámico en PS Vita.
   En Homebrew típicamente corresponde a 0x0C20D060 (USER_RW_UNCACHE) cuando
   se parcha el kernel con kubridge para permitir ejecución. */
#ifndef SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX 0x0C20D060
#endif

static int sh2dyn_arm_init(void)
{
    vita_log("[SH2DynARM] Initializing ARM JIT core...\n");
    
    jit_memblock = sceKernelAllocMemBlock("Yabause_JIT_Cache",
                                          SCE_KERNEL_MEMBLOCK_TYPE_USER_RWX,
                                          8 * 1024 * 1024, // 8MB de caché de instrucciones
                                          NULL);
                                          
    if (jit_memblock >= 0) {
        sceKernelGetMemBlockBase(jit_memblock, &jit_memory);
        vita_log("[SH2DynARM] Allocated 8MB JIT memory at %p\n", jit_memory);
    } else {
        vita_log("[SH2DynARM] Failed to allocate JIT memory! Code: 0x%08X\n", jit_memblock);
    }
    
    return SH2InterpreterInit();
}

static void sh2dyn_arm_deinit(void)
{
    vita_log("[SH2DynARM] De-initializing ARM JIT core...\n");
    if (jit_memblock >= 0) {
        sceKernelFreeMemBlock(jit_memblock);
        jit_memblock = -1;
        jit_memory = NULL;
    }
}

static int sh2dyn_arm_reset(void) 
{ 
    return 0; 
}

static void FASTCALL sh2dyn_arm_exec(SH2_struct *context, u32 cycles)
{
    if (!context) return;
    
    // [AQUÍ SE INSERTARÁ EL SALTO A LA MEMORIA CACHÉ JIT TRADUCIDA]
    // temporal: avanzar ciclos para no congelar el emulador si se selecciona.
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
