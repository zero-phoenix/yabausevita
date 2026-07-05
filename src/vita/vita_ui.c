/*  src/vita/vita_ui.c: YabauseVita tab-based UI
    Copyright 2026 YabauseVita port

    Black & blue themed user interface for the Yabause Saturn emulator
    on PS Vita. Built on vita2d.

    Three tabs:
      ROMs   - lists .cue/.bin/.iso/.chd from ux0:data/yabause/roms/
      BIOS   - scans ux0:data/yabause/bios/ and detects region by filename
      Config - region override, frameskip, audio toggle

    Controls:
      DPAD UP/DOWN : navigate list
      L / R        : switch tabs (left/right)
      X            : select / confirm
      O            : back / cancel
      START        : launch emulation (from ROMs tab)
      TRIANGLE     : info
*/

#include "vita_ui.h"

#include <psp2/ctrl.h>
#include <psp2/display.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/io/stat.h>
#include <vita2d.h>
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

/* Region constants from smpc.h (re-declared here to avoid pulling full header) */
#define SMPC_REGION_AUTODETECT     0
#define SMPC_REGION_JAPAN          1
#define SMPC_REGION_NORTHAMERICA   4
#define SMPC_REGION_EUROPE         12

#define ROMS_DIR  "ux0:data/yabause/roms"
#define BIOS_DIR  "ux0:data/yabause/bios"

/* ---- State ---- */
static vita2d_pgf *g_pgf = NULL;
static int g_ui_initialized = 0;

/* ROM listing */
static char g_rom_files[MAX_FILES][256];
static int  g_rom_count = 0;
static int  g_rom_selected = 0;

/* BIOS listing */
static bios_entry_t g_bios_entries[MAX_BIOS_ENTRIES];
static int g_bios_count = 0;
static int g_bios_selected = 0;

/* Current tab */
static int g_current_tab = TAB_ROMS;

/* Config state (defaults) */
static int g_region_index = 0;  /* 0=Auto,1=JAP,2=USA,3=EUR */
static int g_frameskip = 0;
static int g_audio_enabled = 1;

/* ---- Helpers ---- */

