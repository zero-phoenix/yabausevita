/*  src/vita/main.c: PS Vita entry point for Yabause (Phase 2)
    Copyright 2026 YabauseVita port

    This file is part of Yabause.

    PHASE 2: adds real button input (sceCtrl), a simple on-screen file
    browser (debugScreen text, cloned from the real vitasdk/samples repo
    at build time -- not hand-typed, to avoid transcription bugs), and
    real ISO/CUE+BIN loading via Yabause's own portable ISOCD core.

    Still using:
      - SH2CORE_INTERPRETER (slow but portable -- Phase 4 will add a
        real ARM dynarec)
      - VIDCORE_SOFT (Phase 3 will add vitaGL for filtered upscaling)
      - PERCORE_DUMMY still used INSIDE Yabause once a game boots --
        the buttons you press only work in this file's own menu for
        now. Wiring sceCtrl into Yabause's actual PerInterface_struct
        (so buttons work IN GAME) is the next step after this one.
      - SNDCORE_DUMMY (silence, still no audio)

    SUPPORTED ROM FORMATS this phase: .iso, .cue (+matching .bin).
    NOT YET: .ccd/.img/.sub (CloneCD), .chd (needs libchdr), .7z
    (needs decompression -- extract on your PC first).

    Folder layout this creates on first run:
      ux0:data/yabausevita/roms/
      ux0:data/yabausevita/bios/jp/
      ux0:data/yabausevita/bios/us/
      ux0:data/yabausevita/bios/eu/
*/

#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/display.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdlib.h>

#include "debugScreen.h"
#define printf psvDebugScreenPrintf

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

#define ROMS_DIR       "ux0:data/yabausevita/roms"
#define BIOS_ROOT_DIR  "ux0:data/yabausevita/bios"
#define BIOS_JP_DIR    "ux0:data/yabausevita/bios/jp"
#define BIOS_US_DIR    "ux0:data/yabausevita/bios/us"
#define BIOS_EU_DIR    "ux0:data/yabausevita/bios/eu"
#define YABAUSEVITA_ROOT "ux0:data/yabausevita"

#define MAX_ROM_ENTRIES 200
#define MAX_PATH_LEN 512

static void *vita_fb = NULL;

/* ---- every Yabause port must define these 6 lists ---- */

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

/* ---- required by yui.h ---- */

