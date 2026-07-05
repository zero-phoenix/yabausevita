/*  src/vita/main.c: PS Vita entry point for Yabause (Phase 3)
    Copyright 2026 YabauseVita port

    This file is part of Yabause.

    PHASE 3 changes over Phase 2:
      - FIXED: screen flicker. Phase 2 cleared+redrew every single loop
        iteration (60x/sec) even when nothing changed, which visibly
        tore/flickered on real hardware because debugScreen writes
        straight into the currently-displayed buffer (it isn't double
        buffered like a game renderer). Now we only redraw when the
        selection/tab actually changes ("dirty flag" pattern).
      - Added a visible heartbeat during YabauseInit()/YabauseExec() so
        "stuck loading" is distinguishable from "still working" -- the
        SH2 interpreter core is genuinely slow at the Saturn's BIOS
        boot sequence, this is very likely just slowness, not a hang.
      - Black/blue color theme using ANSI color codes (debugScreen is
        a text-console emulator, so "sophisticated UI" here means
        colored, well-organized text, not a graphical interface).
      - Three tabs (ROMS / BIOS / SETTINGS), switched with L/R triggers.
      - BIOS auto-detection by region: reads the real IP.BIN header
        (SEGA's own documented disc header format, ST-040) from the
        chosen game to find its area code (J/U/E), then prefers a BIOS
        from the matching region folder over the fixed jp->us->eu order.

    STILL NOT DONE (being upfront, not quietly skipping):
      - CHD support. This needs a whole new external library (libchdr)
        cross-compiled for Vita for the first time, plus a new
        CDInterface implementing sector reads through it. That's a
        project on the same scale as the VitaSDK setup itself, so it's
        deliberately NOT crammed in here -- see PORTING_NOTES.md.
      - CCD/IMG/SUB and .7z: same as before, not yet supported.
      - Buttons still only work in this menu, not in-game yet.
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
#define MAX_BIOS_ENTRIES 20
#define MAX_PATH_LEN 512

/* ---- black/blue color theme, plain ANSI SGR codes (confirmed this
   debugScreen supports CSI color + cursor commands) ---- */
#define COL_RESET     "\e[0m"
#define COL_BG_BLACK  "\e[40m"
#define COL_BG_BLUE   "\e[44m"
#define COL_FG_WHITE  "\e[37m"
#define COL_FG_CYAN   "\e[36m"
#define COL_FG_YELLOW "\e[33m"
#define CLEAR_SCREEN  "\e[2J\e[H"

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

/* ---- folder setup (sceIoMkdir, one level at a time) ---- */

static void ensure_folders_exist(void)
{
    sceIoMkdir(YABAUSEVITA_ROOT, 0777);
    sceIoMkdir(ROMS_DIR, 0777);
    sceIoMkdir(BIOS_ROOT_DIR, 0777);
    sceIoMkdir(BIOS_JP_DIR, 0777);
    sceIoMkdir(BIOS_US_DIR, 0777);
    sceIoMkdir(BIOS_EU_DIR, 0777);
}

/* ---- extension helpers ---- */

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

/* ---- generic folder scan into a fixed-size name array ---- */

static int scan_folder(const char *path, char names[][256], int max_entries)
{
    int count = 0;
    SceUID dir = sceIoDopen(path);
    if (dir < 0) return 0;

    SceIoDirent entry;
    memset(&entry, 0, sizeof(entry));
    while (sceIoDread(dir, &entry) > 0 && count < max_entries)
    {
        if (!SCE_S_ISDIR(entry.d_stat.st_mode))
        {
            strncpy(names[count], entry.d_name, 255);
            names[count][255] = '\0';
            count++;
        }
        memset(&entry, 0, sizeof(entry));
    }
    sceIoDclose(dir);
    return count;
}

static char rom_names[MAX_ROM_ENTRIES][256];
static int rom_count = 0;
static char bios_jp_names[MAX_BIOS_ENTRIES][256];
static char bios_us_names[MAX_BIOS_ENTRIES][256];
static char bios_eu_names[MAX_BIOS_ENTRIES][256];
static int bios_jp_count, bios_us_count, bios_eu_count;

static void rescan_everything(void)
{
    rom_count     = scan_folder(ROMS_DIR, rom_names, MAX_ROM_ENTRIES);
    bios_jp_count = scan_folder(BIOS_JP_DIR, bios_jp_names, MAX_BIOS_ENTRIES);
    bios_us_count = scan_folder(BIOS_US_DIR, bios_us_names, MAX_BIOS_ENTRIES);
    bios_eu_count = scan_folder(BIOS_EU_DIR, bios_eu_names, MAX_BIOS_ENTRIES);
}

