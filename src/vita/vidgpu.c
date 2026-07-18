#include <string.h>
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include "yabause.h"
#include "vdp1.h"
#include "vidsoft.h"

#define VIDCORE_GPU_ID 4

extern VideoInterface_struct VIDSoft;

extern int VIDSoftInit(void);
extern void VIDSoftDeInit(void);
extern void VIDSoftResize(unsigned int, unsigned int, int);
extern int VIDSoftIsFullscreen(void);
extern int VIDSoftVdp1Reset(void);
extern void VIDSoftVdp1DrawStart(void);
extern void VIDSoftVdp1DrawEnd(void);
extern void VIDSoftVdp1NormalSpriteDraw(void);
extern void VIDSoftVdp1ScaledSpriteDraw(void);
extern void VIDSoftVdp1DistortedSpriteDraw(void);
extern void VIDSoftVdp1PolygonDraw(void);
extern void VIDSoftVdp1PolylineDraw(void);
extern void VIDSoftVdp1LineDraw(void);
extern void VIDSoftVdp1UserClipping(void);
extern void VIDSoftVdp1SystemClipping(void);
extern void VIDSoftVdp1LocalCoordinate(void);
extern int VIDSoftVdp2Reset(void);
extern void VIDSoftVdp2DrawStart(void);
extern void VIDSoftVdp2DrawScreens(void);
extern void VIDSoftVdp2SetResolution(u16);
extern void FASTCALL VIDSoftVdp2SetPriorityNBG0(int);
extern void FASTCALL VIDSoftVdp2SetPriorityNBG1(int);
extern void FASTCALL VIDSoftVdp2SetPriorityNBG2(int);
extern void FASTCALL VIDSoftVdp2SetPriorityNBG3(int);
extern void FASTCALL VIDSoftVdp2SetPriorityRBG0(int);
extern void VIDSoftVdp2Composite(void);
extern void VIDSoftOnScreenDebugMessage(char *string, ...);
extern void VIDSoftGetGlSize(int *width, int *height);
extern int vita_log(const char *fmt, ...);

/* Lightweight frame timing */
#define TICK() sceKernelGetProcessTimeWide()
static SceUInt64 acc_composite, acc_upload, acc_display;
static int timing_frame_count;

static vita2d_texture *gpu_display_tex = NULL;
static int gpu_tex_w = 0, gpu_tex_h = 0;

/* ── Auto-frameskip real ───────────────────────────────────────────────
   El "frameskip" anterior no saltaba nada: vidsoft renderizaba todas las
   capas y se componía/presentaba cada frame igual. Aquí la decisión se
   toma al inicio del frame (Vdp2DrawStart) contra un deadline de 60 fps
   (50 en PAL) de reloj real:

   - A tiempo → renderizar y presentar (el vsync de vita2d marca el paso).
   - Con retraso de más de medio frame → saltar VIDSoftVdp2DrawScreens +
     Composite + subida a GPU + present + espera de vsync (lo caro),
     hasta FS_MAX_CONSEC frames seguidos. El VDP1 se sigue ejecutando
     siempre (hay juegos que leen su framebuffer).

   Resultado: cuando la emulación va sobrada no se salta nada, y cuando
   una escena pesa, el juego mantiene su velocidad sacrificando frames
   dibujados — que es lo que se nota como "más fps" en la mano.        */

static int fs_auto = 1;
static int fs_fixed = 0;
static int fs_counter = 0;
static int fs_consec = 0;
static int fs_skip_now = 0;
static int fs_skipped_total = 0;
static SceUInt64 fs_deadline = 0;
#define FS_MAX_CONSEC 4

void VIDGPUConfigureFrameSkip(int auto_on, int fixed)
{
    fs_auto  = auto_on ? 1 : 0;
    fs_fixed = fixed < 0 ? 0 : (fixed > 4 ? 4 : fixed);
    fs_counter = fs_consec = fs_skip_now = 0;
    fs_deadline = 0;
}

static int VIDGPUInit(void)
{
    vita2d_init_advanced(0x800000);

    /* Clear both front/back buffers to eliminate menu ghosting */
    for (int i = 0; i < 2; i++)
    {
        vita2d_start_drawing();
        vita2d_clear_screen();
        vita2d_end_drawing();
        vita2d_swap_buffers();
    }

    gpu_display_tex = NULL;
    gpu_tex_w = 0;
    gpu_tex_h = 0;
    acc_composite = acc_upload = acc_display = 0;
    timing_frame_count = 0;
    return VIDSoftInit();
}

static void VIDGPUDeInit(void)
{
    if (gpu_display_tex)
    {
        vita2d_free_texture(gpu_display_tex);
        gpu_display_tex = NULL;
    }
    VIDSoftDeInit();
    vita2d_fini();
}

