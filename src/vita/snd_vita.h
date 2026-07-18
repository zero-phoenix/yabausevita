/*  snd_vita.h — backend de sonido PS Vita (sceAudioOut) para Yabause  */

#ifndef SND_VITA_H
#define SND_VITA_H

#include "../scsp.h"

#define SNDCORE_VITA 11

extern SoundInterface_struct SNDVita;

/* Activa la generación real de sonido en el hilo de audio dedicado.
   Llamar después de YabauseInit + ScspSetThreaded(1). */
void SNDVitaEnableEngine(void);

#endif /* SND_VITA_H */
