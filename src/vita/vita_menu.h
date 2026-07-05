/*
 * vita_menu.h — YabauseVita: Interfaz de menú sofisticada
 * Tema negro/azul con pestañas: ROMs, BIOS, Configuración
 * Soporte CHD y autodetección de BIOS por región
 */
#ifndef VITA_MENU_H
#define VITA_MENU_H

#include <psp2/types.h>
#include <psp2/io/stat.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Rutas por defecto ─────────────────────────────────────── */
#define VMENU_ROM_DIR      "ux0:data/yabause/roms"
#define VMENU_BIOS_DIR     "ux0:data/yabause/bios"
#define VMENU_CONFIG_PATH  "ux0:data/yabause/config.cfg"
#define VMENU_SAVES_DIR    "ux0:data/yabause/saves"

/* ── Límites ───────────────────────────────────────────────── */
#define VMENU_MAX_PATH     260
#define VMENU_MAX_FILES    512
#define VMENU_MAX_BIOS     32
#define VMENU_MAX_MSG      512
#define VMENU_MAX_RECENT   10
#define VMENU_MAX_NAME     128

/* ── Regiones Saturn ───────────────────────────────────────── */
#define VMENU_REGION_JP      0
#define VMENU_REGION_US      1
#define VMENU_REGION_EU      2
#define VMENU_REGION_AUTO    3
#define VMENU_REGION_UNKNOWN 4

/* ── Estado de carga ───────────────────────────────────────── */
#define VMENU_LOAD_IDLE     0
#define VMENU_LOAD_RUNNING  1
#define VMENU_LOAD_DONE     2
#define VMENU_LOAD_ERROR    3

/* ── Pestañas ──────────────────────────────────────────────── */
#define VMENU_TAB_ROMS      0
#define VMENU_TAB_BIOS      1
#define VMENU_TAB_CONFIG    2
#define VMENU_TAB_COUNT     3

/* ── Filtros de vídeo ──────────────────────────────────────── */
#define VMENU_FILTER_NEAREST    0
#define VMENU_FILTER_BILINEAR   1
#define VMENU_FILTER_SCANLINES  2
#define VMENU_FILTER_COUNT      3

/* ── Modos de CPU ──────────────────────────────────────────── */
#define VMENU_CPU_INTERP     0
#define VMENU_CPU_RECOMP     1
#define VMENU_CPU_COUNT      2

/* ── Relación de aspecto ───────────────────────────────────── */
#define VMENU_ASPECT_4_3     0
#define VMENU_ASPECT_16_9    1
#define VMENU_ASPECT_FILL    2
#define VMENU_ASPECT_COUNT   3

/* ── Formatos de ROM ───────────────────────────────────────── */
#define VMENU_FMT_BIN        0
#define VMENU_FMT_CUE        1
#define VMENU_FMT_ISO        2
#define VMENU_FMT_CHD        3
#define VMENU_FMT_MDS        4
#define VMENU_FMT_CCD        5
#define VMENU_FMT_UNKNOWN    6

/* ── Estructura de configuración ───────────────────────────── */
typedef struct {
    char rom_path[VMENU_MAX_PATH];
    char bios_path[VMENU_MAX_PATH];
    int  auto_bios;

    int  video_filter;
    int  aspect_ratio;
    int  vsync;

    int  audio_enabled;
    int  audio_volume;

    int  cpu_mode;
    int  frame_skip;
    int  sh2_sync;

    int  show_fps;
    int  borderless;

    int  rom_region;        /* region auto-detectada de la ROM seleccionada */
    int  bios_region;       /* región del BIOS emparejado */

    char recent_games[VMENU_MAX_RECENT][VMENU_MAX_PATH];
    int  recent_count;
} VitaMenuConfig;

/* ── Callback de carga (se ejecuta en hilo separado) ───────── */
typedef int (*VitaMenuLoadCallback)(const VitaMenuConfig *config,
                                    char *error, int error_size);

/* ── API pública ───────────────────────────────────────────── */
int  vita_menu_init(void);
int  vita_menu_run(VitaMenuConfig *config, VitaMenuLoadCallback load_cb);
void vita_menu_cleanup(void);

#ifdef __cplusplus
}
#endif

#endif /* VITA_MENU_H */
