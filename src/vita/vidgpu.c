#include <string.h>
#include <vita2d.h>
#include <psp2/kernel/processmgr.h>
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
        gpu_display_tex = vita2d_create_empty_texture_format(srcw, srch,
            SCE_GXM_TEXTURE_FORMAT_A8R8G8B8);
        gpu_tex_w = srcw;
        gpu_tex_h = srch;
        if (!gpu_display_tex) { gpu_tex_w = gpu_tex_h = 0; return; }
        vita2d_texture_set_filters(gpu_display_tex,
            SCE_GXM_TEXTURE_FILTER_POINT, SCE_GXM_TEXTURE_FILTER_POINT);
    }

    uint32_t *tp = (uint32_t *)vita2d_texture_get_datap(gpu_display_tex);
    uint32_t stride = vita2d_texture_get_stride(gpu_display_tex) / 4;
    int n = srcw * srch;

    /* dispbuffer stores pixels as 0xAARRGGBB (little-endian: B,G,R,A).
       A8R8G8B8 expects same byte order — no conversion needed. */
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
    int offx = (960 - srcw) / 2, offy = (544 - srch) / 2;
    vita2d_draw_texture(gpu_display_tex, offx, offy);
    vita2d_end_drawing();
    vita2d_swap_buffers();
    acc_display += TICK() - t0;
}

void VIDGPUVdp2LogTiming(void)
{
    if (timing_frame_count > 0)
    {
        vita_log("  GPU timing: composite=%lldus upload=%lldus display=%lldus frames=%d\n",
            acc_composite, acc_upload, acc_display, timing_frame_count);
    }
    acc_composite = acc_upload = acc_display = 0;
    timing_frame_count = 0;
}

static void VIDGPUVdp2DrawEnd(void)
{
    SceUInt64 t0 = TICK();
    VIDSoftVdp2Composite();
    acc_composite += TICK() - t0;

    GPUYuiSwapBuffers();
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