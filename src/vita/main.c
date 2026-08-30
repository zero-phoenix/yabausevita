#include <psp2/kernel/processmgr.h>
#include <psp2/display.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <vita2d.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
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
#include "cd_chd.h"
#include "snd_vita.h"
#include "emuprof.h"

extern SH2Interface_struct SH2Fast;
extern SH2Interface_struct SH2LRU;
extern VideoInterface_struct VIDGPU;
extern void VIDGPUVdp2LogTiming(void);

#define VITA_SCREEN_W 960
#define VITA_SCREEN_H 544
#define TARGET_FRAME_MS 16
#define MAX_SKIP_FRAMES 10

static int g_auto_frameskip = 1;
static int g_frame_skip = 0;
int g_show_fps = 0;              /* show_fps del config: FPS del ROM en pantalla (vidgpu) */
extern float g_game_fps;         /* contador visible del FPS emulado (vidgpu) */
static int g_vsync = 1;

int vita_log(const char *fmt, ...);

extern M68K_struct M68KDummy;
M68K_struct *M68KCoreList[] = { &M68KDummy, &M68KQ68, NULL };

extern SH2Interface_struct SH2Interpreter;
extern SH2Interface_struct SH2DebugInterpreter;
extern SH2Interface_struct SH2DynARM;
SH2Interface_struct *SH2CoreList[] = { &SH2Interpreter, &SH2DebugInterpreter, &SH2Fast, &SH2LRU, &SH2DynARM, NULL };

extern PerInterface_struct PERDummy;
PerInterface_struct *PERCoreList[] = { &PERDummy, NULL };

extern CDInterface DummyCD;
extern CDInterface ISOCD;
CDInterface *CDCoreList[] = { &DummyCD, &ISOCD, &CHDCD, NULL };

extern SoundInterface_struct SNDDummy;
SoundInterface_struct *SNDCoreList[] = { &SNDDummy, &SNDVita, NULL };

extern VideoInterface_struct VIDSoft;
extern VideoInterface_struct VIDVitaGL;
VideoInterface_struct *VIDCoreList[] = { &VIDSoft, &VIDGPU, &VIDVitaGL, NULL };

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
    /* VIDGPU (vidgpu.c) presenta cada frame vía vita2d en Vdp2DrawEnd,
       ya escalado y con el formato de color correcto. La ruta anterior
       (memcpy 1:1 sin escalar a un framebuffer propio + sceDisplaySetFrameBuf)
       competía con los buffers de vita2d: imagen pequeña y parpadeos. */
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

/* Devuelve el CD core adecuado según la extensión del archivo.
   CHD se lee DIRECTO (sin extraer a .bin): carga instantánea. */
static int cd_core_for_path(const char *path)
{
    char ext[16];
    const char *dot = strrchr(path, '.');
    if (!dot) return CDCORE_ISO;
    safe_strcpy(ext, dot + 1, sizeof(ext));
    to_lower(ext);
    if (strcmp(ext, "chd") == 0) return CDCORE_CHD;
    return CDCORE_ISO;
}

