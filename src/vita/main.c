/*  src/vita/main.c: PS Vita entry point for Yabause (Phase 2)
    Copyright 2026 YabauseVita port

    This file is part of Yabause.

    PHASE 2: a usable Saturn emulator front-end on Vita.

    What changed from the milestone-0 walking skeleton:
      - vita2d-based tabbed UI (roms / bios / config) with black+blue theme
      - splash/intro screen on launch
      - REAL ISO loading via CDCORE_ISO / ISOCD (was CDCORE_DUMMY before,
        which is exactly why games "got stuck loading" -- no disc mounted)
      - REAL BIOS loading when a bios file is found in ux0:data/yabause/bios/
      - region auto-detection driven by the UI

    Cores in use (still pure-C / portable):
      - SH2CORE_INTERPRETER  (sh2int.c)
      - VIDCORE_SOFT         (vidsoft.c)
      - PERCORE_DUMMY        (peripheral.c) -- input still not wired in-game
      - SNDCORE_DUMMY        (scsp.c)       -- audio still not wired
      - CDCORE_ISO           (cdbase.c)     -- NOW ACTIVE

    Yabause port interface (yui.h): every port implements 4 functions:
      YuiErrorMsg, YuiSetVideoAttribute, YuiSetVideoMode, YuiSwapBuffers.
*/

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

#include "vita_ui.h"

#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544

/* framebuffer Vita's display controller reads from directly during emulation */
static void *vita_fb = NULL;

int vita_log(const char *fmt, ...);

/* ---- CoreList arrays -------------------------------------------------------
   Every Yabause port must define these six arrays, one entry per compiled-in
   core, NULL-terminated. YabauseInit() searches them by numeric ID.

   FIX from milestone 0: ISOCD is now in CDCoreList so CDCORE_ISO actually
   resolves. Without it, YabauseInit() silently fell back to DummyCD and
   every game "loaded" forever with no disc mounted. */

extern M68K_struct M68KDummy;
M68K_struct *M68KCoreList[] = {
    &M68KDummy,
    NULL
};

extern SH2Interface_struct SH2Interpreter;
extern SH2Interface_struct SH2DebugInterpreter;
SH2Interface_struct *SH2CoreList[] = {
    &SH2Interpreter,
    &SH2DebugInterpreter,
    NULL
};

extern PerInterface_struct PERDummy;
PerInterface_struct *PERCoreList[] = {
    &PERDummy,
    NULL
};

extern CDInterface DummyCD;
extern CDInterface ISOCD;
CDInterface *CDCoreList[] = {
    &DummyCD,
    &ISOCD,      /* <-- THE FIX: real ISO/CUE/BIN support now reachable */
    NULL
};

extern SoundInterface_struct SNDDummy;
SoundInterface_struct *SNDCoreList[] = {
    &SNDDummy,
    NULL
};

extern VideoInterface_struct VIDSoft;
VideoInterface_struct *VIDCoreList[] = {
    &VIDSoft,
    NULL
};

/* ---- yui.h interface (4 required functions) ---- */

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
    /* VIDSoft finished a Saturn frame in its internal dispbuffer.
       Copy + center it into the Vita 960x544 framebuffer. */
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

/* ---- tiny logger to ux0:data/yabausevita_log.txt ---- */

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

/* ---- Helper: clear the framebuffer to a solid color ---- */

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

