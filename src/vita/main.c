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