/* Case-insensitive substring search */
static int strcasestr_local(const char *haystack, const char *needle)
{
    size_t hlen = strlen(haystack);
    size_t nlen = strlen(needle);
    if (nlen == 0 || hlen < nlen) return 0;
    for (size_t i = 0; i <= hlen - nlen; i++)
    {
        size_t j;
        for (j = 0; j < nlen; j++)
        {
            if (tolower((unsigned char)haystack[i + j]) != tolower((unsigned char)needle[j]))
                break;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/* Check if filename ends with given extension (case-insensitive) */
static int has_ext(const char *name, const char *ext)
{
    size_t nlen = strlen(name);
    size_t elen = strlen(ext);
    if (nlen < elen) return 0;
    for (size_t i = 0; i < elen; i++)
    {
        if (tolower((unsigned char)name[nlen - elen + i]) != tolower((unsigned char)ext[i]))
            return 0;
    }
    return 1;
}

/* Detect BIOS region from filename keywords.
   Returns one of the SMPC_REGION_* constants, or 0 if unknown. */
static int detect_bios_region(const char *filename, const char **out_name)
{
    if (strcasestr_local(filename, "jap") || strcasestr_local(filename, "japan"))
    {
        if (out_name) *out_name = "JAPAN";
        return SMPC_REGION_JAPAN;
    }
    if (strcasestr_local(filename, "usa") || strcasestr_local(filename, "us.bin") ||
        strcasestr_local(filename, "north") || strcasestr_local(filename, "_us"))
    {
        if (out_name) *out_name = "USA";
        return SMPC_REGION_NORTHAMERICA;
    }
    if (strcasestr_local(filename, "eur") || strcasestr_local(filename, "euro") ||
        strcasestr_local(filename, "pal") || strcasestr_local(filename, "_eu"))
    {
        if (out_name) *out_name = "EUROPE";
        return SMPC_REGION_EUROPE;
    }
    if (out_name) *out_name = "UNKNOWN";
    return 0;
}

/* Is this a supported ROM extension? */
static int is_rom_file(const char *name)
{
    return has_ext(name, ".cue") || has_ext(name, ".bin") ||
           has_ext(name, ".iso") || has_ext(name, ".chd");
}

/* Is this a BIOS file? (any .bin file in the bios folder counts) */
static int is_bios_file(const char *name)
{
    return has_ext(name, ".bin") || has_ext(name, ".rom");
}

/* ---- File scanning ---- */

static void scan_rom_files(void)
{
    g_rom_count = 0;
    DIR *d = opendir(ROMS_DIR);
    if (!d) return;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL && g_rom_count < MAX_FILES)
    {
        if (dir->d_name[0] == '.') continue;
        if (!is_rom_file(dir->d_name)) continue;
        strncpy(g_rom_files[g_rom_count], dir->d_name, 255);
        g_rom_files[g_rom_count][255] = '\0';
        g_rom_count++;
    }
    closedir(d);
}

int scan_bios_files(void)
{
    g_bios_count = 0;
    DIR *d = opendir(BIOS_DIR);
    if (!d) return 0;

    struct dirent *dir;
    while ((dir = readdir(d)) != NULL && g_bios_count < MAX_BIOS_ENTRIES)
    {
        if (dir->d_name[0] == '.') continue;
        if (!is_bios_file(dir->d_name)) continue;

        bios_entry_t *e = &g_bios_entries[g_bios_count];
        strncpy(e->filename, dir->d_name, 255);
        e->filename[255] = '\0';
        snprintf(e->fullpath, MAX_PATH_LEN, "%s/%s", BIOS_DIR, dir->d_name);
        const char *rname = NULL;
        e->region = detect_bios_region(dir->d_name, &rname);
        e->region_name = rname ? rname : "UNKNOWN";
        e->detected = (e->region != 0) ? 1 : 0;
        g_bios_count++;
    }
    closedir(d);
    return g_bios_count;
}

bios_entry_t *get_bios_entries(int *count)
{
    if (count) *count = g_bios_count;
    return g_bios_entries;
}

/* ---- Initialization ---- */

int init_vita_ui(void)
{
    if (g_ui_initialized) return 1;

    /* Create data directories so the user can drop files in */
    sceIoMkdir("ux0:data/yabause", 0777);
    sceIoMkdir(ROMS_DIR, 0777);
    sceIoMkdir(BIOS_DIR, 0777);

    /* Load the default PGF font */
    g_pgf = vita2d_load_default_pgf();
    if (!g_pgf)
    {
        return 0;
    }

    /* Initial scans */
    scan_rom_files();
    scan_bios_files();

    g_ui_initialized = 1;
    return 1;
}

void cleanup_vita_ui(void)
{
    if (g_pgf)
    {
        vita2d_free_pgf(g_pgf);
        g_pgf = NULL;
    }
    g_ui_initialized = 0;
}

/* ---- Drawing helpers ---- */

static void draw_text(int x, int y, unsigned int color, float scale, const char *text)
{
    if (g_pgf && text)
        vita2d_pgf_draw_text(g_pgf, x, y, color, scale, text);
}

static void draw_textf(int x, int y, unsigned int color, float scale, const char *fmt, ...)
{
    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    draw_text(x, y, color, scale, buf);
}

static void fill_rect(int x, int y, int w, int h, unsigned int color)
{
    vita2d_draw_rectangle((float)x, (float)y, (float)w, (float)h, color);
}

/* Draw the title bar at the top */
static void draw_title_bar(void)
{
    fill_rect(0, 0, 960, 70, UI_COLOR_DARK_BLUE);
    fill_rect(0, 66, 960, 4, UI_COLOR_ACCENT);
    draw_text(30, 45, UI_COLOR_TEXT, 1.4f, "YABAUSE VITA");
    draw_text(820, 45, UI_COLOR_SELECT, 1.0f, "v0.10");
}

/* Draw the tab bar below the title. Returns the y where content starts. */
static int draw_tab_bar(void)
{
    const char *tab_names[TAB_COUNT] = { "JUEGOS", "BIOS", "CONFIG" };
    int tab_w = 960 / TAB_COUNT;
    int y = 70;
    int h = 50;

    for (int i = 0; i < TAB_COUNT; i++)
    {
        unsigned int color = (i == g_current_tab) ? UI_COLOR_TAB_ACTIVE : UI_COLOR_TAB_INACT;
        fill_rect(i * tab_w, y, tab_w, h, color);
        if (i == g_current_tab)
        {
            fill_rect(i * tab_w, y + h - 4, tab_w, 4, UI_COLOR_SELECT);
        }
        /* Center the tab label */
        char buf[64];
        snprintf(buf, sizeof(buf), "< %s >", tab_names[i]);
        draw_text(i * tab_w + tab_w / 2 - 60, y + 32, UI_COLOR_TEXT, 1.0f, buf);
    }
    return y + h;
}

/* Draw footer with control hints */
static void draw_footer(const char *hint)
{
    fill_rect(0, 510, 960, 34, UI_COLOR_DARK_BLUE);
    fill_rect(0, 510, 960, 3, UI_COLOR_ACCENT);
    if (hint)
        draw_text(20, 532, UI_COLOR_TEXT, 0.8f, hint);
}

/* ---- Tab: ROMs ---- */

static void draw_roms_tab(int content_y)
{
    draw_textf(30, content_y + 30, UI_COLOR_TEXT, 1.0f,
               "Carpeta: ux0:data/yabause/roms/   (%d archivos)", g_rom_count);

    if (g_rom_count == 0)
    {
        draw_text(30, content_y + 100, UI_COLOR_WARN, 1.1f,
                  "No se encontraron ROMs.");
        draw_text(30, content_y + 140, UI_COLOR_TEXT, 1.0f,
                  "Copia tus juegos (.cue/.bin, .iso) a:");
        draw_text(30, content_y + 170, UI_COLOR_SELECT, 1.0f,
                  "ux0:data/yabause/roms/");
        draw_text(30, content_y + 220, UI_COLOR_WARN, 0.9f,
                  "Nota: formato .chd aparece en la lista pero NO es");
        draw_text(30, content_y + 245, UI_COLOR_WARN, 0.9f,
                  "ejecutable aun. Conviertelo a .cue/.bin con chdman.");
        return;
    }

    int list_y = content_y + 60;
    int line_h = 30;
    int max_visible = 12;
    int scroll = 0;
    if (g_rom_selected >= max_visible)
        scroll = g_rom_selected - max_visible + 1;

    for (int i = 0; i < g_rom_count && i < max_visible; i++)
    {
        int idx = i + scroll;
        if (idx >= g_rom_count) break;
        int y = list_y + i * line_h;

        unsigned int color = UI_COLOR_TEXT;
        if (idx == g_rom_selected)
        {
            fill_rect(20, y - 4, 920, line_h, UI_COLOR_DARK_BLUE);
            fill_rect(20, y - 4, 4, line_h, UI_COLOR_SELECT);
            color = UI_COLOR_SELECT;
        }

        /* Mark CHD files specially */
        if (has_ext(g_rom_files[idx], ".chd"))
        {
            draw_textf(40, y + 18, UI_COLOR_WARN, 0.9f,
                       "[CHD] %s  (no soportado)", g_rom_files[idx]);
        }
        else
        {
            draw_textf(40, y + 18, color, 0.9f, "%s", g_rom_files[idx]);
        }
    }

    draw_footer("X=Seleccionar  START=Iniciar  L/R=Pestañas  O=Salir");
}

/* ---- Tab: BIOS ---- */

static const char *region_id_to_name(int region)
{
    switch (region)
    {
        case SMPC_REGION_JAPAN:        return "JAPAN (NTSC-J)";
        case SMPC_REGION_NORTHAMERICA: return "USA (NTSC-U)";
        case SMPC_REGION_EUROPE:       return "EUROPE (PAL)";
        default:                       return "DESCONOCIDA";
    }
}

static void draw_bios_tab(int content_y)
{
    draw_text(30, content_y + 30, UI_COLOR_TEXT, 1.0f,
              "Auto-deteccion de BIOS");
    draw_text(30, content_y + 55, UI_COLOR_TEXT, 0.85f,
              "Carpeta: ux0:data/yabause/bios/");

    if (g_bios_count == 0)
    {
        draw_text(30, content_y + 120, UI_COLOR_ERR, 1.1f,
                  "No se encontraron archivos de BIOS.");
        draw_text(30, content_y + 160, UI_COLOR_TEXT, 1.0f,
                  "Copia tus BIOS de Saturn (sega_saturn_bios.bin, etc.) a:");
        draw_text(30, content_y + 190, UI_COLOR_SELECT, 1.0f,
                  "ux0:data/yabause/bios/");
        draw_text(30, content_y + 240, UI_COLOR_WARN, 0.85f,
                  "Sugerencia: nombra los archivos con la region para");
        draw_text(30, content_y + 265, UI_COLOR_WARN, 0.85f,
                  "auto-deteccion: bios_japan.bin, bios_usa.bin, bios_eur.bin");
        draw_text(30, content_y + 310, UI_COLOR_TEXT, 0.85f,
                  "Sin BIOS, se usara HLE BIOS (menos compatible).");
        return;
    }

    /* Header for the list */
    draw_text(30, content_y + 90, UI_COLOR_ACCENT, 0.85f,
              "Archivo                                Region        Estado");
    fill_rect(30, content_y + 98, 900, 2, UI_COLOR_DARK_BLUE);

    int list_y = content_y + 125;
    int line_h = 30;

    for (int i = 0; i < g_bios_count && i < 10; i++)
    {
        int y = list_y + i * line_h;
        bios_entry_t *e = &g_bios_entries[i];

        if (i == g_bios_selected)
        {
            fill_rect(20, y - 4, 920, line_h, UI_COLOR_DARK_BLUE);
            fill_rect(20, y - 4, 4, line_h, UI_COLOR_SELECT);
        }

        /* Filename (truncated) */
        char fname[40];
        strncpy(fname, e->filename, 36);
        fname[36] = '\0';
        if (strlen(e->filename) > 36) strcat(fname, "..");
        draw_text(40, y + 18, UI_COLOR_TEXT, 0.85f, fname);

        /* Region */
        unsigned int rcolor = e->detected ? UI_COLOR_OK : UI_COLOR_WARN;
        draw_text(380, y + 18, rcolor, 0.85f, e->region_name);

        /* Status */
        draw_text(560, y + 18, UI_COLOR_OK, 0.85f, "OK");
    }

    /* Show selected BIOS info */
    if (g_bios_count > 0 && g_bios_selected < g_bios_count)
    {
        bios_entry_t *e = &g_bios_entries[g_bios_selected];
        draw_textf(30, content_y + 360, UI_COLOR_TEXT, 0.8f,
                   "Seleccionada: %s", e->filename);
        draw_textf(30, content_y + 385, UI_COLOR_TEXT, 0.8f,
                   "Region: %s   Ruta: %s", region_id_to_name(e->region), e->fullpath);
    }

    draw_footer("X=Elegir BIOS  L/R=Pestañas  O=Salir");
}

/* ---- Tab: Config ---- */

static void draw_config_tab(int content_y)
{
    draw_text(30, content_y + 30, UI_COLOR_ACCENT, 1.1f, "Configuracion");

    /* Region override */
    draw_text(30, content_y + 80, UI_COLOR_TEXT, 1.0f, "Region:");
    const char *region_opts[] = { "Auto-detectar", "Japon (NTSC-J)",
                                  "USA (NTSC-U)", "Europa (PAL)" };
    for (int i = 0; i < 4; i++)
    {
        int y = content_y + 110 + i * 30;
        unsigned int color = (i == g_region_index) ? UI_COLOR_SELECT : UI_COLOR_TEXT;
        const char *mark = (i == g_region_index) ? "[*]" : "[ ]";
        draw_textf(50, y + 18, color, 0.95f, "%s %s", mark, region_opts[i]);
    }

    /* Frameskip */
    int fs_y = content_y + 250;
    draw_text(30, fs_y, UI_COLOR_TEXT, 1.0f, "Frameskip:");
    draw_textf(50, fs_y + 30, g_frameskip > 0 ? UI_COLOR_WARN : UI_COLOR_TEXT,
               0.95f, "Frameskip = %d  (mayor = mas rapido, menos fluido)", g_frameskip);

    /* Audio */
    int au_y = content_y + 320;
    draw_text(30, au_y, UI_COLOR_TEXT, 1.0f, "Audio:");
    draw_textf(50, au_y + 30,
               g_audio_enabled ? UI_COLOR_OK : UI_COLOR_WARN,
               0.95f, "Audio: %s",
               g_audio_enabled ? "Activado (SCSP)" : "Desactivado (silencio)");

    /* CHD info note */
    draw_text(30, content_y + 390, UI_COLOR_ACCENT, 0.9f, "Sobre formato CHD:");
    draw_text(50, content_y + 415, UI_COLOR_TEXT, 0.8f,
              "Los archivos .chd aparecen en la pestana JUEGOS pero no se");
    draw_text(50, content_y + 435, UI_COLOR_TEXT, 0.8f,
              "pueden ejecutar aun. Para convertir CHD a CUE/BIN usa:");
    draw_text(50, content_y + 455, UI_COLOR_SELECT, 0.8f,
              "  chdman extractcd -i juego.chd -o juego.cue");

    draw_footer("UP/DOWN=Navegar  X=Cambiar  L/R=Pestañas  O=Salir");
}

/* ---- Splash screen ---- */

int draw_splash(void)
{
    SceCtrlData pad;
    uint64_t start = sceKernelGetProcessTimeWide();
    uint64_t duration_us = SPLASH_DURATION_MS * 1000ULL;
    int skipped = 0;

    while (1)
    {
        uint64_t now = sceKernelGetProcessTimeWide();
        uint64_t elapsed = now - start;
        if (elapsed >= duration_us) break;

        /* Allow skipping with X */
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);
        if (pad.buttons & SCE_CTRL_CROSS)
        {
            skipped = 1;
            break;
        }

        /* Fade-in: alpha grows over first 500ms */
        float progress = (float)elapsed / (float)duration_us;
        float fade = 1.0f;
        if (elapsed < 500000) fade = (float)elapsed / 500000.0f;

        vita2d_start_drawing();
        vita2d_clear_screen();

        /* Background */
        fill_rect(0, 0, 960, 544, UI_COLOR_BG);

        /* Decorative blue band */
        int band_alpha = (int)(255.0f * fade);
        unsigned int band_color = RGBA8(0, 85, 255, band_alpha);
        fill_rect(0, 220, 960, 110, band_color);

        /* Title text */
        int title_alpha = band_alpha;
        unsigned int title_color = RGBA8(255, 255, 255, title_alpha);
        draw_text(330, 270, title_color, 2.5f, "YABAUSE");
        draw_text(360, 310, title_color, 1.5f, "V I T A");

        /* Subtitle */
        if (progress > 0.3f)
        {
            int sub_alpha = (int)(255.0f * (progress - 0.3f) / 0.3f);
            if (sub_alpha > 255) sub_alpha = 255;
            unsigned int sub_color = RGBA8(180, 200, 220, sub_alpha);
            draw_text(370, 360, sub_color, 0.9f, "Emulador de Sega Saturn");
        }

        /* Loading bar */
        if (progress > 0.5f)
        {
            int bar_w = (int)(400.0f * progress);
            fill_rect(280, 420, 400, 8, UI_COLOR_DARK_BLUE);
            fill_rect(280, 420, bar_w, 8, UI_COLOR_SELECT);
        }

        /* Skip hint */
        if (progress > 0.6f)
        {
            draw_text(400, 470, UI_COLOR_WARN, 0.8f, "Pulsa X para continuar");
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
    }

    return skipped;
}

/* ---- Main UI loop ---- */

int vita_ui_main_loop(vita_config_t *cfg)
{
    SceCtrlData pad, old_pad;
    memset(&pad, 0, sizeof(pad));
    memset(&old_pad, 0, sizeof(old_pad));

    if (!g_ui_initialized)
    {
        if (!init_vita_ui()) return 0;
    }

    /* Rescan in case files were added */
    scan_rom_files();
    scan_bios_files();

    while (1)
    {
        sceCtrlPeekBufferPositive(0, &pad, 1);

        /* Tab switching with L/R */
        if ((pad.buttons & SCE_CTRL_LTRIGGER) && !(old_pad.buttons & SCE_CTRL_LTRIGGER))
        {
            g_current_tab--;
            if (g_current_tab < 0) g_current_tab = TAB_COUNT - 1;
        }
        if ((pad.buttons & SCE_CTRL_RTRIGGER) && !(old_pad.buttons & SCE_CTRL_RTRIGGER))
        {
            g_current_tab++;
            if (g_current_tab >= TAB_COUNT) g_current_tab = 0;
        }

        /* Navigation within current tab */
        if (g_current_tab == TAB_ROMS)
        {
            if ((pad.buttons & SCE_CTRL_UP) && !(old_pad.buttons & SCE_CTRL_UP))
            {
                g_rom_selected--;
                if (g_rom_selected < 0) g_rom_selected = g_rom_count - 1;
            }
            if ((pad.buttons & SCE_CTRL_DOWN) && !(old_pad.buttons & SCE_CTRL_DOWN))
            {
                g_rom_selected++;
                if (g_rom_selected >= g_rom_count) g_rom_selected = 0;
            }
            if ((pad.buttons & SCE_CTRL_CROSS) && !(old_pad.buttons & SCE_CTRL_CROSS))
            {
                if (g_rom_count > 0)
                {
                    /* Select ROM (CHD flagged but not launchable) */
                    if (has_ext(g_rom_files[g_rom_selected], ".chd"))
                    {
                        /* Show a brief message; user must pick non-CHD */
                        /* (handled by display) */
                    }
                }
            }
            if ((pad.buttons & SCE_CTRL_START) && !(old_pad.buttons & SCE_CTRL_START))
            {
                /* Launch emulation */
                if (g_rom_count > 0 && !has_ext(g_rom_files[g_rom_selected], ".chd"))
                {
                    /* Build the ROM path */
                    snprintf(cfg->rom_path, MAX_PATH_LEN, "%s/%s",
                             ROMS_DIR, g_rom_files[g_rom_selected]);
                    cfg->rom_selected = 1;
                    cfg->selected_is_chd = 0;

                    /* Auto-pick BIOS based on region preference */
                    int target_region = SMPC_REGION_AUTODETECT;
                    switch (g_region_index)
                    {
                        case 1: target_region = SMPC_REGION_JAPAN; break;
                        case 2: target_region = SMPC_REGION_NORTHAMERICA; break;
                        case 3: target_region = SMPC_REGION_EUROPE; break;
                        default: target_region = SMPC_REGION_AUTODETECT; break;
                    }
                    cfg->bios_region = target_region;

                    /* Find a matching BIOS */
                    int found_bios = 0;
                    if (g_bios_count > 0)
                    {
                        if (target_region != SMPC_REGION_AUTODETECT)
                        {
                            /* Look for a BIOS matching the chosen region */
                            for (int i = 0; i < g_bios_count; i++)
                            {
                                if (g_bios_entries[i].region == target_region)
                                {
                                    strncpy(cfg->bios_path, g_bios_entries[i].fullpath,
                                            MAX_PATH_LEN);
                                    found_bios = 1;
                                    break;
                                }
                            }
                        }
                        /* Fallback: use the first available BIOS */
                        if (!found_bios)
                        {
                            strncpy(cfg->bios_path, g_bios_entries[0].fullpath,
                                    MAX_PATH_LEN);
                            found_bios = 1;
                        }
                    }
                    if (!found_bios)
                    {
                        cfg->bios_path[0] = '\0'; /* will use HLE */
                    }

                    cfg->region_override = target_region;
                    cfg->frameskip = g_frameskip;
                    cfg->audio_enabled = g_audio_enabled;
                    old_pad = pad;
                    return 1;
                }
            }
        }
        else if (g_current_tab == TAB_BIOS)
        {
            if ((pad.buttons & SCE_CTRL_UP) && !(old_pad.buttons & SCE_CTRL_UP))
            {
                g_bios_selected--;
                if (g_bios_selected < 0) g_bios_selected = g_bios_count - 1;
            }
            if ((pad.buttons & SCE_CTRL_DOWN) && !(old_pad.buttons & SCE_CTRL_DOWN))
            {
                g_bios_selected++;
                if (g_bios_selected >= g_bios_count) g_bios_selected = 0;
            }
            if ((pad.buttons & SCE_CTRL_TRIANGLE) && !(old_pad.buttons & SCE_CTRL_TRIANGLE))
            {
                /* Rescan BIOS folder */
                scan_bios_files();
            }
        }
        else if (g_current_tab == TAB_CONFIG)
        {
            if ((pad.buttons & SCE_CTRL_UP) && !(old_pad.buttons & SCE_CTRL_UP))
            {
                if (g_region_index > 0) g_region_index--;
                else if (g_frameskip > 0) g_frameskip--;
            }
            if ((pad.buttons & SCE_CTRL_DOWN) && !(old_pad.buttons & SCE_CTRL_DOWN))
            {
                if (g_region_index < 3) g_region_index++;
                else if (g_frameskip < 5) g_frameskip++;
            }
            if ((pad.buttons & SCE_CTRL_LEFT) && !(old_pad.buttons & SCE_CTRL_LEFT))
            {
                if (g_frameskip > 0) g_frameskip--;
            }
            if ((pad.buttons & SCE_CTRL_RIGHT) && !(old_pad.buttons & SCE_CTRL_RIGHT))
            {
                if (g_frameskip < 5) g_frameskip++;
            }
            if ((pad.buttons & SCE_CTRL_CROSS) && !(old_pad.buttons & SCE_CTRL_CROSS))
            {
                /* Cycle audio toggle when in the audio row area; simple: toggle */
                g_audio_enabled = !g_audio_enabled;
            }
        }

        /* Global: O = quit */
        if ((pad.buttons & SCE_CTRL_CIRCLE) && !(old_pad.buttons & SCE_CTRL_CIRCLE))
        {
            old_pad = pad;
            return 0;
        }

        old_pad = pad;

        /* ---- Render ---- */
        vita2d_start_drawing();
        vita2d_clear_screen();

        fill_rect(0, 0, 960, 544, UI_COLOR_BG);

        draw_title_bar();
        int content_y = draw_tab_bar();

        switch (g_current_tab)
        {
            case TAB_ROMS:   draw_roms_tab(content_y);   break;
            case TAB_BIOS:   draw_bios_tab(content_y);   break;
            case TAB_CONFIG: draw_config_tab(content_y); break;
        }

        vita2d_end_drawing();
        vita2d_swap_buffers();
        sceDisplayWaitVblankStart();
    }
}
