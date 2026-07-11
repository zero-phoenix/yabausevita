#include <string.h>
#include <vita2d.h>
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
extern void VIDSoftOnScreenDebugMessage(char *string, ...);
extern void VIDSoftGetGlSize(int *width, int *height);

static vita2d_texture *gpu_display_tex = NULL;
static int gpu_tex_w = 0, gpu_tex_h = 0;

static int VIDGPUInit(void)
{
    vita2d_init_advanced(0x800000);
    gpu_display_tex = NULL;
    gpu_tex_w = 0;
    gpu_tex_h = 0;
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

    if (gpu_tex_w != srcw || gpu_tex_h != srch)
    {
        if (gpu_display_tex)
            vita2d_free_texture(gpu_display_tex);
        gpu_display_tex = vita2d_create_empty_texture(srcw, srch);
        gpu_tex_w = srcw;
        gpu_tex_h = srch;
        if (!gpu_display_tex)
        {
            gpu_tex_w = 0;
            gpu_tex_h = 0;
            return;
        }
    }

    uint32_t *tex_pixels = (uint32_t *)vita2d_texture_get_datap(gpu_display_tex);
    uint32_t tex_stride = vita2d_texture_get_stride(gpu_display_tex);
    uint32_t tex_stride_pix = tex_stride / 4;

    if (tex_stride_pix == (uint32_t)srcw)
    {
        for (int i = 0; i < srcw * srch; i++)
            tex_pixels[i] = __builtin_bswap32(dispbuffer[i]);
    }
    else
    {
        for (int y = 0; y < srch; y++)
        {
            uint32_t *row = tex_pixels + y * tex_stride_pix;
            for (int x = 0; x < srcw; x++)
                row[x] = __builtin_bswap32(dispbuffer[y * srcw + x]);
        }
    }

    vita2d_start_drawing();
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));
    vita2d_clear_screen();

    float scale_x = 960.0f / (float)srcw;
    float scale_y = 544.0f / (float)srch;
    float scale = (scale_x < scale_y) ? scale_x : scale_y;
    float draw_w = srcw * scale;
    float draw_h = srch * scale;
    float draw_x = (960.0f - draw_w) / 2.0f;
    float draw_y = (544.0f - draw_h) / 2.0f;

    vita2d_draw_texture_scale(gpu_display_tex, draw_x, draw_y, scale, scale);

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

static void VIDGPUVdp2DrawEnd(void)
{
    VIDSoftVdp2Composite();
    GPUYuiSwapBuffers();
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
