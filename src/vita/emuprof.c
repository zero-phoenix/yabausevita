/* emuprof.c — acumuladores y volcado al log. Ver emuprof.h para el diseño. */
#include <stdio.h>
#include "emuprof.h"

#if EMUPROF_ENABLE
unsigned long long emuprof_acc[EMUPROF_SLOT_COUNT];
unsigned long long emuprof_slot_t0[EMUPROF_SLOT_COUNT];

extern int vita_log(const char *fmt, ...);

void EMUPROFLog(void)
{
    vita_log("  EMU: msh2=%lluus ssh2=%lluus scu=%lluus scsp=%lluus scsp_th=%lluus m68k=%lluus hblank=%lluus vdp=%lluus cdb=%lluus smpc=%lluus\n",
             emuprof_acc[EMUPROF_MSH2],   emuprof_acc[EMUPROF_SSH2],
             emuprof_acc[EMUPROF_SCU],    emuprof_acc[EMUPROF_SCSP],
             emuprof_acc[EMUPROF_SCSP_TH],emuprof_acc[EMUPROF_M68K],
             emuprof_acc[EMUPROF_HBLANK], emuprof_acc[EMUPROF_VDP],
             emuprof_acc[EMUPROF_CDB],    emuprof_acc[EMUPROF_SMPC]);
}

void EMUPROFReset(void)
{
    for (int i = 0; i < EMUPROF_SLOT_COUNT; i++)
        emuprof_acc[i] = 0;
}
#else
void EMUPROFLog(void) {}
void EMUPROFReset(void) {}
#endif
