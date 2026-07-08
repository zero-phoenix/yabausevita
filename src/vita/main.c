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
#define TARGET_FRAME_MS 50
#define MAX_SKIP_FRAMES 10

static void *vita_fb = NULL;
static int g_auto_frameskip = 1;
static int g_frame_skip = 0;
static int g_show_fps = 0;
static int g_vsync = 1;

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

FILE *g_logfile = NULL;

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

static void clear_fb(void)
{
    if (!vita_fb) return;
    memset(vita_fb, 0, (size_t)VITA_SCREEN_W * VITA_SCREEN_H * sizeof(uint32_t));
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

static void chd_to_bin_path(char *path, int max_len)
{
    char ext[16];
    const char *dot = strrchr(path, '.');
    if (!dot) return;
    safe_strcpy(ext, dot + 1, sizeof(ext));
    to_lower(ext);
    if (strcmp(ext, "chd") != 0) return;

    char bin_path[VMENU_MAX_PATH];
    safe_strcpy(bin_path, path, sizeof(bin_path));
    int len = (int)(dot - path);
    bin_path[len] = '\0';
    safe_strcat(bin_path, ".bin", sizeof(bin_path));

    SceIoStat tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (sceIoGetstat(bin_path, &tmp) >= 0)
    {
        safe_strcpy(path, bin_path, max_len);
        vita_log("CHD->BIN swap: %s\n", bin_path);
    }
}

static int vita_menu_load_callback(const VitaMenuConfig *cfg, char *error, int error_size)
{
    (void)cfg; (void)error; (void)error_size;
    return 0;
}

int main(int argc, char *argv[])
{
    (void)argc; (void)argv;

    g_logfile = fopen("ux0:data/yabausevita_log.txt", "w");
    vita_log("YabauseVita starting\n");

    vita_fb = malloc((size_t)VITA_SCREEN_W * VITA_SCREEN_H * sizeof(uint32_t));
    if (vita_fb == NULL)
    {
        vita_log("FATAL: could not allocate framebuffer\n");
        sceKernelExitProcess(0);
        return 0;
    }

    vita_log("Framebuffer at %p\n", vita_fb);

    // Bypass menu for crash testing
    vita_log("Bypassing menu, calling YabauseInit directly\n");

    yabauseinit_struct yinit;
    memset(&yinit, 0, sizeof(yinit));
    yinit.percoretype   = PERCORE_DUMMY;
    yinit.sh2coretype   = SH2CORE_INTERPRETER;
    yinit.vidcoretype   = VIDCORE_SOFT;
    yinit.sndcoretype   = SNDCORE_DUMMY;
    yinit.m68kcoretype  = 0;
    yinit.cdcoretype    = CDCORE_ISO;
    yinit.cdpath        = "ux0:data/yabause/roms/Sonic R (Europe).chd";
    yinit.biospath      = "ux0:data/yabause/bios/saturn_bios.bin";
    yinit.carttype      = 0;
    yinit.regionid      = REGION_AUTODETECT;
    yinit.buppath       = NULL;
    yinit.mpegpath      = NULL;
    yinit.cartpath      = NULL;
    yinit.netlinksetting = NULL;
    yinit.flags         = 0;
    yinit.frameskip     = 0;

    vita_log("Calling YabauseInit...\n");

    int init_ret = YabauseInit(&yinit);
    vita_log("YabauseInit returned %d\n", init_ret);

    if (init_ret != 0)
    {
        vita_log("FATAL: YabauseInit failed.\n");
        clear_fb();
        while (1) sceDisplayWaitVblankStart();
    }

    vita_log("Boot OK. Adding per-pad.\n");

    PerPad_struct *saturn_pad = PerPadAdd(&PORTDATA1);
    vita_log("Saturn pad %s\n", saturn_pad ? "added" : "FAILED");

    vita_log("Calling YabauseExec once...\n");
    YabauseExec();
    vita_log("YabauseExec returned\n");

    vita_log("Calling YuiSwapBuffers...\n");
    YuiSwapBuffers();
    vita_log("YuiSwapBuffers returned\n");

    vita_log("Test complete, exiting cleanly.\n");

    if (g_logfile) fclose(g_logfile);
    sceKernelExitProcess(0);
    return 0;
}