/* Auto-detecta el primer BIOS .bin en ux0:data/yabause/bios/{jp,us,eu} */
static int autodetect_bios(char *out_path, size_t out_len)
{
    const char *dirs[] = {
        "ux0:data/yabause/bios",
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

    /* Overclock al máximo del API oficial. Se fija UNA sola vez al arrancar:
       PSVshell puede después subirlos (p.ej. ARM a 500 MHz) o bajarlos sin
       que la app se los pise, porque nunca volvemos a tocarlos. */
    scePowerSetArmClockFrequency(444);
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);
    vita_log("Clocks: ARM=%d BUS=%d GPU=%d XBAR=%d MHz\n",
             scePowerGetArmClockFrequency(), scePowerGetBusClockFrequency(),
             scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());

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

    /* Auto-detect BIOS if user didn't select one (try regardless of auto_bios flag) */
    if (cfg.bios_path[0] == '\0')
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
    else
    {
        vita_log("Using user-selected BIOS: %s\n", cfg.bios_path);
    }

    vita_log("Calling YabauseInit\n");

    char cdpath[VMENU_MAX_PATH];
    safe_strcpy(cdpath, cfg.rom_path, sizeof(cdpath));
    int cdcore = cd_core_for_path(cdpath);
    vita_log("CD core: %s (%d)\n", cdcore == CDCORE_CHD ? "CHD directo" : "ISO", cdcore);

    yabauseinit_struct yinit;
    memset(&yinit, 0, sizeof(yinit));
    yinit.percoretype   = PERCORE_DUMMY;
    if (cfg.cpu_mode == VMENU_CPU_DYNARM) {
        yinit.sh2coretype = 4; // SH2DYN_ARM_ID
    } else if (cfg.cpu_mode == VMENU_CPU_RECOMP) {
        yinit.sh2coretype = 3; // SH2LRU
    } else {
        yinit.sh2coretype = 2; // SH2Fast (Interprete rapido)
    }
    yinit.vidcoretype   = VIDCORE_GPU;
    /* Audio real: backend sceAudioOut + 68K Q68 ejecutando el driver de
       sonido del juego (SFX/música secuenciada) + música CDDA del CD.
       Con audio OFF ambos quedan en dummy y se ahorra ese CPU. */
    yinit.sndcoretype   = cfg.audio_enabled ? SNDCORE_VITA : SNDCORE_DUMMY;
    yinit.m68kcoretype  = cfg.audio_enabled ? M68KCORE_Q68 : M68KCORE_DUMMY;
    yinit.cdcoretype    = cdcore;
    yinit.cdpath        = cdpath;
    yinit.biospath      = cfg.bios_path[0] ? cfg.bios_path : NULL;
    yinit.carttype      = 0;
    yinit.regionid      = (cfg.rom_region != VMENU_REGION_UNKNOWN && cfg.rom_region != VMENU_REGION_AUTO) ? map_region(cfg.rom_region) : REGION_AUTODETECT;
    yinit.buppath       = NULL;
    yinit.mpegpath      = NULL;
    yinit.cartpath      = NULL;
    yinit.netlinksetting = NULL;
    yinit.flags         = 0;
    /* Siempre 1: activa el auto-frameskip interno de vdp2.c, que mide
       ticks reales, salta frames completos (incluido el render del VDP1,
       con la semántica correcta de Vdp1NoDraw) y limita la velocidad
       cuando el juego va sobrado. Requiere HAVE_GETTIMEOFDAY definido
       (sin él, YabauseGetTicks devolvía basura en Vita). */
    yinit.frameskip     = 1;

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

    if (cfg.audio_enabled)
    {
        ScspSetVolume(cfg.audio_volume);
        ScspUnMuteAudio();
        /* Motor de sonido en hilo dedicado (otro núcleo): 68K + timers +
           mezcla SCSP fuera del hilo principal. Orden importante:
           primero el modo threaded, luego encender el motor. */
        ScspSetThreaded(1);
        SNDVitaEnableEngine();
        vita_log("Audio ON (hilo dedicado): vol=%d%%, 68K=Q68\n", cfg.audio_volume);
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

    /* cfg.mapping ya viene cargado del menú/archivo de config del usuario.
       (Antes se forzaba set_default_mapping aquí y pisaba lo configurado.) */
    vita_log("Button mapping (from user config):\n");
    for (int mi = 0; mi < MAP_COUNT; mi++) {
        vita_log("  map[%d] = %d\n", mi, cfg.mapping[mi]);
    }

    g_show_fps = cfg.show_fps;
    g_auto_frameskip = cfg.auto_frameskip;
    g_frame_skip = cfg.frame_skip;

    int frame_count = 0;
    int sec_count = 0;
    SceUInt64 fps_timer = sceKernelGetProcessTimeWide();
    SceUInt64 exit_combo_t0 = 0;
    unsigned int last_buttons = 0;

    vita_log("Entering emulation loop: auto_skip=%d fixed_skip=%d audio=%d\n",
             g_auto_frameskip, g_frame_skip, cfg.audio_enabled);

    for (;;)
    {
        YabauseExec();
        frame_count++;

        SceUInt64 now = sceKernelGetProcessTimeWide();

        /* FPS del ROM: refresco de 1 s para el contador en pantalla,
           volcado al log cada 5 s junto a GPU/EMU (mismo contrato). */
        if (now - fps_timer >= 1000000ULL)
        {
            g_game_fps = (float)frame_count * 1000000.0f / (float)(now - fps_timer);
            if (++sec_count >= 5)
            {
                vita_log("FPS: %.1f\n", g_game_fps);
                VIDGPUVdp2LogTiming();
                EMUPROFLog();
                EMUPROFReset();
                sec_count = 0;
            }
            frame_count = 0;
            fps_timer = now;
        }

        SceCtrlData pad;
        sceCtrlPeekBufferPositive(0, &pad, 1);
        unsigned int cur = pad.buttons;

        /* Analógico izquierdo → cruceta del Saturn (zona muerta ±50) */
        if (pad.lx < 78)       cur |= SCE_CTRL_LEFT;
        else if (pad.lx > 178) cur |= SCE_CTRL_RIGHT;
        if (pad.ly < 78)       cur |= SCE_CTRL_UP;
        else if (pad.ly > 178) cur |= SCE_CTRL_DOWN;

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

        /* Salir del juego: mantener START+SELECT ~1 segundo.
           START solo YA NO cierra la app — llega al juego (pausa, menús). */
        if ((cur & (SCE_CTRL_START | SCE_CTRL_SELECT)) ==
                   (SCE_CTRL_START | SCE_CTRL_SELECT))
        {
            if (exit_combo_t0 == 0)
                exit_combo_t0 = now;
            else if (now - exit_combo_t0 >= 1000000ULL)
                break;
        }
        else
            exit_combo_t0 = 0;

        last_buttons = cur;
    }

    vita_log("Emulation stopped\n");

    sceKernelExitProcess(0);
    return 0;
}