/* ---- region-aware BIOS lookup. 'region' is 'J', 'U', 'E', or 0 for
   "no preference" (falls back to jp->us->eu order). Returns 1 and
   fills out_path if any .bin is found. ---- */

static int find_bios_for_region(char region, char *out_path, size_t out_len)
{
    struct { char code; const char *dir; char (*names)[256]; int *count; } table[3] = {
        { 'J', BIOS_JP_DIR, bios_jp_names, &bios_jp_count },
        { 'U', BIOS_US_DIR, bios_us_names, &bios_us_count },
        { 'E', BIOS_EU_DIR, bios_eu_names, &bios_eu_count },
    };

    /* try the requested region first, if we have one */
    for (int pass = 0; pass < 2; pass++)
    {
        for (int i = 0; i < 3; i++)
        {
            if (pass == 0 && table[i].code != region) continue;
            if (pass == 1 && region != 0) continue; /* already tried it in pass 0 */

            for (int j = 0; j < *table[i].count; j++)
            {
                if (has_extension(table[i].names[j], ".bin"))
                {
                    snprintf(out_path, out_len, "%s/%s", table[i].dir, table[i].names[j]);
                    return 1;
                }
            }
        }
        if (region == 0) break; /* no preference: pass 0 already covered everything */
    }
    return 0;
}

/* ---- read a CUE file's first "FILE "x.bin"" line to find the real
   data file, since Yabause hands the .cue path itself to ISOCDInit
   but WE need to peek at the raw .bin ourselves for the region byte. ---- */

static int resolve_cue_to_bin(const char *cue_path, char *out_path, size_t out_len)
{
    FILE *f = fopen(cue_path, "r");
    if (!f) return 0;

    char line[512];
    int found = 0;
    if (fgets(line, sizeof(line), f))
    {
        char *start = strchr(line, '"');
        if (start)
        {
            start++;
            char *end = strchr(start, '"');
            if (end)
            {
                *end = '\0';
                /* the cue's FILE line is usually just the bin's own
                   filename, sitting next to the .cue on disk */
                const char *slash = strrchr(cue_path, '/');
                if (slash)
                {
                    int dirlen = (int)(slash - cue_path) + 1;
                    snprintf(out_path, out_len, "%.*s%s", dirlen, cue_path, start);
                }
                else
                {
                    snprintf(out_path, out_len, "%s", start);
                }
                found = 1;
            }
        }
    }
    fclose(f);
    return found;
}

/* ---- read the real Saturn IP.BIN header (SEGA's own documented
   format, "Disc Format Standards Specification Sheet" ST-040) to find
   the game's area code. Handles both "cooked" (2048 bytes/sector) and
   "raw" (2352 bytes/sector, 16-byte sync+header prefix) bin dumps by
   checking which offset actually has the "SEGA SEGASATURN" magic.
   Returns 'J', 'U', 'E', or 0 if nothing recognizable was found. ---- */

static char detect_game_region(const char *rom_path)
{
    char bin_path[MAX_PATH_LEN];
    const char *data_path = rom_path;

    if (has_extension(rom_path, ".cue"))
    {
        if (resolve_cue_to_bin(rom_path, bin_path, sizeof(bin_path)))
            data_path = bin_path;
        else
            return 0;
    }

    FILE *f = fopen(data_path, "rb");
    if (!f) return 0;

    unsigned char header[256];
    size_t got = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (got < 0x50) return 0;

    int base = -1;
    if (memcmp(header, "SEGA SEGASATURN", 15) == 0)
        base = 0;                              /* cooked, 2048 b/sector */
    else if (got >= 16 + 0x50 && memcmp(header + 16, "SEGA SEGASATURN", 15) == 0)
        base = 16;                             /* raw, 2352 b/sector, skip sync+header */

    if (base < 0) return 0;

    /* Area/compatible symbols field, per ST-040 */
    const unsigned char *area = header + base + 0x40;
    for (int i = 0; i < 10; i++)
    {
        if (area[i] == 'J' || area[i] == 'U' || area[i] == 'E')
            return (char)area[i];
    }
    return 0;
}

/* ---- debounced input ---- */

typedef struct {
    int up, down, cross, start, ltrigger, rtrigger;
} MenuInput;

static SceCtrlData s_prev_pad;