void YuiErrorMsg(const char *string)
{
    printf("[Yabause ERROR] %s\n", string);
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

/* ---- folder setup: sceIoMkdir needs each path level created one at a
   time (like mkdir, not mkdir -p), so we call it once per folder in
   order from shortest to longest path. Failing because a folder
   ALREADY exists is fine and expected on every run after the first. ---- */

static void ensure_folders_exist(void)
{
    sceIoMkdir(YABAUSEVITA_ROOT, 0777);
    sceIoMkdir(ROMS_DIR, 0777);
    sceIoMkdir(BIOS_ROOT_DIR, 0777);
    sceIoMkdir(BIOS_JP_DIR, 0777);
    sceIoMkdir(BIOS_US_DIR, 0777);
    sceIoMkdir(BIOS_EU_DIR, 0777);
}

/* ---- tiny helpers to recognize file extensions, case-insensitively ---- */

static int has_extension(const char *filename, const char *ext)
{
    size_t flen = strlen(filename);
    size_t elen = strlen(ext);
    if (flen < elen) return 0;
    const char *tail = filename + (flen - elen);
    for (size_t i = 0; i < elen; i++)
    {
        char a = tail[i], b = ext[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        if (a != b) return 0;
    }
    return 1;
}

typedef enum {
    ROM_SUPPORTED,
    ROM_UNSUPPORTED_CCD,
    ROM_UNSUPPORTED_CHD,
    ROM_UNSUPPORTED_7Z,
    ROM_UNKNOWN
} RomSupportLevel;

static RomSupportLevel classify_rom(const char *filename)
{
    if (has_extension(filename, ".iso")) return ROM_SUPPORTED;
    if (has_extension(filename, ".cue")) return ROM_SUPPORTED;
    if (has_extension(filename, ".ccd")) return ROM_UNSUPPORTED_CCD;
    if (has_extension(filename, ".chd")) return ROM_UNSUPPORTED_CHD;
    if (has_extension(filename, ".7z"))  return ROM_UNSUPPORTED_7Z;
    return ROM_UNKNOWN;
}

/* ---- scan the roms folder into a simple fixed-size array of names ---- */

static char rom_names[MAX_ROM_ENTRIES][256];
static int rom_count = 0;

static void scan_roms_folder(void)
{
    rom_count = 0;
    SceUID dir = sceIoDopen(ROMS_DIR);
    if (dir < 0)
        return;

    SceIoDirent entry;
    memset(&entry, 0, sizeof(entry));
    while (sceIoDread(dir, &entry) > 0 && rom_count < MAX_ROM_ENTRIES)
    {
        /* skip directories, only list files */
        if (!SCE_S_ISDIR(entry.d_stat.st_mode))
        {
            strncpy(rom_names[rom_count], entry.d_name, sizeof(rom_names[0]) - 1);
            rom_names[rom_count][sizeof(rom_names[0]) - 1] = '\0';
            rom_count++;
        }
        memset(&entry, 0, sizeof(entry));
    }
    sceIoDclose(dir);
}

/* ---- find the first .bin BIOS file across the 3 region folders,
   checked in this order: Japan, USA, Europe. Returns 1 and fills
   out_path if found, 0 otherwise (caller falls back to HLE bios). ---- */

static int find_first_bios(char *out_path, size_t out_len)
{
    const char *dirs[] = { BIOS_JP_DIR, BIOS_US_DIR, BIOS_EU_DIR };
    for (int d = 0; d < 3; d++)
    {
        SceUID dir = sceIoDopen(dirs[d]);
        if (dir < 0) continue;

        SceIoDirent entry;
        memset(&entry, 0, sizeof(entry));
        while (sceIoDread(dir, &entry) > 0)
        {
            if (!SCE_S_ISDIR(entry.d_stat.st_mode) && has_extension(entry.d_name, ".bin"))
            {
                snprintf(out_path, out_len, "%s/%s", dirs[d], entry.d_name);
                sceIoDclose(dir);
                return 1;
            }
            memset(&entry, 0, sizeof(entry));
        }
        sceIoDclose(dir);
    }
    return 0;
}

/* ---- simple debounced button read: returns which direction/button
   was newly pressed this frame (not held from last frame) ---- */

typedef struct {
    int up, down, cross, start;
} MenuInput;

static SceCtrlData s_prev_pad;

static MenuInput read_menu_input(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);

    MenuInput mi;
    mi.up     = (pad.buttons & SCE_CTRL_UP)     && !(s_prev_pad.buttons & SCE_CTRL_UP);
    mi.down   = (pad.buttons & SCE_CTRL_DOWN)   && !(s_prev_pad.buttons & SCE_CTRL_DOWN);
    mi.cross  = (pad.buttons & SCE_CTRL_CROSS)  && !(s_prev_pad.buttons & SCE_CTRL_CROSS);
    mi.start  = (pad.buttons & SCE_CTRL_START)  && !(s_prev_pad.buttons & SCE_CTRL_START);

    s_prev_pad = pad;
    return mi;
}

/* ---- the ROM browser screen. Returns the chosen full path in
   out_path, or returns 0 if the user picked something unsupported
   and we should just keep browsing. ---- */

static int run_rom_browser(char *out_path, size_t out_len)
{
    scan_roms_folder();
    int selected = 0;

    while (1)
    {
        printf("\e[2J\e[H");
        printf("YabauseVita - Phase 2\n");
        printf("========================================\n");

        if (rom_count == 0)
        {
            printf("\nNo files found in:\n  %s\n\n", ROMS_DIR);
            printf("Copy a .iso or .cue+.bin game there via\n");
            printf("FTP or USB, then restart the app.\n");
            sceKernelDelayThread(3 * 1000 * 1000);
            return 0;
        }

        printf("Select a game (UP/DOWN, X to confirm):\n\n");
        for (int i = 0; i < rom_count; i++)
        {
            RomSupportLevel lvl = classify_rom(rom_names[i]);
            const char *marker = (i == selected) ? "> " : "  ";
            const char *tag = "";
            if (lvl == ROM_UNSUPPORTED_CCD) tag = "  [CCD not supported yet]";
            else if (lvl == ROM_UNSUPPORTED_CHD) tag = "  [CHD not supported yet]";
            else if (lvl == ROM_UNSUPPORTED_7Z) tag = "  [extract .7z on PC first]";
            else if (lvl == ROM_UNKNOWN) tag = "  [unknown format]";
            printf("%s%s%s\n", marker, rom_names[i], tag);
        }

        MenuInput mi = read_menu_input();
        if (mi.up)   { selected--; if (selected < 0) selected = rom_count - 1; }
        if (mi.down) { selected++; if (selected >= rom_count) selected = 0; }

        if (mi.cross)
        {
            RomSupportLevel lvl = classify_rom(rom_names[selected]);
            if (lvl == ROM_SUPPORTED)
            {
                snprintf(out_path, out_len, "%s/%s", ROMS_DIR, rom_names[selected]);
                return 1;
            }
            else
            {
                printf("\e[2J\e[H");
                printf("This format isn't supported yet.\n\n");
                if (lvl == ROM_UNSUPPORTED_CCD)
                    printf("CCD/IMG/SUB (CloneCD) needs a new reader\n"
                           "that doesn't exist in this build yet.\n");
                else if (lvl == ROM_UNSUPPORTED_CHD)
                    printf("CHD needs the libchdr library, which\n"
                           "isn't linked into this build yet.\n");
                else if (lvl == ROM_UNSUPPORTED_7Z)
                    printf(".7z is a compressed archive. Extract it\n"
                           "on your PC and copy the .iso/.cue+.bin\n"
                           "here instead.\n");
                else
                    printf("Try a .iso or .cue+.bin file instead.\n");
                printf("\nPress X to go back.\n");

                while (1)
                {
                    MenuInput mi2 = read_menu_input();
                    if (mi2.cross) break;
                    sceDisplayWaitVblankStart();
                }
            }
        }

        sceDisplayWaitVblankStart();
    }
}

/* ---- entry point ---- */

int main(int argc, char *argv[])
{
    psvDebugScreenInit();
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
    memset(&s_prev_pad, 0, sizeof(s_prev_pad));

    printf("YabauseVita starting...\n");
    ensure_folders_exist();

    /* still keep our own dark-blue raw framebuffer ready for when
       VIDCORE_SOFT starts drawing real Saturn frames later */
    vita_fb = malloc(VITA_SCREEN_W * VITA_SCREEN_H * sizeof(uint32_t));
    if (vita_fb == NULL)
    {
        printf("FATAL: could not allocate framebuffer\n");
        sceKernelDelayThread(3 * 1000 * 1000);
        sceKernelExitProcess(0);
        return 0;
    }

    char rom_path[MAX_PATH_LEN];
    if (!run_rom_browser(rom_path, sizeof(rom_path)))
    {
        /* empty roms folder case already showed a message; just exit */
        sceKernelExitProcess(0);
        return 0;
    }

    char bios_path[MAX_PATH_LEN];
    int have_bios = find_first_bios(bios_path, sizeof(bios_path));

    printf("\e[2J\e[H");
    printf("Loading: %s\n", rom_path);
    if (have_bios)
        printf("BIOS:    %s\n", bios_path);
    else
        printf("BIOS:    none found -- using HLE BIOS\n"
               "(put a .bin BIOS in bios/jp, bios/us or\n"
               "bios/eu for better compatibility)\n");
    printf("\nThis will be SLOW (interpreter core, no\n"
             "GPU acceleration yet) -- that's expected.\n");
    sceKernelDelayThread(2 * 1000 * 1000);

    yabauseinit_struct yinit;
    memset(&yinit, 0, sizeof(yinit));
    yinit.percoretype   = PERCORE_DUMMY;
    yinit.sh2coretype   = SH2CORE_INTERPRETER;
    yinit.vidcoretype   = VIDCORE_SOFT;
    yinit.sndcoretype   = SNDCORE_DUMMY;
    yinit.m68kcoretype  = 0;
    yinit.cdcoretype    = CDCORE_ISO;
    yinit.carttype      = 0;
    yinit.regionid      = 0;
    yinit.biospath      = have_bios ? bios_path : NULL;
    yinit.cdpath        = rom_path
    yinit.buppath       = NULL;
    yinit.mpegpath      = NULL;
    yinit.cartpath      = NULL
    yinit.netlinksetting = NULL
    yinit.flags         = 0
    yinit.frameskip     = 0

    int ret = YabauseInit(&yinit);
    if (ret != 0)
    {
        printf("\e[2J\e[H");
        printf("YabauseInit failed (code %d).\n", ret);
        printf("Check that the file is a valid .iso or\n");
        printf(".cue with its matching .bin next to it.\n");
        while (1) sceDisplayWaitVblankStart();
    }

    while (1)
    {
        YabauseExec();
    }

    return 0;
}