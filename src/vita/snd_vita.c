/*  snd_vita.c — motor de sonido en hilo dedicado para PS Vita

    ARQUITECTURA (v01.04):

    La v01.03 ejecutaba 68K + timers SCSP + mezcla de 32 slots en el HILO
    PRINCIPAL, clockeado a 44100 Hz de tiempo real por GetAudioSpace desde
    ScspExec (que corre una vez por línea de video). Ese impuesto fijo de
    CPU hundía la emulación entera (juego a ~0 fps, música arrastrada).

    Ahora el subsistema de sonido completo corre en UN HILO PROPIO fijado
    a otro núcleo de la Vita (la consola tiene 3 núcleos para apps y el
    emulador solo usaba uno):

        hilo audio:  ScspThreadedStep(512 muestras)   ← timers + 68K + mezcla
                     sceAudioOutOutput(chunk)          ← bloquea ~11.6 ms
                     (el puerto de audio ES el reloj de 44100 Hz)

    - El hilo principal (SH2 + video) queda tan libre como en v01.02.
    - La música suena SIEMPRE a velocidad correcta, aunque el video vaya
      por debajo de 60 fps: el 68K avanza en tiempo real en su núcleo.
    - Si la generación tarda más que el búfer (escena extrema), el puerto
      hace un hueco de silencio y se recupera solo: nunca frena al juego.

    La sincronización con el hilo principal (MINT→SCU, M68KReset, realloc
    PAL/NTSC) está resuelta dentro de scsp.c — ver ScspThreadedStep.     */

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#include "../yabause.h"
#include "../scsp.h"
#include "snd_vita.h"

#define PLAYBACK_RATE 44100
#define CHUNK_SAMPLES 512            /* ~11.6 ms por chunk; múltiplo de 64 */

#ifndef SCE_KERNEL_CPU_MASK_USER_2
#define SCE_KERNEL_CPU_MASK_USER_2 (0x04 << 16)
#endif

/* ── Interfaz Yabause ─────────────────────────────────────────────── */

static int  vita_snd_init(void);
static void vita_snd_deinit(void);
static int  vita_snd_reset(void);
static int  vita_snd_change_video_format(int vertfreq);
static void vita_snd_update_audio(u32 *left, u32 *right, u32 num_samples);
static u32  vita_snd_get_audio_space(void);
static void vita_snd_mute(void);
static void vita_snd_unmute(void);
static void vita_snd_set_volume(int volume);

SoundInterface_struct SNDVita = {
    SNDCORE_VITA,
    "Vita Sound Interface (threaded)",
    vita_snd_init,
    vita_snd_deinit,
    vita_snd_reset,
    vita_snd_change_video_format,
    vita_snd_update_audio,
    vita_snd_get_audio_space,
    vita_snd_mute,
    vita_snd_unmute,
    vita_snd_set_volume,
};

/* ── Estado ───────────────────────────────────────────────────────── */

static int          port = -1;
static SceUID       thread_uid = -1;
static volatile int stop_flag = 0;
static volatile int engine_on = 0;   /* 1: generar audio real (modo threaded) */

static int          muted = 0;
static int          cur_volume = 100;

static s16 chunk_buf[CHUNK_SAMPLES * 2];
static s16 silence[CHUNK_SAMPLES * 2];

static void apply_volume(void)
{
    if (port < 0) return;
    int v = muted ? 0 : (SCE_AUDIO_VOLUME_0DB * cur_volume + 50) / 100;
    if (v > SCE_AUDIO_VOLUME_0DB) v = SCE_AUDIO_VOLUME_0DB;
    if (v < 0) v = 0;
    int vols[2] = { v, v };
    sceAudioOutSetVolume(port,
        (SceAudioOutChannelFlag)(SCE_AUDIO_VOLUME_FLAG_L_CH |
                                 SCE_AUDIO_VOLUME_FLAG_R_CH), vols);
}

/* ── Hilo del motor de audio ──────────────────────────────────────── */

static int audio_engine_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!stop_flag)
    {
        if (engine_on)
        {
            /* Genera 512 muestras (timers + 68K + mezcla, en ESTE núcleo)
               y las entrega; sceAudioOutOutput bloquea hasta que el chunk
               anterior terminó: cadencia exacta de 44100 Hz. */
            ScspThreadedStep(chunk_buf, CHUNK_SAMPLES);
            sceAudioOutOutput(port, chunk_buf);
        }
        else
        {
            /* Antes de activar el motor (menú, carga): mantener el puerto
               alimentado con silencio, misma cadencia, cero costo. */
            sceAudioOutOutput(port, silence);
        }
    }
    return 0;
}

/* Llamado por main.c tras YabauseInit, cuando ScspSetThreaded(1) ya está
   activo: a partir de aquí el hilo genera sonido real. */
void SNDVitaEnableEngine(void)
{
    engine_on = 1;
}

/* ── Implementación de la interfaz ────────────────────────────────── */

static int vita_snd_init(void)
{
    if (port >= 0)
        return 0;

    memset(silence, 0, sizeof(silence));
    memset(chunk_buf, 0, sizeof(chunk_buf));
    stop_flag = 0;
    engine_on = 0;

    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
                               CHUNK_SAMPLES, PLAYBACK_RATE,
                               SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        return -1;

    apply_volume();

    /* Prioridad alta y OTRO núcleo: el 68K+SCSP no roban ni un ciclo
       al hilo principal del emulador. */
    thread_uid = sceKernelCreateThread("yab_sndengine", audio_engine_thread,
                                       96, 0x20000, 0,
                                       SCE_KERNEL_CPU_MASK_USER_2, NULL);
    if (thread_uid < 0)
    {
        sceAudioOutReleasePort(port);
        port = -1;
        return -1;
    }
    sceKernelStartThread(thread_uid, 0, NULL);
    return 0;
}

static void vita_snd_deinit(void)
{
    if (port < 0) return;
    engine_on = 0;
    stop_flag = 1;
    if (thread_uid >= 0)
    {
        sceKernelWaitThreadEnd(thread_uid, NULL, NULL);
        sceKernelDeleteThread(thread_uid);
        thread_uid = -1;
    }
    sceAudioOutReleasePort(port);
    port = -1;
}

static int vita_snd_reset(void)
{
    return 0;
}

static int vita_snd_change_video_format(int vertfreq)
{
    (void)vertfreq;   /* SCSP siempre entrega 44100 Hz */
    return 0;
}

/* Con el motor threaded, la ruta clásica ScspExec→UpdateAudio del hilo
   principal queda desactivada (ScspExec retorna temprano). Estas dos
   funciones quedan inertes por si algo las llama antes de activar. */
static void vita_snd_update_audio(u32 *left, u32 *right, u32 num_samples)
{
    (void)left; (void)right; (void)num_samples;
}

static u32 vita_snd_get_audio_space(void)
{
    return 0;
}

static void vita_snd_mute(void)   { muted = 1; apply_volume(); }
static void vita_snd_unmute(void) { muted = 0; apply_volume(); }

static void vita_snd_set_volume(int volume)
{
    if (volume < 0)   volume = 0;
    if (volume > 100) volume = 100;
    cur_volume = volume;
    apply_volume();
}
