#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <vita2d.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

#include "../yabause.h"
#include "../sh2core.h"
#include "../sh2int.h"
#include "../vidsoft.h"
#include "../peripheral.h"
#include "../cdbase.h"
#include "../cs2.h"
#include "../scsp.h"
#include "../m68kcore.h"
#include "../smpc.h"

#include "vita_menu.h"

#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544

static void *vita_fb = NULL;

int vita_log(const char *fmt, ...);

extern M68K_struct M68KDummy;
M68K_struct *M68KCoreList[] = { &M68KDummy, NULL };

extern SH2Interface_struct SH2Interpreter;
extern SH2Interface_struct SH2DebugInterpreter;
SH2Interface_struct *SH2CoreList[] = { &SH2Interpreter, &SH2DebugInterpreter, NULL };

extern PerInterface_struct PERDummy;
PerInterface_struct *PERCoreList[] = { &PERDummy, NULL };

extern CDInterface DummyCD;
extern CDInterface ISOCD;
CDInterface *CDCoreList[] = { &DummyCD, &ISOCD, NULL };

extern SoundInterface_struct SNDDummy;
SoundInterface_struct *SNDCoreList[] = { &SNDDummy, NULL };

extern VideoInterface_struct VIDSoft;
VideoInterface_struct *VIDCoreList[] = { &VIDSoft, NULL };

void YuiErrorMsg(const char *string)
{
    vita_log("[Yabause ERROR] %s\n", string);
}

void YuiSetVideoAttribute(int type, int val)
{
    (void)type; (void)val;
}

int YuiSetVideoMode(int width, int height, int bpp, int fullscreen)
{
    (void)width; (void)height; (void)bpp; (void)fullscreen;
    return 0;
}

void YuiSwapBuffers(void)
{
    extern u32 *dispbuffer;
    int srcw, srch;
    VIDSoftGetScreenSize(&srcw, &srch);

    if (dispbuffer == NULL || srcw <= 0 || srch <= 0 || vita_fb == NULL)
        return;

    int offx = (VITA_SCREEN_W - srcw) / 2;
    int offy = (VITA_SCREEN_H - srch) / 2;
    if (offx < 0) offx = 0;
    if (offy < 0) offy = 0;

    uint32_t *dst = (uint32_t *)vita_fb;
    for (int y = 0; y < srch && (y + offy) < VITA_SCREEN_H; y++)
    {
        uint32_t *dstrow = dst + (y + offy) * VITA_SCREEN_W + offx;
        u32 *srcrow = dispbuffer + y * srcw;
        int copyw = srcw;
        if (offx + copyw > VITA_SCREEN_W)
            copyw = VITA_SCREEN_W - offx;
        memcpy(dstrow, srcrow, copyw * sizeof(uint32_t));
    }

    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = vita_fb;
    fb.pitch = VITA_SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = VITA_SCREEN_W;
    fb.height = VITA_SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static FILE *g_logfile = NULL;

int vita_log(const char *fmt, ...)
{
    if (g_logfile == NULL) return -1;
    va_list args;
    va_start(args, fmt);
    int r = vfprintf(g_logfile, fmt, args);
    va_end(args);
    fflush(g_logfile);
    return r;
}

static void clear_framebuffer(uint32_t abgr)
{
    if (!vita_fb) return;
    uint32_t *px = (uint32_t *)vita_fb;
    for (int i = 0; i < VITA_SCREEN_W * VITA_SCREEN_H; i++)
        px[i] = abgr;

    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = vita_fb;
    fb.pitch = VITA_SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = VITA_SCREEN_W;
    fb.height = VITA_SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);
}

static int map_region(int vmenu_region)
{
    switch (vmenu_region)
    {
        case VMENU_REGION_JP: return REGION_JAPAN;
        case VMENU_REGION_US: return REGION_NORTHAMERICA;
        case VMENU_REGION_EU: return REGION_EUROPE;
        default:              return REGION_AUTODETECT;
    }
}

