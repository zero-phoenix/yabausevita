#include <string.h>
#include <stdint.h>
#include <stdio.h>
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include "yabause.h"
#include "vdp1.h"
#include "vdp2.h"
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
extern void VIDSoftVdp1SwapFrameBuffer(void);
extern void VIDSoftVdp2SwapCompositeBuffer(void);
extern int vidsoft_external_vdp1_swap;
extern int vita_log(const char *fmt, ...);

/* Lightweight frame timing */
#define TICK() sceKernelGetProcessTimeWide()
static SceUInt64 acc_composite, acc_upload, acc_display;
static int timing_frame_count;

/* ── FPS del ROM en pantalla (show_fps) ───────────────────────────────
   Son los FPS de la EMULACION Saturn (los que main.c cuenta por
   YabauseExec), no los de la app Vita que muestra Vita3K. Tira de
   glifos 8x8 con solo los 16 caracteres necesarios; se crea una vez
   y se dibuja por partes al presentar cada frame. */
extern int g_show_fps;        /* main.c, desde config.cfg show_fps */
float g_game_fps = 0.0f;      /* la escribe main.c cada segundo */

static const char FPS_CHARS[17] = "0123456789.FPS: ";
static const unsigned char fps_font[16][8] = {
    {0x3C,0x66,0x66,0x6E,0x66,0x66,0x3C,0x00}, /* 0 */
    {0x18,0x38,0x18,0x18,0x18,0x18,0x7E,0x00}, /* 1 */
    {0x3C,0x66,0x06,0x1C,0x30,0x60,0x7E,0x00}, /* 2 */
    {0x3C,0x66,0x06,0x1C,0x06,0x66,0x3C,0x00}, /* 3 */
    {0x0E,0x1E,0x36,0x66,0x7F,0x06,0x06,0x00}, /* 4 */
    {0x7E,0x60,0x7C,0x06,0x06,0x66,0x3C,0x00}, /* 5 */
    {0x1C,0x30,0x60,0x7C,0x66,0x66,0x3C,0x00}, /* 6 */
    {0x7E,0x66,0x0C,0x18,0x18,0x18,0x18,0x00}, /* 7 */
    {0x3C,0x66,0x66,0x3C,0x66,0x66,0x3C,0x00}, /* 8 */
    {0x3C,0x66,0x66,0x3E,0x06,0x0C,0x38,0x00}, /* 9 */
    {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00}, /* . */
    {0x7E,0x60,0x60,0x7C,0x60,0x60,0x60,0x00}, /* F */
    {0x7C,0x66,0x66,0x7C,0x60,0x60,0x60,0x00}, /* P */
    {0x3C,0x66,0x60,0x3C,0x06,0x66,0x3C,0x00}, /* S */
    {0x00,0x18,0x18,0x00,0x00,0x18,0x18,0x00}, /* : */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* espacio */
};
static vita2d_texture *fps_font_tex = NULL;

static void fps_font_init(void)
{
    fps_font_tex = vita2d_create_empty_texture(16 * 8, 8);
    if (!fps_font_tex) return;
    unsigned int *px = (unsigned int *)vita2d_texture_get_datap(fps_font_tex);
    unsigned int stride = vita2d_texture_get_stride(fps_font_tex) / 4;
    for (int c = 0; c < 16; c++)
        for (int row = 0; row < 8; row++)
            for (int col = 0; col < 8; col++)
                px[row * stride + c * 8 + col] =
                    (fps_font[c][row] & (0x80 >> col))
                        ? RGBA8(255, 255, 255, 255) : 0;
}

static void draw_fps_overlay(void)
{
    if (!g_show_fps || !fps_font_tex) return;
    char buf[20];
    snprintf(buf, sizeof(buf), "FPS: %.1f", g_game_fps);
    const float sc = 2.0f;
    float x = 10.0f;
    for (char *p = buf; *p; p++)
    {
        const char *q = strchr(FPS_CHARS, *p);
        if (q && *p != ' ')
            vita2d_draw_texture_part_scale(fps_font_tex, x, 10.0f,
                                           (int)(q - FPS_CHARS) * 8, 0,
                                           8, 8, sc, sc);
        x += 8.0f * sc;
    }
}

static vita2d_texture *gpu_display_tex = NULL;
static int gpu_tex_w = 0, gpu_tex_h = 0;

