/* emuprof — contadores ligeros por subsistema del lazo de emulación.
 *
 * Ronda 1 de la bitácora (R8): el 98,7 % del tiempo está en la emulación
 * y no había ni un contador. Esto rellena ese hueco con el mismo contrato
 * que el GPU timing de vidgpu.c: totales en µs por ventana de 5 s, que
 * main.c vuelca al log junto al FPS.
 *
 * Coste: una lectura de timer por START y otra por STOP. Con ~263 lazos
 * de línea por frame son ~3.700 lecturas/s a 20 FPS: en Vita real
 * ~0,3-0,5 % de sobrecoste, declarado en la bitácora. EMUPROF_ENABLE=0
 * compila todo a nada si algún día estorba.
 *
 * El SSH2 corre en hilo propio (ssh2_thread_func) y acumula en su slot
 * desde su hilo; MSH2 usa otro slot, así que nunca hay carrera sobre el
 * mismo acumulador. Sus µs NO son aditivos con los del hilo principal:
 * son solapamiento real de dos núcleos.
 */
#ifndef EMUPROF_H
#define EMUPROF_H

#define EMUPROF_ENABLE 1

#if EMUPROF_ENABLE && defined(__vita__)

#include <psp2/kernel/processmgr.h>

enum {
    EMUPROF_MSH2 = 0,   /* SH-2 maestro */
    EMUPROF_SSH2,       /* SH-2 esclavo (secuencial en NO_DECILINE; slot propio por si se reactiva el camino paralelo) */
    EMUPROF_SCU,        /* SCU + DMA */
    EMUPROF_SCSP,       /* ScspExec en el hilo de emulación */
    EMUPROF_SCSP_TH,    /* ScspThreadedStep: timers+68K+mezcla en el hilo de audio (paralelo real) */
    EMUPROF_M68K,       /* M68KExec del hilo principal (no-op con audio threaded) */
    EMUPROF_HBLANK,     /* Vdp2HBlankIN/OUT por línea */
    EMUPROF_VDP,        /* Vdp2VBlankOUT: render VDP1+VDP2 del frame */
    EMUPROF_CDB,        /* bloque de CD (Cs2Exec) */
    EMUPROF_SMPC,       /* SMPC */
    EMUPROF_SLOT_COUNT
};

extern unsigned long long emuprof_acc[EMUPROF_SLOT_COUNT];
extern unsigned long long emuprof_slot_t0[EMUPROF_SLOT_COUNT];

#define EMUPROF_START(s) \
    ((void)(emuprof_slot_t0[(s)] = (unsigned long long)sceKernelGetProcessTimeWide()))
#define EMUPROF_STOP(s) \
    ((void)(emuprof_acc[(s)] += (unsigned long long)sceKernelGetProcessTimeWide() - emuprof_slot_t0[(s)]))

#else /* fuera de Vita, o apagado: coste cero */

#define EMUPROF_START(s) ((void)0)
#define EMUPROF_STOP(s)  ((void)0)

#endif

void EMUPROFLog(void);   /* imprime "  EMU: ..." vía vita_log */
void EMUPROFReset(void); /* a cero tras volcar cada ventana */

#endif /* EMUPROF_H */