static void draw_splash(void)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
    vita2d_draw_rectangle(0, 0, 960, 544, RGBA8(6, 6, 14, 255));
    vita2d_draw_rectangle(0, 220, 960, 110, RGBA8(0, 85, 255, 255));
    vita2d_end_drawing();
    vita2d_swap_buffers();
    sceDisplayWaitVblankStart();
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    g_logfile = fopen("ux0:data/yabausevita_log.txt", "w");
    vita_log("YabauseVita starting (vita_menu UI)\n");

    vita_fb = malloc((size_t)VITA_SCREEN_W * VITA_SCREEN_H * sizeof(uint32_t));
    if (vita_fb == NULL)
    {
        vita_log("FATAL: could not allocate framebuffer\n");
        sceKernelExitProcess(0);
        return 0;
    }

    vita_log("Initializing menu...\n");
    if (vita_menu_init() != 0)
    {
        vita_log("FATAL: vita_menu_init failed\n");
        sceKernelExitProcess(0);
        return 0;
    }

    draw_splash();

    VitaMenuConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_bios = 1;
    cfg.video_filter = VMENU_FILTER_BILINEAR;
    cfg.aspect_ratio = VMENU_ASPECT_4_3;
    cfg.vsync = 1;
    cfg.audio_enabled = 1;
    cfg.audio_volume = 80;
    cfg.cpu_mode = VMENU_CPU_INTERP;
    cfg.frame_skip = 0;
    cfg.sh2_sync = 1;
    cfg.show_fps = 0;
    cfg.borderless = 0;

    vita_log("Entering menu...\n");
    int ret = vita_menu_run(&cfg, NULL);

    if (ret != 0 || cfg.rom_path[0] == '\0')
    {
        vita_log("User exited without launching. Quitting.\n");
        vita_menu_cleanup();
        if (g_logfile) fclose(g_logfile);
        sceKernelExitProcess(0);
        return 0;
    }

    vita_log("ROM: %s\n", cfg.rom_path);
    vita_log("BIOS: %s\n", cfg.bios_path[0] ? cfg.bios_path : "(HLE)");

    vita_menu_cleanup();
    clear_framebuffer(0xFF100820);

    yabauseinit_struct yinit;
    memset(&yinit, 0, sizeof(yinit));
    yinit.percoretype   = PERCORE_DUMMY;
    yinit.sh2coretype   = SH2CORE_INTERPRETER;
    yinit.vidcoretype   = VIDCORE_SOFT;
    yinit.sndcoretype   = SNDCORE_DUMMY;
    yinit.m68kcoretype  = 0;
    yinit.cdcoretype    = CDCORE_ISO;
    yinit.cdpath        = cfg.rom_path;
    yinit.biospath      = cfg.bios_path[0] ? cfg.bios_path : NULL;
    yinit.carttype      = 0;
    yinit.regionid      = (u8)map_region(cfg.rom_region);
    yinit.buppath       = NULL;
    yinit.mpegpath      = NULL;
    yinit.cartpath      = NULL;
    yinit.netlinksetting = NULL;
    yinit.flags         = 0;
    yinit.frameskip     = cfg.frame_skip;

    vita_log("Calling YabauseInit (cdcore=CDCORE_ISO, cdpath=%s, biospath=%s, region=%d)...\n",
             yinit.cdpath ? yinit.cdpath : "(null)",
             yinit.biospath ? yinit.biospath : "(HLE)",
             yinit.regionid);

    int init_ret = YabauseInit(&yinit);
    vita_log("YabauseInit returned %d\n", init_ret);

    if (init_ret != 0)
    {
        vita_log("FATAL: YabauseInit failed.\n");
        clear_framebuffer(0xFF200810);
        while (1) sceDisplayWaitVblankStart();
    }

    vita_log("Boot OK. Entering emulation loop.\n");

    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    int frame = 0;
    int running = 1;
    while (running)
    {
        YabauseExec();
        frame++;

        SceCtrlData pad;
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_START)
            running = 0;

        if (frame % 300 == 0)
            vita_log("frame %d\n", frame);
    }

    vita_log("Emulation stopped by user.\n");

    YabauseDeInit();
    if (g_logfile) fclose(g_logfile);
    sceKernelExitProcess(0);
    return 0;
}