/* ── Hilo de render en el núcleo 1 (v01.05) ────────────────────────────
   El frameskip ahora lo gobierna el mecanismo INTERNO de vdp2.c
   (EnableAutoFrameSkip): mide ticks reales, salta frames completos
   (incluido el render del VDP1, con Vdp1NoDraw manteniendo la semántica
   de EDSR que los juegos esperan) y limita la velocidad cuando el juego
   va sobrado. Los hooks de este core ya solo se llaman en frames NO
   saltados.

   De los frames que sí se dibujan, la mitad cara de la salida —
   Composite (mezcla VDP1+VDP2 por píxel) + subida a GPU + present +
   ESPERA DE VSYNC — corre en un hilo propio fijado al núcleo 1:

     principal (núcleo 0): SH2 + SCU + VDP1 + capas VDP2  → handoff
     render    (núcleo 1): Composite + upload + draw + vsync
     audio     (núcleo 2): 68K + timers SCSP + mezcla + CDDA

   Handoff sin bloqueo: al terminar de dibujar un frame, si el hilo de
   render está libre se intercambian los búferes (ping-pong del VDP2 y
   swap del VDP1 — ambos desde el hilo principal, sin carreras) y se le
   da la señal; si está ocupado, el frame simplemente no se presenta.
   El hilo principal JAMÁS espera al vsync ni a la GPU.                */

#ifndef SCE_KERNEL_CPU_MASK_USER_1
#define SCE_KERNEL_CPU_MASK_USER_1 (0x02 << 16)
#endif

static SceUID render_thread_uid = -1;
static SceUID render_sema = -1;
static volatile int render_stop = 0;
static volatile int render_busy = 0;
static int presented_frames = 0;
static int dropped_presents = 0;

static void GPUYuiSwapBuffers(void);

static int render_thread(SceSize args, void *argp)
{
    (void)args; (void)argp;
    for (;;)
    {
        sceKernelWaitSema(render_sema, 1, NULL);
        if (render_stop)
            break;

        SceUInt64 t0 = TICK();
        VIDSoftVdp2Composite();
        acc_composite += TICK() - t0;

        GPUYuiSwapBuffers();
        presented_frames++;

        render_busy = 0;
    }
    return 0;
}

static int VIDGPUInit(void)
{
    vita2d_init_advanced(0x800000);
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));

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
    fps_font_init();
    timing_frame_count = 0;
    presented_frames = dropped_presents = 0;

    if (VIDSoftInit() != 0)
        return -1;

    /* Hilo de render en el núcleo 1. El swap del VDP1 pasa a hacerse en
       el handoff (hilo principal), no dentro del composite. */
    render_stop = 0;
    render_busy = 0;
    render_sema = sceKernelCreateSema("yab_render_sema", 0, 0, 1, NULL);
    if (render_sema >= 0)
    {
        render_thread_uid = sceKernelCreateThread("yab_render", render_thread,
                                                  96, 0x10000, 0,
                                                  SCE_KERNEL_CPU_MASK_USER_1,
                                                  NULL);
        if (render_thread_uid >= 0)
        {
            vidsoft_external_vdp1_swap = 1;
            sceKernelStartThread(render_thread_uid, 0, NULL);
        }
    }
    /* Si el hilo no pudo crearse, render_thread_uid < 0 y DrawEnd hará
       todo en el hilo principal (modo degradado). */
    return 0;
}

