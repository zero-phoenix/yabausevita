/*  src/vita/vita_ui.h: YabauseVita UI header
    Copyright 2026 YabauseVita port

    Tab-based user interface with:
      - Tab 1: ROM selection (.cue, .bin, .iso, .chd listed; .chd shows conversion note)
      - Tab 2: BIOS auto-detection (scans bios folder, detects region by filename)
      - Tab 3: Configuration (region override, frameskip, audio toggle)
    Plus a brief splash/intro screen on startup.
*/

#ifndef VITA_UI_H
#define VITA_UI_H

#include <psp2/ctrl.h>

/* ---- Color theme (ABGR for vita2d RGBA8 macro) ---- */
#define UI_COLOR_BG          RGBA8(0,   0,   0,   255)   /* black background   */
#define UI_COLOR_ACCENT      RGBA8(0,   85,  255, 255)   /* bright blue accent */
#define UI_COLOR_DARK_BLUE   RGBA8(0,   34,  85,  255)   /* dark blue panels   */
#define UI_COLOR_TAB_ACTIVE  RGBA8(0,   100, 220, 255)   /* active tab highlight */
#define UI_COLOR_TAB_INACT   RGBA8(30,  50,  80,  255)   /* inactive tab       */
#define UI_COLOR_TEXT         RGBA8(255, 255, 255, 255)   /* white text         */
#define UI_COLOR_SELECT      RGBA8(0,   200, 255, 255)   /* cyan selection     */
#define UI_COLOR_OK          RGBA8(0,   255, 128, 255)   /* green = OK/Found   */
#define UI_COLOR_WARN        RGBA8(255, 200, 0,   255)   /* yellow = warning   */
#define UI_COLOR_ERR         RGBA8(255, 60,  60,  255)   /* red = error/missing */

/* ---- Tab identifiers ---- */
#define TAB_ROMS      0
#define TAB_BIOS      1
#define TAB_CONFIG   2
#define TAB_COUNT     3

/* ---- Constants ---- */
#define MAX_FILES       200
#define MAX_PATH_LEN    512
#define MAX_BIOS_ENTRIES 16
#define SPLASH_DURATION_MS 2500   /* 2.5 seconds */

/* ---- BIOS region info ---- */
typedef struct {
    char filename[256];
    char fullpath[MAX_PATH_LEN];
    int  region;          /* 0=unknown, REGION_JAPAN=1, REGION_NORTHAMERICA=4, REGION_EUROPE=12 */
    int  detected;        /* 1 if region was detected from filename */
    const char *region_name;  /* "JAPAN", "USA", "EUROPE", or "UNKNOWN" */
} bios_entry_t;

/* ---- Global configuration shared between UI and main ---- */
typedef struct {
    /* Selected ROM */
    char rom_path[MAX_PATH_LEN];
    int  rom_selected;

    /* BIOS */
    char bios_path[MAX_PATH_LEN];
    int  bios_region;      /* REGION_AUTODETECT or a specific region constant */

    /* Settings */
    int  region_override;   /* 0=auto, or REGION_JAPAN/NORTHAMERICA/EUROPE */
    int  frameskip;
    int  audio_enabled;

    /* CHD info */
    int  selected_is_chd;  /* 1 if the selected ROM is a .chd file */
} vita_config_t;

/* ---- Public functions ---- */

/* Initialize vita2d, fonts, and create data directories.
   Must call vita2d_init() BEFORE this function. */
int init_vita_ui(void);

/* Cleanup vita2d resources */
void cleanup_vita_ui(void);

/* Draw the splash/intro screen. Blocks for SPLASH_DURATION_MS.
   Returns 1 if user pressed X to skip, 0 on timeout. */
int draw_splash(void);

/* Run the main UI loop (tabs). Returns when user selects START to launch.
   Populates cfg with selected ROM path, BIOS path, and settings.
   Returns 1 if user chose to start emulation, 0 if user pressed O to quit. */
int vita_ui_main_loop(vita_config_t *cfg);

/* Scan the BIOS folder and populate bios_entries array.
   Returns number of entries found. */
int scan_bios_files(void);

/* Get the current BIOS entries (for drawing) */
bios_entry_t *get_bios_entries(int *count);

#endif /* VITA_UI_H */
