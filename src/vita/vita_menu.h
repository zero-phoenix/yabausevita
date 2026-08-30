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
#define VMENU_CPU_DYNARM     2
#define VMENU_CPU_COUNT      3

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

/* ── Mapeo de controles ────────────────────────────────────── */
#define MAP_UP       0
#define MAP_DOWN     1
#define MAP_LEFT     2
#define MAP_RIGHT    3
#define MAP_CROSS    4
#define MAP_CIRCLE   5
#define MAP_SQUARE   6
#define MAP_TRIANGLE 7
#define MAP_L        8
#define MAP_R        9
#define MAP_START   10
#define MAP_SELECT  11
#define MAP_COUNT   12

#define SAT_UP      0
#define SAT_DOWN    1
#define SAT_LEFT    2
#define SAT_RIGHT   3
#define SAT_A       4
#define SAT_B       5
#define SAT_C       6
#define SAT_X       7
#define SAT_Y       8
#define SAT_Z       9
#define SAT_L       10
#define SAT_R       11
#define SAT_START   12
#define SAT_COUNT   13

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
    int  auto_frameskip;

    /* autostart=1 en config.cfg: si rom_path existe, saltar el menú y
       cargar el juego directamente. Infraestructura del ciclo de medición:
       sin él, cada ronda exige una pulsación humana de Circle. */
    int  autostart;

    int  rom_region;        /* region auto-detectada de la ROM seleccionada */
    int  bios_region;       /* región del BIOS emparejado */

    unsigned char mapping[MAP_COUNT]; /* MAP_* → SAT_* */

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
void vita_menu_show_error(const char *title, const char *msg);

/* ── Utilidades exportadas ─────────────────────────────────── */
void load_config_file(VitaMenuConfig *cfg);
void set_default_mapping(VitaMenuConfig *cfg);
void safe_strcpy(char *dst, const char *src, int max);
void safe_strcat(char *dst, const char *src, int max);
void to_lower(char *s);

#ifdef __cplusplus
}
#endif

#endif /* VITA_MENU_H */
