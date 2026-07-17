/*  snd_vita.c — backend de sonido PS Vita (sceAudioOut) para Yabause

    Basado en el patrón probado de src/psp/psp-sound.c (Andrew Church):
    doble búfer con handshake write_ready entre el hilo del emulador
    (productor: SCSP entrega chunks vía UpdateAudio) y un hilo de
    reproducción (consumidor: sceAudioOutOutput, que bloquea hasta que
    el búfer anterior terminó — el propio puerto hace de reloj).

    SCSP genera 44100 Hz estéreo con muestras s32 por canal; aquí se
    saturan a s16 y se intercalan L,R como espera sceAudioOut.

    Mientras suena el búfer A, el emulador tiene ~11.6 ms para llenar
    el búfer B: GetAudioSpace() devuelve el hueco restante del búfer
    en escritura, y scsp.c entrega en trozos parciales hasta llenarlo. */

#include <psp2/audioout.h>
#include <psp2/kernel/threadmgr.h>
#include <string.h>

#include "../yabause.h"
#include "../scsp.h"
#include "snd_vita.h"

#define PLAYBACK_RATE 44100
#define BUFFER_SIZE   512   /* muestras por búfer (~11.6 ms de latencia) */

/* ── Interfaz ──────────────────────────────────────────────────────── */

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
    "Vita Sound Interface",
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

/* ── Estado ────────────────────────────────────────────────────────── */

static int          port = -1;
static SceUID       thread_uid = -1;
static volatile int stop_flag = 0;

/* Handshake (un productor / un consumidor, como en psp-sound.c):
   write_ready=1 → el emulador puede escribir en buffers[next_free].
   write_ready=0 → hay un búfer completo listo para reproducirse.   */
static volatile int write_ready = 1;
static volatile int next_free = 0;
static int          saved_samples = 0;   /* muestras ya escritas en el búfer actual */

static int          muted = 0;
static int          cur_volume = 100;

static s16 buffers[2][BUFFER_SIZE * 2];  /* estéreo intercalado L,R */

static inline s16 clamp16(s32 v)
{
    if (v >  0x7FFF) return  0x7FFF;
    if (v < -0x8000) return -0x8000;
    return (s16)v;
}

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

/* ── Hilo de reproducción ─────────────────────────────────────────── */

static int playback_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    while (!stop_flag)
    {
        if (write_ready)
        {
            /* El emulador aún no completó el búfer: esperar un poco.
               (Si va lento hay un hueco de silencio natural, sin cuelgue.) */
            sceKernelDelayThread(200);
            continue;
        }
        /* Bloquea hasta que el búfer anterior terminó de sonar:
           marca el ritmo a 44100 Hz sin busy-wait. */
        sceAudioOutOutput(port, buffers[next_free]);
        next_free ^= 1;
        write_ready = 1;
    }
    return 0;
}

/* ── Implementación de la interfaz ────────────────────────────────── */

static int vita_snd_init(void)
{
    if (port >= 0)
        return 0;   /* ya inicializado */

    memset(buffers, 0, sizeof(buffers));
    saved_samples = 0;
    next_free = 0;
    write_ready = 1;
    stop_flag = 0;

    port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM,
                               BUFFER_SIZE, PLAYBACK_RATE,
                               SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        return -1;

    apply_volume();

    thread_uid = sceKernelCreateThread("yab_snd", playback_thread,
                                       96,        /* prioridad alta (audio) */
                                       0x10000, 0, 0, NULL);
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
    saved_samples = 0;
    return 0;
}

static int vita_snd_change_video_format(int vertfreq)
{
    (void)vertfreq;   /* SCSP siempre entrega 44100 Hz */
    return 0;
}

static void vita_snd_update_audio(u32 *left, u32 *right, u32 num_samples)
{
    if (!left || !right || !write_ready ||
        num_samples == 0 || num_samples > (u32)(BUFFER_SIZE - saved_samples))
        return;

    s32 *in_l = (s32 *)left;
    s32 *in_r = (s32 *)right;
    s16 *out  = &buffers[next_free][saved_samples * 2];

    for (u32 i = 0; i < num_samples; i++)
    {
        *out++ = clamp16(*in_l++);
        *out++ = clamp16(*in_r++);
    }
    saved_samples += num_samples;

    if (saved_samples >= BUFFER_SIZE)
    {
        saved_samples = 0;
        write_ready = 0;   /* búfer completo → el hilo lo reproduce */
    }
}

static u32 vita_snd_get_audio_space(void)
{
    return write_ready ? (u32)(BUFFER_SIZE - saved_samples) : 0;
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
