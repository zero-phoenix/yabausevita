#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
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
#include "chd_read.h"

extern SH2Interface_struct SH2Fast;
extern SH2Interface_struct SH2LRU;
extern VideoInterface_struct VIDGPU;

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
SH2Interface_struct *SH2CoreList[] = { &SH2Interpreter, &SH2DebugInterpreter, &SH2Fast, &SH2LRU, NULL };

extern PerInterface_struct PERDummy;
PerInterface_struct *PERCoreList[] = { &PERDummy, NULL };

extern CDInterface DummyCD;
extern CDInterface ISOCD;
CDInterface *CDCoreList[] = { &DummyCD, &ISOCD, NULL };

extern SoundInterface_struct SNDDummy;
SoundInterface_struct *SNDCoreList[] = { &SNDDummy, NULL };

extern VideoInterface_struct VIDSoft;
VideoInterface_struct *VIDCoreList[] = { &VIDSoft, &VIDGPU, NULL };

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

    if (srcw <= 0 || srch <= 0 || vita_fb == NULL || dispbuffer == NULL)
        return;

    int offx = (VITA_SCREEN_W - srcw) / 2;
    int offy = (VITA_SCREEN_H - srch) / 2;
    if (offx < 0) offx = 0;
    if (offy < 0) offy = 0;

    uint32_t *dst = (uint32_t *)vita_fb;
    int copyw = srcw;
    if (offx + copyw > VITA_SCREEN_W)
        copyw = VITA_SCREEN_W - offx;
    if (copyw <= 0) return;

    int max_h = srch;
    if (offy + max_h > VITA_SCREEN_H)
        max_h = VITA_SCREEN_H - offy;
    if (max_h <= 0) return;

    for (int y = 0; y < max_h; y++)
        memcpy(dst + (y + offy) * VITA_SCREEN_W + offx, dispbuffer + y * srcw, copyw * sizeof(uint32_t));

    SceDisplayFrameBuf fb;
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

/* Returns: 0=success (path now points to .bin), 1=not CHD (no change) */
static int chd_to_bin_path(char *path, int max_len)
{
    char ext[16];
    const char *dot = strrchr(path, '.');
    if (!dot) return 1;
    safe_strcpy(ext, dot + 1, sizeof(ext));
    to_lower(ext);
    if (strcmp(ext, "chd") != 0) return 1;

    char bin_path[VMENU_MAX_PATH];
    int len = (int)(dot - path);
    strncpy(bin_path, path, len);
    bin_path[len] = '\0';
    safe_strcat(bin_path, ".bin", sizeof(bin_path));

    SceIoStat tmp;
    memset(&tmp, 0, sizeof(tmp));
    if (sceIoGetstat(bin_path, &tmp) >= 0)
    {
        safe_strcpy(path, bin_path, max_len);
        vita_log("Using cached CHD extraction: %s\n", bin_path);
        return 0;
    }

    vita_log("Extracting CHD to %s\n", bin_path);
    char err[256];
    int ret = chd_extract(path, bin_path, err, sizeof(err));
    if (ret != 0)
    {
        vita_log("CHD extraction failed: %s\n", err);
        vita_menu_show_error("Error al extraer CHD", err);
        sceKernelExitProcess(0);
        return 0;
    }

    safe_strcpy(path, bin_path, max_len);
    vita_log("CHD extracted successfully: %s\n", bin_path);
    return 0;
}