static void GPUYuiSwapBuffers(void)
{
    int srcw = vdp2width;
    int srch = vdp2height;
    if (srcw <= 0 || srch <= 0 || !dispbuffer)
        return;

    SceUInt64 t0 = TICK();

    if (gpu_tex_w != srcw || gpu_tex_h != srch)
    {
        if (gpu_display_tex)
            vita2d_free_texture(gpu_display_tex);
        /* dispbuffer (vidsoft) empaqueta 0xAABBGGRR: R en el byte bajo
           (ver COLSAT2YAB32/Vdp2ColorRamGetColor). En memoria little-endian
           los bytes quedan R,G,B,A == A8B8G8R8. Usar A8R8G8B8 aquí
           intercambiaba rojo y azul (tonalidades raras). */
        gpu_display_tex = vita2d_create_empty_texture_format(srcw, srch,
            SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
        gpu_tex_w = srcw;
        gpu_tex_h = srch;
        if (!gpu_display_tex) { gpu_tex_w = gpu_tex_h = 0; return; }
        vita2d_texture_set_filters(gpu_display_tex,
            SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    }

    uint32_t *tp = (uint32_t *)vita2d_texture_get_datap(gpu_display_tex);
    uint32_t stride = vita2d_texture_get_stride(gpu_display_tex) / 4;
    int n = srcw * srch;

    /* La textura ya coincide byte a byte con dispbuffer: copia directa. */
    if (stride == (uint32_t)srcw)
    {
        memcpy(tp, dispbuffer, (size_t)n * sizeof(uint32_t));
    }
    else
    {
        for (int y = 0; y < srch; y++)
        {
            uint32_t *row = tp + y * stride;
            u32 *src = dispbuffer + y * srcw;
            for (int x = 0; x < srcw; x++)
                row[x] = src[x];
        }
    }

    acc_upload += TICK() - t0;

    t0 = TICK();
    vita2d_start_drawing();
    vita2d_clear_screen();
    /* Escalado proporcional a la altura máxima de la Vita (544 px),
       centrado horizontal, sin estirar (misma escala en X e Y).
       El filtro POINT (arriba) mantiene los píxeles nítidos, sin blur.
       Toda resolución del Saturn (320x224...704x480) cabe a lo ancho. */
    {
        float scale = 544.0f / (float)srch;
        float drww  = (float)srcw * scale;
        float offx  = (960.0f - drww) * 0.5f;
        if (offx < 0.0f) offx = 0.0f;
        vita2d_draw_texture_scale(gpu_display_tex, offx, 0.0f, scale, scale);
    }
    vita2d_end_drawing();
    vita2d_swap_buffers();
    acc_display += TICK() - t0;
}

void VIDGPUVdp2LogTiming(void)
{
    if (timing_frame_count > 0)
    {
        vita_log("  GPU timing: composite=%lldus upload=%lldus display=%lldus frames=%d skipped=%d\n",
            acc_composite, acc_upload, acc_display, timing_frame_count, fs_skipped_total);
    }
    acc_composite = acc_upload = acc_display = 0;
    timing_frame_count = 0;
    fs_skipped_total = 0;
}

/* Decide al inicio de cada frame si este se dibuja o se salta. */
static void VIDGPUVdp2DrawStart(void)
{
    SceUInt64 now = TICK();
    SceUInt64 period = yabsys.IsPal ? 20000 : 16667;

    if (fs_deadline == 0)
        fs_deadline = now;

    fs_skip_now = 0;
    if (fs_auto)
    {
        if ((s64)(now - fs_deadline) > (s64)(period / 2) &&
            fs_consec < FS_MAX_CONSEC)
            fs_skip_now = 1;
    }
    else if (fs_fixed > 0)
    {
        fs_skip_now = (fs_counter != 0);
        fs_counter = (fs_counter + 1) % (fs_fixed + 1);
    }

    if (fs_skip_now) { fs_consec++; fs_skipped_total++; }
    else               fs_consec = 0;

    fs_deadline += period;
    /* Resincronizar tras pausas largas (cargas de CD, menús, etc.) */
    if ((s64)(now - fs_deadline) > (s64)(8 * period))
        fs_deadline = now + period;

    VIDSoftVdp2DrawStart();
}

/* En frames saltados no se renderizan las capas del VDP2 (lo más caro). */
static void VIDGPUVdp2DrawScreens(void)
{
    if (fs_skip_now)
        return;
    VIDSoftVdp2DrawScreens();
}

static void VIDGPUVdp2DrawEnd(void)
{
    if (!fs_skip_now)
    {
        SceUInt64 t0 = TICK();
        VIDSoftVdp2Composite();
        acc_composite += TICK() - t0;

        GPUYuiSwapBuffers();
    }
    timing_frame_count++;
}

VideoInterface_struct VIDGPU = {
    VIDCORE_GPU_ID,
    "GPU Video Interface",
    VIDGPUInit,
    VIDGPUDeInit,
    VIDSoftResize,
    VIDSoftIsFullscreen,
    VIDSoftVdp1Reset,
    VIDSoftVdp1DrawStart,
    VIDSoftVdp1DrawEnd,
    VIDSoftVdp1NormalSpriteDraw,
    VIDSoftVdp1ScaledSpriteDraw,
    VIDSoftVdp1DistortedSpriteDraw,
    VIDSoftVdp1PolygonDraw,
    VIDSoftVdp1PolylineDraw,
    VIDSoftVdp1LineDraw,
    VIDSoftVdp1UserClipping,
    VIDSoftVdp1SystemClipping,
    VIDSoftVdp1LocalCoordinate,
    VIDSoftVdp2Reset,
    VIDGPUVdp2DrawStart,
    VIDGPUVdp2DrawEnd,
    VIDGPUVdp2DrawScreens,
    VIDSoftVdp2SetResolution,
    VIDSoftVdp2SetPriorityNBG0,
    VIDSoftVdp2SetPriorityNBG1,
    VIDSoftVdp2SetPriorityNBG2,
    VIDSoftVdp2SetPriorityNBG3,
    VIDSoftVdp2SetPriorityRBG0,
    VIDSoftOnScreenDebugMessage,
    VIDSoftGetGlSize,
};