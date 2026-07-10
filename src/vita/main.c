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
#define TARGET_FRAME_MS 16
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
    FILE *f = fopen("ux0:data/yabausevita_log.txt", "a");
    if (!f) return -1;
    va_list args;
    va_start(args, fmt);
    int r = vfprintf(f, fmt, args);
    va_end(args);
    fclose(f);
    return r;
}

static const unsigned int vita_btn_bits[MAP_COUNT] = {
    SCE_CTRL_UP,       SCE_CTRL_DOWN,    SCE_CTRL_LEFT,   SCE_CTRL_RIGHT,
    SCE_CTRL_CROSS,    SCE_CTRL_CIRCLE,  SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE,
    SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER,SCE_CTRL_START,  SCE_CTRL_SELECT
};

static void apply_saturn_btn(PerPad_struct *pad, int btn, int press)
{
    switch (btn)
    {
        case SAT_UP:     if(press) PerPadUpPressed(pad);    else PerPadUpReleased(pad);    break;
        case SAT_DOWN:   if(press) PerPadDownPressed(pad);  else PerPadDownReleased(pad);  break;
        case SAT_LEFT:   if(press) PerPadLeftPressed(pad);  else PerPadLeftReleased(pad);  break;
        case SAT_RIGHT:  if(press) PerPadRightPressed(pad); else PerPadRightReleased(pad); break;
        case SAT_A:      if(press) PerPadAPressed(pad);     else PerPadAReleased(pad);     break;
        case SAT_B:      if(press) PerPadBPressed(pad);     else PerPadBReleased(pad);     break;
        case SAT_C:      if(press) PerPadCPressed(pad);     else PerPadCReleased(pad);     break;
        case SAT_X:      if(press) PerPadXPressed(pad);     else PerPadXReleased(pad);     break;
        case SAT_Y:      if(press) PerPadYPressed(pad);     else PerPadYReleased(pad);     break;
        case SAT_Z:      if(press) PerPadZPressed(pad);     else PerPadZReleased(pad);     break;
        case SAT_L:      if(press) PerPadLTriggerPressed(pad);  else PerPadLTriggerReleased(pad);  break;
        case SAT_R:      if(press) PerPadRTriggerPressed(pad);  else PerPadRTriggerReleased(pad);  break;
        case SAT_START:  if(press) PerPadStartPressed(pad); else PerPadStartReleased(pad); break;
    }
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

    vita_log("YabauseVita starting\n");

    vita_fb = malloc((size_t)VITA_SCREEN_W * VITA_SCREEN_H * sizeof(uint32_t));
    if (vita_fb == NULL)
    {
        vita_log("FATAL: could not allocate framebuffer\n");
        sceKernelExitProcess(0);
        return 0;
    }
    vita_log("fb=%p\n", vita_fb);

    vita_log("Initializing menu\n");
    if (vita_menu_init() != 0)
    {
        vita_log("FATAL: vita_menu_init failed\n");
        sceKernelExitProcess(0);
        return 0;
    }

    VitaMenuConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    vita_log("Entering menu\n");
    int menu_result = vita_menu_run(&cfg, NULL);
    vita_log("Menu exited with result=%d\n", menu_result);

    if (menu_result != 0)
    {
        vita_log("No game selected, exiting\n");
        sceKernelExitProcess(0);
        return 0;
    }

    vita_log("Selected: %s\n", cfg.rom_path);
    vita_log("BIOS: %s\n", cfg.bios_path);

    vita_log("Calling YabauseInit\n");

    char cdpath[VMENU_MAX_PATH];
    safe_strcpy(cdpath, cfg.rom_path, sizeof(cdpath));
    chd_to_bin_path(cdpath, sizeof(cdpath));

    yabauseinit_struct yinit;
    memset(&yinit, 0, sizeof(yinit));
    yinit.percoretype   = PERCORE_DUMMY;
    yinit.sh2coretype   = (cfg.cpu_mode == VMENU_CPU_RECOMP) ? SH2CORE_DEBUGINTERPRETER : SH2CORE_INTERPRETER;
    yinit.vidcoretype   = VIDCORE_SOFT;
    yinit.sndcoretype   = SNDCORE_DUMMY;
    yinit.m68kcoretype  = 0;
    yinit.cdcoretype    = CDCORE_ISO;
    yinit.cdpath        = cdpath;
    yinit.biospath      = cfg.bios_path;
    yinit.carttype      = 0;
    yinit.regionid      = (cfg.rom_region != VMENU_REGION_UNKNOWN && cfg.rom_region != VMENU_REGION_AUTO) ? map_region(cfg.rom_region) : REGION_AUTODETECT;
    yinit.buppath       = NULL;
    yinit.mpegpath      = NULL;
    yinit.cartpath      = NULL;
    yinit.netlinksetting = NULL;
    yinit.flags         = 0;
    yinit.frameskip     = cfg.frame_skip;

    int init_ret = YabauseInit(&yinit);
    vita_log("YabauseInit returned %d\n", init_ret);

    if (init_ret != 0)
    {
        vita_log("FATAL: YabauseInit failed\n");
        while (1) sceDisplayWaitVblankStart();
    }

    vita_log("QuickLoading game\n");
    int ql_ret = YabauseQuickLoadGame();
    vita_log("YabauseQuickLoadGame returned %d\n", ql_ret);

    if (ql_ret != 0)
    {
        vita_log("FATAL: QuickLoad failed\n");
        while (1) sceDisplayWaitVblankStart();
    }

    vita_log("Adding per-pad\n");
    PerPad_struct *saturn_pad = PerPadAdd(&PORTDATA1);
    vita_log("pad=%s\n", saturn_pad ? "OK" : "FAIL");

    g_show_fps = cfg.show_fps;
    g_auto_frameskip = cfg.auto_frameskip;
    g_frame_skip = cfg.frame_skip;

    int fps_count = 0, skip_counter = 0;
    SceUInt64 fps_timer = sceKernelGetProcessTimeWide();
    SceUInt64 next_display = 0;
    unsigned int last_buttons = 0;
    int skip = g_frame_skip;
    if (skip < 0) skip = 0;
    if (skip > 4) skip = 4;
    SceUInt64 frame_interval = TARGET_FRAME_MS * 1000ULL;

    vita_log("Entering emulation loop, frame_skip=%d\n", skip);

    for (;;)
    {
        int display_this = (skip == 0) ? 1 : (skip_counter == 0);
        skip_counter = (skip_counter + 1) % (skip + 1);

        if (!display_this)
        {
            YabauseExec();
            continue;
        }

        YabauseExec();
        fps_count++;

        SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now >= fps_timer + 1000000ULL)
        {
            float f = (float)fps_count * 1000000.0f / (float)(now - fps_timer);
            vita_log("FPS: %.1f\n", f);
            fps_count = 0;
            fps_timer = now;
        }

        if (now >= next_display)
        {
            YuiSwapBuffers();
            next_display = now + frame_interval;
        }

        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int cur = pad.buttons;
        unsigned int changed = cur ^ last_buttons;
        if (changed && saturn_pad)
        {
            for (int m = 0; m < MAP_COUNT; m++)
            {
                unsigned int bit = vita_btn_bits[m];
                if (changed & bit)
                    apply_saturn_btn(saturn_pad, cfg.mapping[m], cur & bit);
            }
        }
        if (cur & SCE_CTRL_START)
            break;
        last_buttons = cur;
    }

    vita_log("Emulation stopped\n");

    sceKernelExitProcess(0);
    return 0;
}