/* Auto-detecta el primer BIOS .bin en ux0:data/yabause/bios/{jp,us,eu} */
static int autodetect_bios(char *out_path, size_t out_len)
{
    const char *dirs[] = {
        "ux0:data/yabause/bios/jp",
        "ux0:data/yabause/bios/us",
        "ux0:data/yabause/bios/eu"
    };

    for (int d = 0; d < 3; d++)
    {
        SceUID dir = sceIoDopen(dirs[d]);
        if (dir < 0) continue;

        SceIoDirent entry;
        memset(&entry, 0, sizeof(entry));
        while (sceIoDread(dir, &entry) > 0)
        {
            if (!SCE_S_ISDIR(entry.d_stat.st_mode))
            {
                const char *name = entry.d_name;
                size_t len = strlen(name);
                if (len >= 4)
                {
                    const char *ext = name + len - 4;
                    char e1 = ext[0], e2 = ext[1], e3 = ext[2], e4 = ext[3];
                    if ((e1 == 'b' || e1 == 'B') &&
                        (e2 == 'i' || e2 == 'I') &&
                        (e3 == 'n' || e3 == 'N') &&
                        (e4 == 'n' || e4 == 'N'))
                    {
                        snprintf(out_path, out_len, "%s/%s", dirs[d], name);
                        sceIoDclose(dir);
                        vita_log("Auto-detected BIOS: %s\n", out_path);
                        return 1;
                    }
                }
            }
            memset(&entry, 0, sizeof(entry));
        }
        sceIoDclose(dir);
    }
    return 0;
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

    /* Auto-detect BIOS si el usuario no eligió uno y auto_bios está activado */
    if (cfg.auto_bios && (cfg.bios_path[0] == '\0'))
    {
        if (autodetect_bios(cfg.bios_path, sizeof(cfg.bios_path)))
        {
            vita_log("Using auto-detected BIOS: %s\n", cfg.bios_path);
        }
        else
        {
            vita_log("No BIOS auto-detected, using HLE\n");
            cfg.bios_path[0] = '\0';
        }
    }
    else if (cfg.bios_path[0] != '\0')
    {
        vita_log("Using user-selected BIOS: %s\n", cfg.bios_path);
    }
    else
    {
        vita_log("No BIOS selected, using HLE\n");
    }

    vita_log("Calling YabauseInit\n");

    char cdpath[VMENU_MAX_PATH];
    safe_strcpy(cdpath, cfg.rom_path, sizeof(cdpath));
    int chd_ret = chd_to_bin_path(cdpath, sizeof(cdpath));
    if (chd_ret < 0)
    {
        vita_menu_show_error("CHD no soportado",
            "Este CHD no tiene un archivo .bin adjunto.\nExtrae el .bin primero.");
        sceKernelExitProcess(0);
        return 0;
    }

    yabauseinit_struct yinit;
    memset(&yinit, 0, sizeof(yinit));
    yinit.percoretype   = PERCORE_DUMMY;
    yinit.sh2coretype   = (cfg.cpu_mode == VMENU_CPU_RECOMP) ? 3 : 2; /* 2=SH2Fast, 3=SH2LRU */
    yinit.vidcoretype   = VIDCORE_GPU;
    yinit.sndcoretype   = SNDCORE_DUMMY;
    yinit.m68kcoretype  = 0;
    yinit.cdcoretype    = CDCORE_ISO;
    yinit.cdpath        = cdpath;
    yinit.biospath      = cfg.bios_path[0] ? cfg.bios_path : NULL;
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
        vita_menu_show_error("Error de inicializacion",
            "Yabause no pudo inicializarse.\nRevisa la configuracion y BIOS.");
        sceKernelExitProcess(0);
        return 0;
    }

    vita_log("QuickLoading game\n");
    int ql_ret = YabauseQuickLoadGame();
    vita_log("YabauseQuickLoadGame returned %d\n", ql_ret);

    if (ql_ret != 0)
    {
        vita_log("FATAL: QuickLoad failed\n");
        vita_menu_show_error("Error al cargar el juego",
            "YabauseQuickLoadGame fallo.\n"
            "El formato del disco podria no ser compatible.");
        sceKernelExitProcess(0);
        return 0;
    }

    vita_log("Adding per-pad\n");
    PerPad_struct *saturn_pad = PerPadAdd(&PORTDATA1);
    vita_log("pad=%s\n", saturn_pad ? "OK" : "FAIL");

    vita_log("Button mapping:\n");
    for (int mi = 0; mi < MAP_COUNT; mi++) {
        vita_log("  map[%d] = %d\n", mi, cfg.mapping[mi]);
    }

    g_show_fps = cfg.show_fps;
    g_auto_frameskip = cfg.auto_frameskip;
    g_frame_skip = cfg.frame_skip;

    int frame_count = 0, skip_counter = 0;
    SceUInt64 fps_timer = sceKernelGetProcessTimeWide();
    SceUInt64 next_display = 0;
    unsigned int last_buttons = 0;
    int skip = g_frame_skip;
    if (skip < 0) skip = 0;
    if (skip > 4) skip = 4;

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
        frame_count++;

        SceUInt64 now = sceKernelGetProcessTimeWide();
        if (now >= next_display)
        {
            YuiSwapBuffers();
            next_display = now + 16000ULL;
        }

        if (now - fps_timer >= 5000000ULL)
        {
            float f = (float)frame_count * 1000000.0f / (float)(now - fps_timer);
            vita_log("FPS: %.1f\n", f);
            VIDGPUVdp2LogTiming();
            frame_count = 0;
            fps_timer = now;
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