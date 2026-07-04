/*  src/vita/main.c: PS Vita entry point for Yabause (Milestone 0)
    Copyright 2026 YabauseVita port

    This file is part of Yabause.

    MILESTONE 0 GOAL: boot the emulator core on real Vita hardware using
    only the platform-neutral pieces that already exist in Yabause:

      - SH2CORE_INTERPRETER  (sh2int.c)   -> pure C, no MIPS/x86 asm
      - VIDCORE_SOFT         (vidsoft.c)  -> pure C software rasterizer
      - PERCORE_DUMMY        (peripheral.c) -> no input yet
      - SNDCORE_DUMMY        (scsp.c)     -> silence, no audio yet
      - CDCORE_DUMMY         (cdbase.c)   -> no real disc yet

    None of this is fast. That's expected and correct for this phase --
    the goal is a walking skeleton we can verify on hardware, then speed
    up in later phases (ARM dynarec, vitaGL renderer, real CD/audio/input).

    NOT YET IMPLEMENTED (by design, for this milestone):
      - Controls (sceCtrl) - PERCORE_DUMMY means no buttons do anything
      - Audio (sceAudio)   - SNDCORE_DUMMY means silence
      - Real ISO loading   - CDCORE_DUMMY means no game will actually run;
                              this milestone only proves the BIOS/core boots
*/
#include <stdarg.h>        // para va_start, va_end, va_list
#include <psp2/io/stat.h>  // para sceIoMkdir
#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>
#include <stdio.h>
#include <string.h>

#include "../yabause.h"
#include "../sh2core.h"
#include "../sh2int.h"
#include "../vidsoft.h"
#include "../peripheral.h"
#include "../cdbase.h"
#include "../cs2.h"
#include "../scsp.h"
#include "../m68kcore.h"

#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544

/* framebuffer Vita's display controller reads from directly */
static void *vita_fb = NULL;

int vita_log(const char *fmt, ...);

/* ---- every Yabause port must define these 6 lists, one entry per
   compiled-in core, NULL-terminated. This is how YabauseInit() finds
   the core matching e.g. yinit.sh2coretype. We only list the portable
   "dummy"/software cores for milestone 0 -- nothing PSP/MIPS-specific
   gets compiled in at all. ---- */

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
CDInterface *CDCoreList[] = {
    &DummyCD,
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

/* ---- required by yui.h : every Yabause port must implement these 4 ---- */

void YuiErrorMsg(const char *string)
{
    vita_log("[Yabause ERROR] %s\n", string);
}

void YuiSetVideoAttribute(int type, int val)
{
    /* not needed for the software core */
    (void)type; (void)val;
}

int YuiSetVideoMode(int width, int height, int bpp, int fullscreen)
{
    (void)width; (void)height; (void)bpp; (void)fullscreen;
    /* VIDSoft owns its own internal dispbuffer; we just need our
       Vita-side framebuffer ready, which we already set up in main() */
    return 0;
}

void YuiSwapBuffers(void)
{
    /* VIDSoft finished drawing a Saturn frame into its internal
       dispbuffer (max 704x512, actual size in vdp2width/vdp2height
       via VIDSoftGetScreenSize). Copy + center it into the Vita's
       960x544 framebuffer. This is a NAIVE 1:1 copy for milestone 0
       (no scaling yet) -- centered, letterboxed, no filtering. */

    extern u32 *dispbuffer;
    int srcw, srch;
    VIDSoftGetScreenSize(&srcw, &srch);

    if (dispbuffer == NULL || srcw <= 0 || srch <= 0)
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

/* ---- tiny logger: writes to ux0:data/yabausevita/log.txt so we can
   see what happened even though there's no debugger attached ---- */

static FILE *g_logfile = NULL;

int vita_log(const char *fmt, ...)
{
    if (g_logfile == NULL)
        return -1;
    va_list args;
    va_start(args, fmt);
    int r = vfprintf(g_logfile, fmt, args);
    va_end(args);
    fflush(g_logfile);
    return r;
}

/* ---- entry point ---- */

int main(int argc, char *argv[])
{
    sceIoMkdir("ux0:data/yabausevita", 0777);
    g_logfile = fopen("ux0:data/yabausevita/log.txt", "w");
    vita_log("YabauseVita milestone 0 starting\n");

    /* solid dark-blue framebuffer so we can SEE something even before
       the core finishes booting -- if the screen never turns from
       black to blue, main() itself is crashing before this point */
    vita_fb = malloc(VITA_SCREEN_W * VITA_SCREEN_H * sizeof(uint32_t));
    if (vita_fb == NULL)
    {
        vita_log("FATAL: could not allocate framebuffer\n");
        sceKernelExitProcess(0);
        return 0;
    }
    uint32_t *px = (uint32_t *)vita_fb;
    for (int i = 0; i < VITA_SCREEN_W * VITA_SCREEN_H; i++)
        px[i] = 0xFF3B2510; /* ABGR: opaque dark blue */

    SceDisplayFrameBuf fb;
    memset(&fb, 0, sizeof(fb));
    fb.size = sizeof(fb);
    fb.base = vita_fb;
    fb.pitch = VITA_SCREEN_W;
    fb.pixelformat = SCE_DISPLAY_PIXELFORMAT_A8B8G8R8;
    fb.width = VITA_SCREEN_W;
    fb.height = VITA_SCREEN_H;
    sceDisplaySetFrameBuf(&fb, SCE_DISPLAY_SETBUF_NEXTFRAME);

    vita_log("Framebuffer ready, %dx%d\n", VITA_SCREEN_W, VITA_SCREEN_H);

    yabauseinit_struct yinit;
    memset(&yinit, 0, sizeof(yinit));
    yinit.percoretype   = PERCORE_DUMMY;
    yinit.sh2coretype   = SH2CORE_INTERPRETER;
    yinit.vidcoretype   = VIDCORE_SOFT;
    yinit.sndcoretype   = SNDCORE_DUMMY;
    yinit.m68kcoretype  = 0; /* M68KCORE_DUMMY - id 0 in M68KCoreList */
    yinit.cdcoretype    = CDCORE_DUMMY;
    yinit.carttype      = 0; /* CART_NONE */
    yinit.regionid      = 0; /* auto */
    yinit.biospath      = NULL; /* NULL = HLE bios, no real BIOS dump needed yet */
    yinit.cdpath        = NULL;
    yinit.buppath       = NULL;
    yinit.mpegpath      = NULL;
    yinit.cartpath      = NULL;
    yinit.netlinksetting = NULL;
    yinit.flags         = 0;
    yinit.frameskip     = 0;

    vita_log("Calling YabauseInit...\n");
    int ret = YabauseInit(&yinit);
    vita_log("YabauseInit returned %d\n", ret);

    if (ret != 0)
    {
        vita_log("FATAL: YabauseInit failed, stopping here (screen should\n"
                  "stay dark blue). Check log.txt for the failing subsystem.\n");
        while (1)
        {
            sceDisplayWaitVblankStart();
        }
    }

    vita_log("Boot succeeded. Entering emulation loop (interpreter core,\n"
              "software renderer -- this WILL be slow, that's expected).\n");

    int frame = 0;
    while (1)
    {
        YabauseExec();
        frame++;
        if (frame % 60 == 0)
            vita_log("frame %d\n", frame);
    }

    return 0;
}