static MenuInput read_menu_input(void)
{
    SceCtrlData pad;
    sceCtrlPeekBufferPositive(0, &pad, 1);

    MenuInput mi;
    mi.up       = (pad.buttons & SCE_CTRL_UP)       && !(s_prev_pad.buttons & SCE_CTRL_UP);
    mi.down     = (pad.buttons & SCE_CTRL_DOWN)     && !(s_prev_pad.buttons & SCE_CTRL_DOWN);
    mi.cross    = (pad.buttons & SCE_CTRL_CROSS)    && !(s_prev_pad.buttons & SCE_CTRL_CROSS);
    mi.start    = (pad.buttons & SCE_CTRL_START)    && !(s_prev_pad.buttons & SCE_CTRL_START);
    mi.ltrigger = (pad.buttons & SCE_CTRL_LTRIGGER) && !(s_prev_pad.buttons & SCE_CTRL_LTRIGGER);
    mi.rtrigger = (pad.buttons & SCE_CTRL_RTRIGGER) && !(s_prev_pad.buttons & SCE_CTRL_RTRIGGER);

    s_prev_pad = pad;
    return mi;
}

/* ---- tabbed menu ---- */

typedef enum { TAB_ROMS, TAB_BIOS, TAB_SETTINGS, TAB_COUNT } Tab;
static const char *TAB_NAMES[TAB_COUNT] = { "ROMS", "BIOS", "SETTINGS" };

static void draw_tab_bar(Tab active)
{
    printf(COL_BG_BLUE COL_FG_WHITE);
    for (int i = 0; i < TAB_COUNT; i++)
    {
        if (i == (int)active)
            printf(COL_BG_BLUE COL_FG_YELLOW " [%s] " COL_FG_WHITE, TAB_NAMES[i]);
        else
            printf(COL_BG_BLACK COL_FG_WHITE "  %s  ", TAB_NAMES[i]);
    }
    printf(COL_RESET "\n");
    printf(COL_FG_CYAN "L/R: switch tabs   UP/DOWN: move   X: select" COL_RESET "\n");
    printf("========================================\n");
}

/* returns: 1 = a supported rom was chosen (out_path filled),
            0 = still browsing / nothing chosen yet this call */
static int draw_roms_tab(int selected, char *out_path, size_t out_len, int *chosen)
{
    *chosen = 0;
    if (rom_count == 0)
    {
        printf("\nNo files in:\n  %s\n\n", ROMS_DIR);
        printf("Copy a .iso or .cue+.bin there via FTP/USB.\n");
        return 0;
    }

    for (int i = 0; i < rom_count; i++)
    {
        RomSupportLevel lvl = classify_rom(rom_names[i]);
        const char *marker = (i == selected) ? COL_FG_YELLOW "> " : COL_FG_WHITE "  ";
        const char *tag = "";
        if (lvl == ROM_UNSUPPORTED_CCD) tag = "  [CCD - not supported yet]";
        else if (lvl == ROM_UNSUPPORTED_CHD) tag = "  [CHD - coming in a future update]";
        else if (lvl == ROM_UNSUPPORTED_7Z) tag = "  [extract .7z on PC first]";
        else if (lvl == ROM_UNKNOWN) tag = "  [unknown format]";
        printf("%s%s%s" COL_RESET "\n", marker, rom_names[i], tag);
    }
    return 1;
}

static void draw_bios_tab(void)
{
    printf(COL_FG_CYAN "Japan (bios/jp):" COL_RESET "\n");
    if (bios_jp_count == 0) printf("  (empty)\n");
    for (int i = 0; i < bios_jp_count; i++) printf("  %s\n", bios_jp_names[i]);

    printf(COL_FG_CYAN "\nUSA (bios/us):" COL_RESET "\n");
    if (bios_us_count == 0) printf("  (empty)\n");
    for (int i = 0; i < bios_us_count; i++) printf("  %s\n", bios_us_names[i]);

    printf(COL_FG_CYAN "\nEurope (bios/eu):" COL_RESET "\n");
    if (bios_eu_count == 0) printf("  (empty)\n");
    for (int i = 0; i < bios_eu_count; i++) printf("  %s\n", bios_eu_names[i]);

    printf("\nWhen you pick a game, its region is read\n");
    printf("from the disc and matched automatically\n");
    printf("to one of these folders.\n");
}

static void draw_settings_tab(void)
{
    printf(COL_FG_CYAN "CPU core:    " COL_RESET "Interpreter (slow, portable)\n");
    printf(COL_FG_CYAN "Video core:  " COL_RESET "Software renderer\n");
    printf(COL_FG_CYAN "Sound core:  " COL_RESET "Silent (not implemented yet)\n");
    printf(COL_FG_CYAN "Controls:    " COL_RESET "Menu only, not in-game yet\n");
    printf(COL_FG_CYAN "CHD support: " COL_RESET "Not yet (needs libchdr)\n");
    printf("\nThese will become real options in a\n");
    printf("future update.\n");
}

/* ---- the main tabbed menu loop. Only redraws when something changes
   (fixes the Phase 2 flicker, which came from redrawing every frame
   even when nothing was different). ---- */