static void VIDGPUDeInit(void)
{
    /* Parar el hilo de render antes de liberar recursos que usa */
    if (render_thread_uid >= 0)
    {
        render_stop = 1;
        sceKernelSignalSema(render_sema, 1);
        sceKernelWaitThreadEnd(render_thread_uid, NULL, NULL);
        sceKernelDeleteThread(render_thread_uid);
        render_thread_uid = -1;
    }
    if (render_sema >= 0)
    {
        sceKernelDeleteSema(render_sema);
        render_sema = -1;
    }
    vidsoft_external_vdp1_swap = 0;

    if (gpu_display_tex)
    {
        vita2d_free_texture(gpu_display_tex);
        gpu_display_tex = NULL;
    }
    if (fps_font_tex)
    {
        vita2d_free_texture(fps_font_tex);
        fps_font_tex = NULL;
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

    /* Sonda (Ronda 1): por que llega negro. Cada 300 presents cuenta
       pixeles no-negros de dispbuffer y vuelca los registros que deciden
       si hay salida de video: TVMD (bit15=DISP) y el disptoggle del VDP1. */
    {
        static int probe = 0;
        if (++probe >= 300)
        {
            probe = 0;
            int nonzero = 0;
            int total = srcw * srch;
            for (int i = 0; i < total; i += 11)
                if (dispbuffer[i] & 0x00FFFFFF) nonzero++;
            vita_log("PROBE: nonzero=%d/%d tvmd=%04x vdp1_toggle=%d\n",
                     nonzero, total / 11,
                     (unsigned)(Vdp2Regs ? Vdp2Regs->TVMD : 0xFFFF),
                     Vdp1Regs ? (int)Vdp1Regs->disptoggle : -1);
        }
    }

    if (!gpu_display_tex)
    {
        /* Occam's razor: allocate a single maximum-size texture to prevent VRAM fragmentation
           and memory leaks caused by repeatedly allocating and freeing textures when the
           emulated resolution changes. Saturn max res is 704x512. */
        gpu_display_tex = vita2d_create_empty_texture_format(1024, 512,
            SCE_GXM_TEXTURE_FORMAT_A8B8G8R8);
        if (!gpu_display_tex) { gpu_tex_w = gpu_tex_h = 0; return; }
        vita2d_texture_set_filters(gpu_display_tex,
            SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    }
    
    gpu_tex_w = srcw;
    gpu_tex_h = srch;

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
       centrado horizontal, sin estirar (misma escala en X e Y). */
    {
        float scale = 544.0f / (float)srch;
        float drww  = (float)srcw * scale;
        float offx  = (960.0f - drww) * 0.5f;
        if (offx < 0.0f) offx = 0.0f;
        vita2d_draw_texture_part_scale(gpu_display_tex, offx, 0.0f, 0, 0, srcw, srch, scale, scale);
    }
    draw_fps_overlay();
    vita2d_end_drawing();
    vita2d_swap_buffers();
    acc_display += TICK() - t0;
}

void VIDGPUVdp2LogTiming(void)
{
    if (timing_frame_count > 0)
    {
        vita_log("  GPU: drawn=%d presented=%d dropped=%d composite=%lldus upload=%lldus display=%lldus\n",
            timing_frame_count, presented_frames, dropped_presents,
            acc_composite, acc_upload, acc_display);
    }
    acc_composite = acc_upload = acc_display = 0;
    timing_frame_count = 0;
    presented_frames = 0;
    dropped_presents = 0;
}

/* Fin de frame dibujado (los saltados por el auto-frameskip interno de
   vdp2.c ni siquiera llegan aquí). Handoff al hilo de render sin
   bloquear jamás el hilo principal. */
static void VIDGPUVdp2DrawEnd(void)
{
    timing_frame_count++;

    if (render_thread_uid >= 0)
    {
        if (render_busy)
        {
            /* El render sigue con el frame anterior: este no se presenta.
               Los búferes NO se intercambian, así que el siguiente frame
               sobreescribe este sin carreras. */
            dropped_presents++;
            return;
        }
        /* Render libre: publicar el frame (swaps desde el hilo principal,
           sin carreras) y despertar al hilo del núcleo 1. */
        VIDSoftVdp1SwapFrameBuffer();
        VIDSoftVdp2SwapCompositeBuffer();
        render_busy = 1;
        sceKernelSignalSema(render_sema, 1);
        return;
    }

    /* Modo degradado (sin hilo de render): todo en el hilo principal.
       vidsoft_external_vdp1_swap quedó en 0: el composite hace su swap. */
    {
        SceUInt64 t0 = TICK();
        VIDSoftVdp2Composite();
        acc_composite += TICK() - t0;
        GPUYuiSwapBuffers();
        presented_frames++;
    }
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
    VIDSoftVdp2DrawStart,
    VIDGPUVdp2DrawEnd,
    VIDSoftVdp2DrawScreens,
    VIDSoftVdp2SetResolution,
    VIDSoftVdp2SetPriorityNBG0,
    VIDSoftVdp2SetPriorityNBG1,
    VIDSoftVdp2SetPriorityNBG2,
    VIDSoftVdp2SetPriorityNBG3,
    VIDSoftVdp2SetPriorityRBG0,
    VIDSoftOnScreenDebugMessage,
    VIDSoftGetGlSize,
};