/* ---- Entry point ---- */

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    g_logfile = fopen("ux0:data/yabausevita_log.txt", "w");
    vita_log("YabauseVita phase 2 starting\n");

    /* Allocate the emulation framebuffer up front. vita2d uses the GPU and
       its own buffers for the UI; this plain malloc'd buffer is what the
       software renderer blits into during emulation. */
    vita_fb = malloc((size_t)VITA_SCREEN_W * VITA_SCREEN_H * sizeof(uint32_t));
    if (vita_fb == NULL)
    {
        vita_log("FATAL: could not allocate framebuffer\n");
        sceKernelExitProcess(0);
        return 0;
    }
    clear_framebuffer(0xFF000000); /* black */

    /* Set up control sampling so the menu actually responds */
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_DIGITAL);

    /* ---- Initialize vita2d for the UI ---- */
    vita_log("Initializing vita2d...\n");
    vita2d_init();
    vita2d_set_clear_color(RGBA8(0, 0, 0, 255));

    vita_log("Initializing UI...\n");
    if (!init_vita_ui())
    {
        vita_log("WARNING: init_vita_ui() failed (font load?), continuing\n");
    }

    /* ---- Splash / intro ---- */
    vita_log("Showing splash...\n");
    draw_splash();

    /* ---- Main UI loop (tabs) ---- */
    vita_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));

    vita_log("Entering UI main loop...\n");
    int launch = vita_ui_main_loop(&cfg);

    if (!launch || !cfg.rom_selected)
    {
        /* User pressed O to quit, or no ROM was selected */
        vita_log("User exited UI without launching a game. Quitting.\n");
        cleanup_vita_ui();
        vita2d_fini();
        if (g_logfile) fclose(g_logfile);
        sceKernelExitProcess(0);
        return 0;
    }

    vita_log("ROM selected: %s\n", cfg.rom_path);
    if (cfg.bios_path[0])
        vita_log("BIOS selected: %s (region=%d)\n", cfg.bios_path, cfg.bios_region);
    else
        vita_log("No BIOS selected, using HLE BIOS\n");

    /* ---- Tear down vita2d before starting emulation (we use raw framebuffer) ---- */
    cleanup_vita_ui();
    vita2d_fini();

    /* Re-assert the framebuffer as the display target after vita2d released
       the GPU, so the boot progress color actually shows. */
    clear_framebuffer(0xFF100820); /* very dark blue while booting */

    /* ---- Configure and initialize Yabause ---- */
    yabauseinit_struct yinit;
    memset(&yinit, 0, sizeof(yinit));
    yinit.percoretype   = PERCORE_DUMMY;
    yinit.sh2coretype   = SH2CORE_INTERPRETER;
    yinit.vidcoretype   = VIDCORE_SOFT;
    yinit.sndcoretype   = SNDCORE_DUMMY;
    yinit.m68kcoretype  = 0;            /* M68KCORE_DUMMY */

    /* THE KEY FIX: use CDCORE_ISO with the real ROM path instead of
       CDCORE_DUMMY. Without a real CD core, the Saturn BIOS/HLE sits
       forever waiting for a disc that never appears. */
    yinit.cdcoretype    = CDCORE_ISO;
    yinit.cdpath        = cfg.rom_path;

    /* BIOS: use a real dump if the user selected one, otherwise HLE. */
    yinit.biospath      = cfg.bios_path[0] ? cfg.bios_path : NULL;

    yinit.carttype      = 0;            /* CART_NONE */
    yinit.regionid      = cfg.region_override ? cfg.region_override : REGION_AUTODETECT;
    yinit.buppath       = NULL;
    yinit.mpegpath      = NULL;
    yinit.cartpath      = NULL;
    yinit.netlinksetting = NULL;
    yinit.flags         = 0;
    yinit.frameskip     = cfg.frameskip;

    vita_log("Calling YabauseInit (cdcore=CDCORE_ISO, cdpath=%s, biospath=%s, region=%d)...\n",
             yinit.cdpath ? yinit.cdpath : "(null)",
             yinit.biospath ? yinit.biospath : "(HLE)",
             yinit.regionid);

    int ret = YabauseInit(&yinit);
    vita_log("YabauseInit returned %d\n", ret);

    if (ret != 0)
    {
        vita_log("FATAL: YabauseInit failed. Screen will stay dark blue.\n");
        clear_framebuffer(0xFF200810);
        while (1)
        {
            sceDisplayWaitVblankStart();
        }
    }

    vita_log("Boot OK. Entering emulation loop.\n");

    int frame = 0;
    while (1)
    {
        YabauseExec();
        frame++;
        if (frame % 300 == 0)
            vita_log("frame %d\n", frame);
    }

    /* Unreachable in normal operation */
    YabauseDeInit();
    if (g_logfile) fclose(g_logfile);
    sceKernelExitProcess(0);
    return 0;
}