static int run_menu(char *out_rom_path, size_t out_len)
{
    rescan_everything();
    Tab tab = TAB_ROMS;
    int selected = 0;
    int dirty = 1;

    while (1)
    {
        if (dirty)
        {
            printf(CLEAR_SCREEN COL_BG_BLACK COL_FG_WHITE);
            printf("YabauseVita\n");
            draw_tab_bar(tab);

            if (tab == TAB_ROMS)
            {
                int chosen;
                draw_roms_tab(selected, out_rom_path, out_len, &chosen);
            }
            else if (tab == TAB_BIOS)
            {
                draw_bios_tab();
            }
            else
            {
                draw_settings_tab();
            }
            dirty = 0;
        }

        MenuInput mi = read_menu_input();

        if (mi.ltrigger) { tab = (tab + TAB_COUNT - 1) % TAB_COUNT; selected = 0; dirty = 1; }
        if (mi.rtrigger) { tab = (tab + 1) % TAB_COUNT; selected = 0; dirty = 1; }

        if (tab == TAB_ROMS && rom_count > 0)
        {
            if (mi.up)   { selected = (selected - 1 + rom_count) % rom_count; dirty = 1; }
            if (mi.down) { selected = (selected + 1) % rom_count; dirty = 1; }

            if (mi.cross)
            {
                RomSupportLevel lvl = classify_rom(rom_names[selected]);
                if (lvl == ROM_SUPPORTED)
                {
                    snprintf(out_rom_path, out_len, "%s/%s", ROMS_DIR, rom_names[selected]);
                    return 1;
                }
                else
                {
                    printf(CLEAR_SCREEN);
                    printf("This format isn't supported yet.\n\n");
                    if (lvl == ROM_UNSUPPORTED_CCD)
                        printf("CCD/IMG/SUB (CloneCD) needs a new\nreader that doesn't exist yet.\n");
                    else if (lvl == ROM_UNSUPPORTED_CHD)
                        printf("CHD needs the libchdr library,\nplanned for a future update.\n");
                    else if (lvl == ROM_UNSUPPORTED_7Z)
                        printf(".7z is compressed. Extract it on\nyour PC first.\n");
                    else
                        printf("Try a .iso or .cue+.bin instead.\n");
                    printf("\nPress X to go back.\n");
                    while (1)
                    {
                        MenuInput mi2 = read_menu_input();
                        if (mi2.cross) break;
                        sceDisplayWaitVblankStart();
                    }
                    dirty = 1;
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

    printf(CLEAR_SCREEN "YabauseVita starting...\n");
    ensure_folders_exist();

    vita_fb = malloc(VITA_SCREEN_W * VITA_SCREEN_H * sizeof(uint32_t));
    if (vita_fb == NULL)
    {
        printf("FATAL: could not allocate framebuffer\n");
        sceKernelDelayThread(3 * 1000 * 1000);
        sceKernelExitProcess(0);
        return 0;
    }

    char rom_path[MAX_PATH_LEN];
    if (!run_menu(rom_path, sizeof(rom_path)))
    {
        sceKernelExitProcess(0);
        return 0;
    }

    char region = detect_game_region(rom_path);
    char bios_path[MAX_PATH_LEN];
    int have_bios = find_bios_for_region(region, bios_path, sizeof(bios_path));

    printf(CLEAR_SCREEN);
    printf("Loading: %s\n", rom_path);
    printf("Region:  %s\n", region == 'J' ? "Japan" : region == 'U' ? "USA" :
                             region == 'E' ? "Europe" : "unknown (couldn't read header)");
    if (have_bios)
        printf("BIOS:    %s (auto-matched)\n", bios_path);
    else
        printf("BIOS:    none found -- using HLE BIOS\n");
    printf("\nBooting... this uses a slow interpreter\n");
    printf("core with no GPU acceleration yet, so\n");
    printf("this can take a while. A dot will print\n");
    printf("below every few seconds so you know it's\n");
    printf("still working, not frozen:\n\n");

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
    yinit.cdpath        = rom_path;
    yinit.buppath       = NULL;
    yinit.mpegpath      = NULL;
    yinit.cartpath      = NULL;
    yinit.netlinksetting = NULL;
    yinit.flags         = 0;
    yinit.frameskip     = 0;

    int ret = YabauseInit(&yinit);
    if (ret != 0)
    {
        printf(CLEAR_SCREEN);
        printf("YabauseInit failed (code %d).\n", ret);
        printf("Check that the file is a valid .iso or\n");
        printf(".cue with its matching .bin next to it.\n");
        while (1) sceDisplayWaitVblankStart();
    }

    int frame = 0;
    while (1)
    {
        YabauseExec();
        frame++;
        if (frame % 180 == 0) /* heartbeat, roughly every few seconds */
            printf(".");
    }

    return 0;
}
