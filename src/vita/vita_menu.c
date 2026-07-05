/*
 * vita_menu.c — YabauseVita: Implementación completa del menú
 * ─────────────────────────────────────────────────────────────
 * - Tema negro/azul con partículas animadas
 * - 3 pestañas: ROMs, BIOS (autodetectar región), Configuración
 * - Soporte .bin .cue .iso .chd .mds .ccd
 * - Carga asíncrona con timeout (evita "cargando eterno")
 * - Autodetección de BIOS por región del juego
 * - Configuración persistente en archivo
 */
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/dirent.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/power.h>
#include <vita2d.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>

#include "vita_menu.h"

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 1 — COLORES DEL TEMA NEGRO/AZUL
   ══════════════════════════════════════════════════════════════ */
#define COL_BG_DARK       RGBA8(6, 6, 14, 255)
#define COL_BG_MEDIUM     RGBA8(12, 12, 26, 255)
#define COL_BG_LIGHT      RGBA8(20, 22, 44, 255)
#define COL_BG_PANEL      RGBA8(10, 12, 28, 240)

#define COL_BLUE_DARK     RGBA8(8, 40, 110, 255)
#define COL_BLUE_MAIN     RGBA8(25, 90, 210, 255)
#define COL_BLUE_LIGHT    RGBA8(55, 135, 255, 255)
#define COL_BLUE_PALE     RGBA8(100, 170, 255, 255)
#define COL_BLUE_GLOW     RGBA8(40, 120, 255, 60)

#define COL_TEXT_MAIN     RGBA8(210, 218, 235, 255)
#define COL_TEXT_DIM      RGBA8(100, 110, 140, 255)
#define COL_TEXT_BRIGHT   RGBA8(255, 255, 255, 255)
#define COL_TEXT_ACCENT   RGBA8(80, 160, 255, 255)

#define COL_SUCCESS       RGBA8(40, 200, 80, 255)
#define COL_ERROR         RGBA8(230, 55, 55, 255)
#define COL_WARNING       RGBA8(230, 190, 40, 255)

#define COL_TAB_ACTIVE    RGBA8(25, 90, 210, 255)
#define COL_TAB_INACTIVE  RGBA8(22, 22, 48, 255)
#define COL_TAB_BORDER    RGBA8(35, 80, 180, 255)

#define COL_SEL_BG        RGBA8(20, 65, 160, 200)
#define COL_SEL_BORDER    RGBA8(50, 140, 255, 255)

#define COL_SCROLL_TRACK  RGBA8(30, 30, 60, 120)
#define COL_SCROLL_THUMB  RGBA8(50, 110, 220, 200)

#define COL_REGION_JP     RGBA8(220, 50, 50, 255)
#define COL_REGION_US     RGBA8(50, 120, 230, 255)
#define COL_REGION_EU     RGBA8(50, 190, 80, 255)
#define COL_REGION_AUTO   RGBA8(180, 180, 180, 255)
#define COL_REGION_UNK    RGBA8(100, 100, 100, 255)

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 2 — TIPOS INTERNOS
   ══════════════════════════════════════════════════════════════ */
typedef struct {
    char  name[VMENU_MAX_NAME];
    char  path[VMENU_MAX_PATH];
    SceIoStat stat;
    int   is_dir;
    int   format;
} FileEntry;

typedef struct {
    char  name[VMENU_MAX_NAME];
    char  path[VMENU_MAX_PATH];
    int   region;
    int   version_major;
    int   version_minor;
    int   valid;
    SceUInt64 size;
} BiosEntry;

typedef struct {
    float x, y;
    float speed;
    float radius;
    float alpha;
} Particle;

typedef struct {
    const VitaMenuConfig *config;
    VitaMenuLoadCallback  callback;
    char                 error[VMENU_MAX_MSG];
    int                  result;
} LoadThreadData;

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 3 — ESTADO GLOBAL
   ══════════════════════════════════════════════════════════════ */
static vita2d_pgf *g_font = NULL;

/* Partículas de fondo */
#define PARTICLE_COUNT 35
static Particle g_particles[PARTICLE_COUNT];

/* Pestaña activa */
static int g_active_tab = VMENU_TAB_ROMS;

/* ── Estado del navegador de archivos ──────────────────────── */
static FileEntry  g_files[VMENU_MAX_FILES];
static int        g_file_count = 0;
static int        g_file_sel = 0;
static int        g_file_scroll = 0;
static char       g_current_dir[VMENU_MAX_PATH] = "";

/* ── Estado de BIOS ────────────────────────────────────────── */
static BiosEntry  g_bios[VMENU_MAX_BIOS];
static int        g_bios_count = 0;
static int        g_bios_sel = 0;
static int        g_bios_scroll = 0;
static int        g_bios_scanned = 0;
static char       g_bios_status_msg[VMENU_MAX_MSG] = "";

/* ── Estado de configuración ───────────────────────────────── */
static int g_cfg_sel = 0;
static int g_cfg_scroll = 0;
static char g_cfg_msg[VMENU_MAX_MSG] = "";

/* ── Estado de carga ───────────────────────────────────────── */
static LoadThreadData g_load_data;
static SceUID         g_load_tid = -1;
static volatile int   g_load_state = VMENU_LOAD_IDLE;
static SceUInt64      g_load_start = 0;
#define LOAD_TIMEOUT_SEC 45

/* ── Mensaje de estado general ─────────────────────────────── */
static char       g_status_msg[VMENU_MAX_MSG] = "";
static SceUInt64  g_status_time = 0;
#define STATUS_DURATION_MS 3000

/* ── Layout ────────────────────────────────────────────────── */
#define SCREEN_W  960
#define SCREEN_H  544
#define TITLE_H   52
#define TAB_H     42
#define CONTENT_Y (TITLE_H + TAB_H)
#define CONTENT_H (SCREEN_H - CONTENT_Y - 50)
#define STATUS_H  22
#define HINTS_H   28
#define ITEM_H    34
#define MARGIN    16
#define FONT_SCALE 0.85f
#define FONT_SCALE_SM 0.72f
#define FONT_SCALE_LG 1.15f
#define FONT_BASELINE 22.0f

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 4 — UTILIDADES DE CADENAS Y RUTAS
   ══════════════════════════════════════════════════════════════ */
static void safe_strcpy(char *dst, const char *src, int max) {
    if (!dst || !src || max <= 0) return;
    strncpy(dst, src, max - 1);
    dst[max - 1] = '\0';
}

static void safe_strcat(char *dst, const char *src, int max) {
    if (!dst || !src || max <= 0) return;
    int len = (int)strlen(dst);
    if (len < max - 1) {
        strncpy(dst + len, src, max - 1 - len);
        dst[max - 1] = '\0';
    }
}

static void to_lower(char *s) {
    if (!s) return;
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static void get_extension(const char *path, char *ext, int ext_size) {
    if (!path || !ext || ext_size <= 0) { if(ext) ext[0]='\0'; return; }
    const char *dot = strrchr(path, '.');
    if (!dot) { ext[0] = '\0'; return; }
    safe_strcpy(ext, dot + 1, ext_size);
    to_lower(ext);
}

static int detect_format(const char *path) {
    char ext[16];
    get_extension(path, ext, sizeof(ext));
    if (strcmp(ext, "bin") == 0) return VMENU_FMT_BIN;
    if (strcmp(ext, "cue") == 0) return VMENU_FMT_CUE;
    if (strcmp(ext, "iso") == 0) return VMENU_FMT_ISO;
    if (strcmp(ext, "chd") == 0) return VMENU_FMT_CHD;
    if (strcmp(ext, "mds") == 0) return VMENU_FMT_MDS;
    if (strcmp(ext, "ccd") == 0) return VMENU_FMT_CCD;
    return VMENU_FMT_UNKNOWN;
}

static int is_rom_extension(const char *ext) {
    if (!ext) return 0;
    return (strcmp(ext, "bin") == 0 || strcmp(ext, "cue") == 0 ||
            strcmp(ext, "iso") == 0 || strcmp(ext, "chd") == 0 ||
            strcmp(ext, "mds") == 0 || strcmp(ext, "ccd") == 0);
}

static void get_parent_dir(const char *path, char *parent, int max) {
    if (!path || !parent || max <= 0) return;
    safe_strcpy(parent, path, max);
    char *last_slash = strrchr(parent, '/');
    if (last_slash && last_slash != parent) {
        *last_slash = '\0';
    } else {
        safe_strcpy(parent, path, max);
    }
}

static void format_size(SceUInt64 bytes, char *out, int out_size) {
    if (!out || out_size <= 0) return;
    if (bytes < 1024ULL) {
        snprintf(out, out_size, "%llu B", (unsigned long long)bytes);
    } else if (bytes < 1048576ULL) {
        snprintf(out, out_size, "%.1f KB", bytes / 1024.0);
    } else if (bytes < 1073741824ULL) {
        snprintf(out, out_size, "%.1f MB", bytes / 1048576.0);
    } else {
        snprintf(out, out_size, "%.2f GB", bytes / 1073741824.0);
    }
}

/* Helper: leer una línea de texto desde un archivo (como fgets).
   Retorna la longitud de la línea (sin el \\n), 0 en EOF, o negativo en error. */
static int sceIoReadLine(SceUID fd, char *buf, int buf_size) {
    if (!buf || buf_size <= 0) return -1;
    int total = 0;
    char c;
    while (total < buf_size - 1) {
        int ret = sceIoRead(fd, &c, 1);
        if (ret < 0) return -1;
        if (ret == 0) break;
        if (c == '\n') break;
        if (c == '\r') continue;
        buf[total++] = c;
    }
    buf[total] = '\0';
    return total;
}

static void set_status(const char *msg) {
    if (!msg) return;
    safe_strcpy(g_status_msg, msg, sizeof(g_status_msg));
    g_status_time = sceKernelGetProcessTimeWide();
}

static int status_visible(void) {
    if (g_status_msg[0] == '\0') return 0;
    SceUInt64 elapsed = sceKernelGetProcessTimeWide() - g_status_time;
    return (elapsed < STATUS_DURATION_MS * 1000ULL) ? 1 : 0;
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 5 — PARTÍCULAS DE FONDO
   ══════════════════════════════════════════════════════════════ */
static void init_particles(void) {
    srand((unsigned int)time(NULL));
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        g_particles[i].x = (float)(rand() % SCREEN_W);
        g_particles[i].y = (float)(rand() % SCREEN_H);
        g_particles[i].speed = 8.0f + (float)(rand() % 20) / 10.0f;
        g_particles[i].radius = 1.0f + (float)(rand() % 30) / 10.0f;
        g_particles[i].alpha = 20.0f + (float)(rand() % 50);
    }
}

static void update_particles(float dt) {
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        g_particles[i].y -= g_particles[i].speed * dt;
        g_particles[i].x += sinf(g_particles[i].y * 0.01f + (float)i) * 5.0f * dt;
        if (g_particles[i].y < -10.0f) {
            g_particles[i].y = (float)SCREEN_H + 10.0f;
            g_particles[i].x = (float)(rand() % SCREEN_W);
        }
        if (g_particles[i].x < 0.0f) g_particles[i].x = (float)SCREEN_W;
        if (g_particles[i].x > (float)SCREEN_W) g_particles[i].x = 0.0f;
    }
}

static void draw_particles(void) {
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        unsigned int a = (unsigned int)g_particles[i].alpha;
        unsigned int c = RGBA8(40, 120, 255, a);
        vita2d_draw_fill_circle(g_particles[i].x, g_particles[i].y,
                                g_particles[i].radius, c);
    }
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 6 — PRIMITIVAS DE DIBUJO
   ══════════════════════════════════════════════════════════════ */
static void draw_text(float x, float y, const char *text, unsigned int color, float scale) {
    if (!g_font || !text) return;
    vita2d_pgf_draw_text(g_font, x, y + FONT_BASELINE * scale, color, scale, text);
}

static void draw_text_clipped(float x, float y, float max_w, const char *text,
                              unsigned int color, float scale) {
    if (!g_font || !text || max_w <= 0.0f) return;
    /* Medir y truncar si es necesario */
    float w = vita2d_pgf_text_width(g_font, scale, text);
    if (w <= max_w) {
        draw_text(x, y, text, color, scale);
        return;
    }
    /* Truncar carácter por carácter */
    char buf[VMENU_MAX_NAME];
    safe_strcpy(buf, text, sizeof(buf));
    int len = (int)strlen(buf);
    while (len > 0) {
        buf[len] = '\0';
        w = vita2d_pgf_text_width(g_font, scale, buf);
        if (w <= max_w - 20.0f) break;
        len--;
    }
    if (len > 0) {
        buf[len] = '\0';
        safe_strcat(buf, "...", sizeof(buf));
        draw_text(x, y, buf, color, scale);
    }
}

static void draw_rounded_rect(float x, float y, float w, float h,
                               unsigned int color, float r) {
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (r < 0.0f) r = 0.0f;
    vita2d_draw_rectangle(x + r, y, w - 2.0f * r, h, color);
    vita2d_draw_rectangle(x, y + r, w, h - 2.0f * r, color);
    vita2d_draw_fill_circle(x + r, y + r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + r, r, color);
    vita2d_draw_fill_circle(x + r, y + h - r, r, color);
    vita2d_draw_fill_circle(x + w - r, y + h - r, r, color);
}

static void draw_scrollbar(int x, int y, int w, int h,
                           int total, int visible, int offset) {
    if (total <= visible || w <= 0 || h <= 0) return;
    vita2d_draw_rectangle(x, y, w, h, COL_SCROLL_TRACK);
    float ratio = (float)visible / (float)total;
    float thumb_h = h * ratio;
    if (thumb_h < 20.0f) thumb_h = 20.0f;
    int max_off = total - visible;
    if (max_off <= 0) max_off = 1;
    float scroll_f = (float)offset / (float)max_off;
    float thumb_y = y + scroll_f * (h - thumb_h);
    draw_rounded_rect(x + 1, thumb_y, w - 2, thumb_h, COL_SCROLL_THUMB, 3.0f);
}

static void draw_gradient_h(int x, int y, int w, int h,
                            unsigned int c_left, unsigned int c_right) {
    if (w <= 0 || h <= 0) return;
    int steps = w;
    if (steps > 64) steps = 64;
    float sw = (float)w / (float)steps;
    int r1 = (c_left >> 0) & 0xFF, g1 = (c_left >> 8) & 0xFF;
    int b1 = (c_left >> 16) & 0xFF, a1 = (c_left >> 24) & 0xFF;
    int r2 = (c_right >> 0) & 0xFF, g2 = (c_right >> 8) & 0xFF;
    int b2 = (c_right >> 16) & 0xFF, a2 = (c_right >> 24) & 0xFF;
    for (int i = 0; i < steps; i++) {
        float t = (float)i / (float)(steps - 1);
        int r = (int)(r1 + (r2 - r1) * t);
        int g = (int)(g1 + (g2 - g1) * t);
        int b = (int)(b1 + (b2 - b1) * t);
        int a = (int)(a1 + (a2 - a1) * t);
        unsigned int c = RGBA8(r, g, b, a);
        vita2d_draw_rectangle(x + (float)i * sw, y, sw + 1.0f, h, c);
    }
}

static void draw_gradient_v(int x, int y, int w, int h,
                            unsigned int c_top, unsigned int c_bot) {
    if (w <= 0 || h <= 0) return;
    int steps = h;
    if (steps > 64) steps = 64;
    float sh = (float)h / (float)steps;
    int r1 = (c_top >> 0) & 0xFF, g1 = (c_top >> 8) & 0xFF;
    int b1 = (c_top >> 16) & 0xFF, a1 = (c_top >> 24) & 0xFF;
    int r2 = (c_bot >> 0) & 0xFF, g2 = (c_bot >> 8) & 0xFF;
    int b2 = (c_bot >> 16) & 0xFF, a2 = (c_bot >> 24) & 0xFF;
    for (int i = 0; i < steps; i++) {
        float t = (float)i / (float)(steps - 1);
        int r = (int)(r1 + (r2 - r1) * t);
        int g = (int)(g1 + (g2 - g1) * t);
        int b = (int)(b1 + (b2 - b1) * t);
        int a = (int)(a1 + (a2 - a1) * t);
        unsigned int c = RGBA8(r, g, b, a);
        vita2d_draw_rectangle(x, y + (float)i * sh, w, sh + 1.0f, c);
    }
}

static void draw_spinner(float cx, float cy, float radius, float angle) {
    for (int i = 0; i < 12; i++) {
        float a = angle + (float)i * (M_PI * 2.0f / 12.0f);
        int alpha = 255 - i * 21;
        if (alpha < 0) alpha = 0;
        float px = cx + cosf(a) * radius;
        float py = cy + sinf(a) * radius;
        vita2d_draw_fill_circle(px, py, 4.0f, RGBA8(55, 135, 255, (unsigned)alpha));
    }
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 7 — NAVEGADOR DE ARCHIVOS
   ══════════════════════════════════════════════════════════════ */
static int file_cmp(const void *a, const void *b) {
    const FileEntry *fa = (const FileEntry *)a;
    const FileEntry *fb = (const FileEntry *)b;
    if (fa->is_dir && !fb->is_dir) return -1;
    if (!fa->is_dir && fb->is_dir) return 1;
    return strcasecmp(fa->name, fb->name);
}

static void scan_directory(const char *path) {
    g_file_count = 0;
    g_file_sel = 0;
    g_file_scroll = 0;

    SceUID dfd = sceIoDopen(path);
    if (dfd < 0) {
        /* Crear directorio si no existe */
        sceIoMkdir(path, 0777);
        return;
    }

    SceIoDirent dent;
    memset(&dent, 0, sizeof(dent));

    while (g_file_count < VMENU_MAX_FILES) {
        int res = sceIoDread(dfd, &dent);
        if (res <= 0) break;

        /* Ignorar archivos ocultos */
        if (dent.d_name[0] == '.') continue;

        FileEntry *fe = &g_files[g_file_count];
        safe_strcpy(fe->name, dent.d_name, VMENU_MAX_NAME);
        snprintf(fe->path, VMENU_MAX_PATH, "%s/%s", path, dent.d_name);
        fe->stat = dent.d_stat;
        fe->is_dir = (SCE_S_ISDIR(dent.d_stat.st_mode)) ? 1 : 0;
        fe->format = VMENU_FMT_UNKNOWN;

        if (!fe->is_dir) {
            char ext[16];
            get_extension(fe->name, ext, sizeof(ext));
            if (is_rom_extension(ext)) {
                fe->format = detect_format(fe->name);
                g_file_count++;
            }
            /* Ignorar archivos que no sean ROMs */
        } else {
            g_file_count++;
        }
    }
    sceIoDclose(dfd);

    qsort(g_files, (size_t)g_file_count, sizeof(FileEntry), file_cmp);

    /* Agregar entrada ".." al inicio si no estamos en la raíz */
    if (strlen(path) > strlen(VMENU_ROM_DIR)) {
        if (g_file_count < VMENU_MAX_FILES) {
            memmove(&g_files[1], &g_files[0],
                    (size_t)g_file_count * sizeof(FileEntry));
            safe_strcpy(g_files[0].name, "..", VMENU_MAX_NAME);
            get_parent_dir(path, g_files[0].path, VMENU_MAX_PATH);
            g_files[0].is_dir = 1;
            g_files[0].format = VMENU_FMT_UNKNOWN;
            memset(&g_files[0].stat, 0, sizeof(SceIoStat));
            g_file_count++;
        }
    }
}

static const char *format_name(int fmt) {
    switch (fmt) {
        case VMENU_FMT_BIN: return "BIN";
        case VMENU_FMT_CUE: return "CUE";
        case VMENU_FMT_ISO: return "ISO";
        case VMENU_FMT_CHD: return "CHD";
        case VMENU_FMT_MDS: return "MDS";
        case VMENU_FMT_CCD: return "CCD";
        default: return "";
    }
}

static unsigned int format_color(int fmt) {
    switch (fmt) {
        case VMENU_FMT_CHD: return COL_WARNING;
        case VMENU_FMT_CUE: return COL_BLUE_LIGHT;
        case VMENU_FMT_MDS: return COL_SUCCESS;
        default: return COL_TEXT_DIM;
    }
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 8 — DETECCIÓN DE BIOS
   ══════════════════════════════════════════════════════════════ */
static const char *region_name(int region) {
    switch (region) {
        case VMENU_REGION_JP:   return "Japon";
        case VMENU_REGION_US:   return "USA";
        case VMENU_REGION_EU:   return "Europa";
        case VMENU_REGION_AUTO: return "Auto";
        default:                return "Desconocida";
    }
}

static unsigned int region_color(int region) {
    switch (region) {
        case VMENU_REGION_JP:   return COL_REGION_JP;
        case VMENU_REGION_US:   return COL_REGION_US;
        case VMENU_REGION_EU:   return COL_REGION_EU;
        case VMENU_REGION_AUTO: return COL_REGION_AUTO;
        default:                return COL_REGION_UNK;
    }
}

static const char *region_code(int region) {
    switch (region) {
        case VMENU_REGION_JP:   return "J";
        case VMENU_REGION_US:   return "U";
        case VMENU_REGION_EU:   return "E";
        default:                return "?";
    }
}

/*
 * Detectar región de un archivo BIOS de Saturn.
 * Los BIOS de Saturn tienen un patrón identificable en el header.
 * Offsets clave:
 *   0x00-0x0F: Cadena de identificación / copyright SEGA
 *   0x10-0x1F: Más datos de identificación
 *   Área code típicamente en distintas posiciones según versión.
 *
 * Estrategia:
 *   1. Verificar que contiene cadenas SEGA
 *   2. Leer el area code si es posible
 *   3. Si no se puede determinar, intentar por nombre de archivo
 */
static int detect_bios_region_from_data(const char *path) {
    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return VMENU_REGION_UNKNOWN;

    unsigned char buf[512];
    int bytes_read = sceIoRead(fd, buf, 512);
    sceIoClose(fd);

    if (bytes_read < 256) return VMENU_REGION_UNKNOWN;

    /* Verificar que parece un BIOS de Saturn:
       Buscar "SEGA" en los primeros 256 bytes */
    int found_sega = 0;
    for (int i = 0; i < 240; i++) {
        if (buf[i] == 'S' && buf[i+1] == 'E' && buf[i+2] == 'G' &&
            buf[i+3] == 'A') {
            found_sega = 1;
            break;
        }
    }
    if (!found_sega) return VMENU_REGION_UNKNOWN;

    /* Intentar leer el area code.
       En muchos BIOS de Saturn, el area code está alrededor del offset 0x50.
       El byte indica la región:
         0-3: Japón, 4: USA, 5-7: Europa, 8-9: Sudamérica, A-C: Asia
    */
    int area = buf[0x50] & 0x0F;

    if (area <= 3) return VMENU_REGION_JP;
    if (area == 4) return VMENU_REGION_US;
    if (area >= 5 && area <= 7) return VMENU_REGION_EU;

    /* Fallback: intentar por nombre de archivo */
    return VMENU_REGION_UNKNOWN;
}

static int detect_bios_region_from_name(const char *name) {
    char lower[VMENU_MAX_NAME];
    safe_strcpy(lower, name, sizeof(lower));
    to_lower(lower);

    /* Patrones comunes en nombres de BIOS */
    if (strstr(lower, "jp") || strstr(lower, "japan") ||
        strstr(lower, "seg_101") || strstr(lower, "mpr-17933") ||
        strstr(lower, "mpr-17871")) {
        return VMENU_REGION_JP;
    }
    if (strstr(lower, "us") || strstr(lower, "usa") ||
        strstr(lower, "na") || strstr(lower, "america") ||
        strstr(lower, "seg_100") || strstr(lower, "mpr-18638")) {
        return VMENU_REGION_US;
    }
    if (strstr(lower, "eu") || strstr(lower, "europe") ||
        strstr(lower, "pal") || strstr(lower, "mpr-18811")) {
        return VMENU_REGION_EU;
    }
    return VMENU_REGION_UNKNOWN;
}

static void scan_bios_directory(void) {
    g_bios_count = 0;
    g_bios_sel = 0;
    g_bios_scroll = 0;

    /* Crear directorio si no existe */
    sceIoMkdir(VMENU_BIOS_DIR, 0777);

    SceUID dfd = sceIoDopen(VMENU_BIOS_DIR);
    if (dfd < 0) {
        snprintf(g_bios_status_msg, sizeof(g_bios_status_msg),
                 "Directorio BIOS no encontrado. Crea: %s", VMENU_BIOS_DIR);
        g_bios_scanned = 1;
        return;
    }

    SceIoDirent dent;
    memset(&dent, 0, sizeof(dent));

    while (g_bios_count < VMENU_MAX_BIOS) {
        int res = sceIoDread(dfd, &dent);
        if (res <= 0) break;
        if (SCE_S_ISDIR(dent.d_stat.st_mode)) continue;
        if (dent.d_name[0] == '.') continue;

        /* Solo considerar archivos binarios potenciales */
        char ext[16];
        get_extension(dent.d_name, ext, sizeof(ext));
        if (strcmp(ext, "bin") != 0 && strcmp(ext, "bios") != 0 &&
            strcmp(ext, "rom") != 0) {
            /* También aceptar archivos sin extensión si tienen tamaño típico de BIOS */
            if (dent.d_stat.st_size < 256*1024 || dent.d_stat.st_size > 1024*1024)
                continue;
        }

        BiosEntry *be = &g_bios[g_bios_count];
        safe_strcpy(be->name, dent.d_name, VMENU_MAX_NAME);
        snprintf(be->path, VMENU_MAX_PATH, "%s/%s", VMENU_BIOS_DIR, dent.d_name);
        be->size = dent.d_stat.st_size;
        be->version_major = 0;
        be->version_minor = 0;
        be->valid = 0;

        /* Detectar región desde los datos del archivo */
        be->region = detect_bios_region_from_data(be->path);

        /* Si no se pudo determinar por datos, intentar por nombre */
        if (be->region == VMENU_REGION_UNKNOWN) {
            be->region = detect_bios_region_from_name(be->name);
        }

        /* Verificar validez: tamaño típico 512KB */
        if (be->size >= 512*1024 - 16 && be->size <= 1024*1024) {
            be->valid = 1;
        } else if (be->region != VMENU_REGION_UNKNOWN) {
            /* Si detectamos región, probablemente es válido aunque el tamaño sea raro */
            be->valid = 1;
        }

        g_bios_count++;
    }
    sceIoDclose(dfd);

    if (g_bios_count > 0) {
        snprintf(g_bios_status_msg, sizeof(g_bios_status_msg),
                 "%d BIOS encontrados", g_bios_count);
    } else {
        snprintf(g_bios_status_msg, sizeof(g_bios_status_msg),
                 "No se encontraron BIOS en %s", VMENU_BIOS_DIR);
    }
    g_bios_scanned = 1;
}

/*
 * Detectar región de una ROM de Saturn.
 * Lee el header del disco (IP.BIN) que está en los primeros sectores.
 * Para .bin/.iso: leer offset 0x50 directamente.
 * Para .cue: parsear para encontrar el archivo .bin asociado.
 * Para .chd: intentar detectar por nombre o default a AUTO.
 */
static int detect_rom_region(const char *path, int format) {
    char target_path[VMENU_MAX_PATH];
    safe_strcpy(target_path, path, sizeof(target_path));
    int target_fmt = format;

    /* Si es CUE, encontrar el BIN referenciado */
    if (target_fmt == VMENU_FMT_CUE) {
        SceUID fd = sceIoOpen(target_path, SCE_O_RDONLY, 0);
        if (fd >= 0) {
            char line[512];
            while (sceIoReadLine(fd, line, sizeof(line)) > 0) {
                char *file = strstr(line, "FILE");
                if (file) {
                    file += 4;
                    while (*file == ' ' || *file == '"') file++;
                    char *end = strchr(file, '"');
                    if (!end) end = strchr(file, ' ');
                    if (end) {
                        int len = (int)(end - file);
                        if (len > 0 && len < VMENU_MAX_PATH) {
                            /* Construir ruta relativa al directorio del CUE */
                            char dir[VMENU_MAX_PATH];
                            safe_strcpy(dir, target_path, sizeof(dir));
                            char *slash = strrchr(dir, '/');
                            if (slash) {
                                *(slash + 1) = '\0';
                            } else {
                                dir[0] = '\0';
                            }
                            snprintf(target_path, sizeof(target_path), "%s%.*s",
                                     dir, len, file);
                            target_fmt = VMENU_FMT_BIN;
                            break;
                        }
                    }
                }
            }
            sceIoClose(fd);
        }
    }

    /* Para CHD, no podemos leer directamente (datos comprimidos) */
    if (target_fmt == VMENU_FMT_CHD) {
        /* Intentar deducir por nombre de archivo */
        char lower[VMENU_MAX_PATH];
        safe_strcpy(lower, path, sizeof(lower));
        to_lower(lower);
        if (strstr(lower, "(j)") || strstr(lower, "(jp)") ||
            strstr(lower, "japan")) return VMENU_REGION_JP;
        if (strstr(lower, "(u)") || strstr(lower, "(us)") ||
            strstr(lower, "(usa)") || strstr(lower, "ntsc-u") ||
            strstr(lower, "ntsc-u")) return VMENU_REGION_US;
        if (strstr(lower, "(e)") || strstr(lower, "(eu)") ||
            strstr(lower, "(pal)") || strstr(lower, "europe")) return VMENU_REGION_EU;
        return VMENU_REGION_AUTO;
    }

    /* Leer el area code del header para BIN/ISO */
    if (target_fmt == VMENU_FMT_BIN || target_fmt == VMENU_FMT_ISO ||
        target_fmt == VMENU_FMT_MDS || target_fmt == VMENU_FMT_CCD) {
        SceUID fd = sceIoOpen(target_path, SCE_O_RDONLY, 0);
        if (fd < 0) return VMENU_REGION_AUTO;

        unsigned char header[256];
        int bytes = sceIoRead(fd, header, 256);
        sceIoClose(fd);

        if (bytes < 0x51) return VMENU_REGION_AUTO;

        /* Verificar que parece un disco Saturn (buscar "SEGA" en header) */
        int is_saturn = 0;
        for (int i = 0; i < 0x30; i++) {
            if (header[i] == 'S' && i + 16 < bytes &&
                header[i+1] == 'E' && header[i+2] == 'G' &&
                header[i+3] == 'A') {
                is_saturn = 1;
                break;
            }
        }

        if (!is_saturn) return VMENU_REGION_AUTO;

        int area = header[0x50] & 0x0F;
        if (area <= 3) return VMENU_REGION_JP;
        if (area == 4) return VMENU_REGION_US;
        if (area >= 5 && area <= 7) return VMENU_REGION_EU;
    }

    return VMENU_REGION_AUTO;
}

/*
 * Encontrar el BIOS más apropiado para una región dada.
 * Retorna el índice en g_bios[], o -1 si no hay coincidencia.
 */
static int find_bios_for_region(int region) {
    if (region == VMENU_REGION_AUTO || region == VMENU_REGION_UNKNOWN) {
        /* Sin región específica: preferir cualquier BIOS válido */
        for (int i = 0; i < g_bios_count; i++) {
            if (g_bios[i].valid) return i;
        }
        return -1;
    }

    /* Buscar BIOS de la misma región */
    for (int i = 0; i < g_bios_count; i++) {
        if (g_bios[i].valid && g_bios[i].region == region) return i;
    }

    /* Fallback: cualquier BIOS válido */
    for (int i = 0; i < g_bios_count; i++) {
        if (g_bios[i].valid) return i;
    }

    return -1;
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 9 — SOPORTE CHD (información básica)
   ══════════════════════════════════════════════════════════════ */

/*
 * Leer información básica del header CHD.
 * CHD v4/v5: magic "MComprHD" en offset 0.
 * Extrae: versión, tamaño hunk, total hunks, metadatos de pistas.
 */
typedef struct {
    int    valid;
    int    version;
    int    hunk_size;
    int    total_hunks;
    int    track_count;
    SceUInt64 total_size;
} ChdInfo;

static int read_chd_info(const char *path, ChdInfo *info) {
    if (!info) return -1;
    memset(info, 0, sizeof(ChdInfo));

    SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
    if (fd < 0) return -1;

    unsigned char header[128];
    int bytes = sceIoRead(fd, header, 128);
    sceIoClose(fd);

    if (bytes < 48) return -1;

    /* Verificar magic */
    if (memcmp(header, "MComprHD", 8) != 0) return -1;

    info->valid = 1;

    /* Leer versión (little-endian uint32 en offset 12) */
    info->version = header[12] | (header[13] << 8) |
                    (header[14] << 16) | (header[15] << 24);

    /* Hunk size (offset 36) */
    info->hunk_size = header[36] | (header[37] << 8) |
                      (header[38] << 16) | (header[39] << 24);

    /* Total hunks (offset 40) */
    info->total_hunks = header[40] | (header[41] << 8) |
                        (header[42] << 16) | (header[43] << 24);

    info->total_size = (SceUInt64)info->hunk_size * info->total_hunks;

    /* Para tracks, necesitaríamos leer metadatos (complejo sin libchdr).
       Por ahora, estimamos tracks típicos de Saturn (máx 2-5 pistas). */
    info->track_count = -1; /* Desconocido sin leer metadatos completos */

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 10 — GUARDAR/CARGAR CONFIGURACIÓN
   ══════════════════════════════════════════════════════════════ */
static void load_config_file(VitaMenuConfig *cfg) {
    if (!cfg) return;

    /* Valores por defecto */
    cfg->rom_path[0] = '\0';
    cfg->bios_path[0] = '\0';
    cfg->auto_bios = 1;
    cfg->video_filter = VMENU_FILTER_BILINEAR;
    cfg->aspect_ratio = VMENU_ASPECT_4_3;
    cfg->vsync = 1;
    cfg->audio_enabled = 1;
    cfg->audio_volume = 80;
    cfg->cpu_mode = VMENU_CPU_RECOMP;
    cfg->frame_skip = 0;
    cfg->sh2_sync = 1;
    cfg->show_fps = 0;
    cfg->borderless = 0;
    cfg->recent_count = 0;

    SceUID fd = sceIoOpen(VMENU_CONFIG_PATH, SCE_O_RDONLY, 0);
    if (fd < 0) return;

    char line[512];
    while (sceIoReadLine(fd, line, sizeof(line)) > 0) {
        char key[64], val[VMENU_MAX_PATH];
        if (sscanf(line, "%63[^=]=%511[^\n\r]", key, val) != 2) continue;

        if (strcmp(key, "rom_path") == 0)
            safe_strcpy(cfg->rom_path, val, VMENU_MAX_PATH);
        else if (strcmp(key, "bios_path") == 0)
            safe_strcpy(cfg->bios_path, val, VMENU_MAX_PATH);
        else if (strcmp(key, "auto_bios") == 0)
            cfg->auto_bios = atoi(val);
        else if (strcmp(key, "video_filter") == 0)
            cfg->video_filter = atoi(val);
        else if (strcmp(key, "aspect_ratio") == 0)
            cfg->aspect_ratio = atoi(val);
        else if (strcmp(key, "vsync") == 0)
            cfg->vsync = atoi(val);
        else if (strcmp(key, "audio_enabled") == 0)
            cfg->audio_enabled = atoi(val);
        else if (strcmp(key, "audio_volume") == 0)
            cfg->audio_volume = atoi(val);
        else if (strcmp(key, "cpu_mode") == 0)
            cfg->cpu_mode = atoi(val);
        else if (strcmp(key, "frame_skip") == 0)
            cfg->frame_skip = atoi(val);
        else if (strcmp(key, "sh2_sync") == 0)
            cfg->sh2_sync = atoi(val);
        else if (strcmp(key, "show_fps") == 0)
            cfg->show_fps = atoi(val);
        else if (strcmp(key, "borderless") == 0)
            cfg->borderless = atoi(val);
        else if (strncmp(key, "recent_", 7) == 0) {
            int idx = atoi(key + 7);
            if (idx >= 0 && idx < VMENU_MAX_RECENT) {
                safe_strcpy(cfg->recent_games[idx], val, VMENU_MAX_PATH);
                if (idx >= cfg->recent_count) cfg->recent_count = idx + 1;
            }
        }
    }
    sceIoClose(fd);
}

static void save_config_file(const VitaMenuConfig *cfg) {
    if (!cfg) return;

    SceUID fd = sceIoOpen(VMENU_CONFIG_PATH,
                          SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
    if (fd < 0) return;

    char buf[512];
    #define WRITE_KV(k, v) do { \
        snprintf(buf, sizeof(buf), "%s=%s\n", k, v); \
        sceIoWrite(fd, buf, strlen(buf)); \
    } while(0)

    WRITE_KV("rom_path", cfg->rom_path);
    WRITE_KV("bios_path", cfg->bios_path);
    snprintf(buf, sizeof(buf), "auto_bios=%d\n", cfg->auto_bios);
    sceIoWrite(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "video_filter=%d\n", cfg->video_filter);
    sceIoWrite(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "aspect_ratio=%d\n", cfg->aspect_ratio);
    sceIoWrite(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "vsync=%d\n", cfg->vsync);
    sceIoWrite(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "audio_enabled=%d\n", cfg->audio_enabled);
    sceIoWrite(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "audio_volume=%d\n", cfg->audio_volume);
    sceIoWrite(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "cpu_mode=%d\n", cfg->cpu_mode);
    sceIoWrite(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "frame_skip=%d\n", cfg->frame_skip);
    sceIoWrite(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "sh2_sync=%d\n", cfg->sh2_sync);
    sceIoWrite(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "show_fps=%d\n", cfg->show_fps);
    sceIoWrite(fd, buf, strlen(buf));
    snprintf(buf, sizeof(buf), "borderless=%d\n", cfg->borderless);
    sceIoWrite(fd, buf, strlen(buf));

    for (int i = 0; i < cfg->recent_count && i < VMENU_MAX_RECENT; i++) {
        char key[32];
        snprintf(key, sizeof(key), "recent_%d", i);
        WRITE_KV(key, cfg->recent_games[i]);
    }

    #undef WRITE_KV
    sceIoClose(fd);
}

/* Agregar juego a recientes */
static void add_recent(VitaMenuConfig *cfg, const char *path) {
    if (!cfg || !path) return;

    /* Si ya está en la lista, moverlo al inicio */
    for (int i = 0; i < cfg->recent_count && i < VMENU_MAX_RECENT; i++) {
        if (strcmp(cfg->recent_games[i], path) == 0) {
            if (i > 0) {
                char tmp[VMENU_MAX_PATH];
                safe_strcpy(tmp, cfg->recent_games[i], VMENU_MAX_PATH);
                memmove(&cfg->recent_games[1], &cfg->recent_games[0],
                        (size_t)i * VMENU_MAX_PATH);
                safe_strcpy(cfg->recent_games[0], tmp, VMENU_MAX_PATH);
            }
            return;
        }
    }

    /* Mover todos uno hacia abajo e insertar al inicio */
    int max = cfg->recent_count;
    if (max >= VMENU_MAX_RECENT) max = VMENU_MAX_RECENT - 1;
    if (max > 0) {
        memmove(&cfg->recent_games[1], &cfg->recent_games[0],
                (size_t)max * VMENU_MAX_PATH);
    }
    safe_strcpy(cfg->recent_games[0], path, VMENU_MAX_PATH);
    if (cfg->recent_count < VMENU_MAX_RECENT) cfg->recent_count++;
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 11 — PESTAÑA: ROMs
   ══════════════════════════════════════════════════════════════ */
static void draw_roms_tab(VitaMenuConfig *cfg) {
    int cx = MARGIN;
    int cy = CONTENT_Y + 8;
    int cw = SCREEN_W - 2 * MARGIN;
    int ch = CONTENT_H - 16;

    /* Panel de fondo */
    draw_rounded_rect(cx, cy, cw, ch, COL_BG_PANEL, 6.0f);

    /* Ruta actual */
    draw_text(cx + 12, cy + 4, g_current_dir, COL_TEXT_DIM, FONT_SCALE_SM);

    /* Área de lista */
    int list_x = cx + 4;
    int list_y = cy + 28;
    int list_w = cw - 24;
    int list_h = ch - 36;
    int visible_items = list_h / ITEM_H;
    if (visible_items < 1) visible_items = 1;

    /* Ajustar scroll */
    if (g_file_sel < g_file_scroll) g_file_scroll = g_file_sel;
    if (g_file_sel >= g_file_scroll + visible_items)
        g_file_sel = g_file_scroll + visible_items - 1;

    /* Dibujar archivos */
    for (int i = 0; i < visible_items && (g_file_scroll + i) < g_file_count; i++) {
        int idx = g_file_scroll + i;
        FileEntry *fe = &g_files[idx];
        float iy = list_y + (float)i * ITEM_H;

        /* Fondo de selección */
        if (idx == g_file_sel) {
            draw_rounded_rect(list_x, iy + 1, list_w, ITEM_H - 2,
                              COL_SEL_BG, 4.0f);
            /* Borde izquierdo azul */
            vita2d_draw_rectangle(list_x, iy + 3, 3, ITEM_H - 6, COL_BLUE_LIGHT);
        }

        /* Icono de carpeta o formato */
        float ix = list_x + 12;
        if (fe->is_dir) {
            draw_text(ix, iy + 6, "[DIR]", COL_BLUE_PALE, FONT_SCALE_SM);
        } else {
            const char *fmt = format_name(fe->format);
            unsigned int fc = format_color(fe->format);
            draw_text(ix, iy + 6, fmt, fc, FONT_SCALE_SM);
        }

        /* Nombre del archivo */
        float name_x = ix + 48;
        float name_w = list_w - 180;
        if (idx == g_file_sel) {
            draw_text_clipped(name_x, iy + 6, name_w, fe->name,
                              COL_TEXT_BRIGHT, FONT_SCALE);
        } else {
            draw_text_clipped(name_x, iy + 6, name_w, fe->name,
                              COL_TEXT_MAIN, FONT_SCALE);
        }

        /* Tamaño (solo para archivos) */
        if (!fe->is_dir) {
            char size_str[32];
            format_size(fe->stat.st_size, size_str, sizeof(size_str));
            float sw = vita2d_pgf_text_width(g_font, FONT_SCALE_SM, size_str);
            draw_text(list_x + list_w - sw - 12, iy + 7, size_str,
                      COL_TEXT_DIM, FONT_SCALE_SM);
        }
    }

    /* Mensaje si directorio vacío */
    if (g_file_count == 0) {
        draw_text(list_x + list_w * 0.5f - 120, list_y + list_h * 0.3f,
                  "No se encontraron ROMs", COL_TEXT_DIM, FONT_SCALE_LG);
        draw_text(list_x + list_w * 0.5f - 180, list_y + list_h * 0.3f + 40,
                  "Copia tus juegos (.bin/.cue/.iso/.chd) a:", COL_TEXT_DIM, FONT_SCALE);
        draw_text(list_x + list_w * 0.5f - 130, list_y + list_h * 0.3f + 65,
                  VMENU_ROM_DIR, COL_TEXT_ACCENT, FONT_SCALE);
    }

    /* Scrollbar */
    draw_scrollbar(cx + cw - 16, list_y, 8, list_h,
                   g_file_count, visible_items, g_file_scroll);
}

static int handle_roms_input(VitaMenuConfig *cfg, SceCtrlData *pad,
                              SceCtrlData *old_pad) {
    int pressed = pad->buttons & ~old_pad->buttons;
    int visible_items = (CONTENT_H - 44) / ITEM_H;
    if (visible_items < 1) visible_items = 1;

    if (pressed & SCE_CTRL_UP) {
        if (g_file_sel > 0) g_file_sel--;
    }
    if (pressed & SCE_CTRL_DOWN) {
        if (g_file_sel < g_file_count - 1) g_file_sel++;
    }
    if (pressed & SCE_CTRL_LEFT) {
        g_file_sel -= visible_items;
        if (g_file_sel < 0) g_file_sel = 0;
    }
    if (pressed & SCE_CTRL_RIGHT) {
        g_file_sel += visible_items;
        if (g_file_sel >= g_file_count) g_file_sel = g_file_count - 1;
    }
    /* L1/R1: página */
    if (pressed & SCE_CTRL_L1) {
        g_file_sel -= visible_items - 1;
        if (g_file_sel < 0) g_file_sel = 0;
    }
    if (pressed & SCE_CTRL_R1) {
        g_file_sel += visible_items - 1;
        if (g_file_sel >= g_file_count) g_file_sel = g_file_count - 1;
    }

    /* X: Seleccionar */
    if (pressed & SCE_CTRL_CROSS) {
        if (g_file_count <= 0) return 0;
        FileEntry *fe = &g_files[g_file_sel];

        if (fe->is_dir) {
            /* Navegar al directorio */
            safe_strcpy(g_current_dir, fe->path, sizeof(g_current_dir));
            scan_directory(g_current_dir);
        } else {
            /* Validar archivo antes de proceder */
            SceIoStat st;
            if (sceIoGetstat(fe->path, &st) < 0) {
                set_status("Error: archivo no encontrado");
                return 0;
            }
            if (st.st_size == 0) {
                set_status("Error: el archivo esta vacio");
                return 0;
            }

            /* Verificar BIOS */
            if (cfg->auto_bios) {
                int rom_region = detect_rom_region(fe->path, fe->format);
                cfg->rom_region = rom_region;
                int bios_idx = find_bios_for_region(rom_region);
                if (bios_idx >= 0) {
                    safe_strcpy(cfg->bios_path, g_bios[bios_idx].path,
                                VMENU_MAX_PATH);
                    cfg->bios_region = g_bios[bios_idx].region;
                    char msg[VMENU_MAX_MSG];
                    snprintf(msg, sizeof(msg),
                             "BIOS auto: %s [%s]",
                             g_bios[bios_idx].name,
                             region_code(g_bios[bios_idx].region));
                    set_status(msg);
                } else {
                    set_status("Advertencia: no se encontro BIOS compatible");
                }
            } else {
                if (cfg->bios_path[0] == '\0') {
                    set_status("Error: no hay BIOS seleccionado. Ve a la pestana BIOS.");
                    return 0;
                }
                SceIoStat bst;
                if (sceIoGetstat(cfg->bios_path, &bst) < 0) {
                    set_status("Error: BIOS no encontrado. Selecciona uno valido.");
                    return 0;
                }
            }

            safe_strcpy(cfg->rom_path, fe->path, VMENU_MAX_PATH);
            add_recent(cfg, fe->path);
            save_config_file(cfg);
            return 1; /* Señal para iniciar carga */
        }
    }

    /* Triángulo: Directorio padre */
    if (pressed & SCE_CTRL_TRIANGLE) {
        if (strlen(g_current_dir) > strlen(VMENU_ROM_DIR)) {
            char parent[VMENU_MAX_PATH];
            get_parent_dir(g_current_dir, parent, sizeof(parent));
            safe_strcpy(g_current_dir, parent, sizeof(g_current_dir));
            scan_directory(g_current_dir);
        }
    }

    return 0;
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 12 — PESTAÑA: BIOS
   ══════════════════════════════════════════════════════════════ */
static void draw_bios_tab(VitaMenuConfig *cfg) {
    int cx = MARGIN;
    int cy = CONTENT_Y + 8;
    int cw = SCREEN_W - 2 * MARGIN;
    int ch = CONTENT_H - 16;

    draw_rounded_rect(cx, cy, cw, ch, COL_BG_PANEL, 6.0f);

    /* Botón de escanear */
    int scan_x = cx + cw - 140;
    int scan_y = cy + 6;
    int scan_w = 128;
    int scan_h = 28;
    draw_rounded_rect(scan_x, scan_y, scan_w, scan_h, COL_BLUE_DARK, 4.0f);
    vita2d_draw_rectangle(scan_x, scan_y, scan_w, 2, COL_BLUE_MAIN);
    float stw = vita2d_pgf_text_width(g_font, FONT_SCALE_SM, "Escanear BIOS");
    draw_text(scan_x + (scan_w - stw) * 0.5f, scan_y + 4, "Escanear BIOS",
              COL_TEXT_BRIGHT, FONT_SCALE_SM);

    /* Estado del auto-BIOS */
    char auto_label[64];
    if (cfg->auto_bios) {
        snprintf(auto_label, sizeof(auto_label), "Auto-BIOS: ON");
        draw_text(cx + 12, cy + 8, auto_label, COL_SUCCESS, FONT_SCALE);
    } else {
        snprintf(auto_label, sizeof(auto_label), "Auto-BIOS: OFF (manual)");
        draw_text(cx + 12, cy + 8, auto_label, COL_WARNING, FONT_SCALE);
    }

    /* Mensaje de estado */
    if (g_bios_status_msg[0]) {
        draw_text(cx + 12, cy + 30, g_bios_status_msg, COL_TEXT_DIM, FONT_SCALE_SM);
    }

    /* Lista de BIOS */
    int list_x = cx + 4;
    int list_y = cy + 52;
    int list_w = cw - 24;
    int list_h = ch - 60;
    int visible = list_h / ITEM_H;
    if (visible < 1) visible = 1;

    if (g_bios_sel < g_bios_scroll) g_bios_sel = g_bios_scroll;
    if (g_bios_sel >= g_bios_scroll + visible)
        g_bios_sel = g_bios_scroll + visible - 1;

    for (int i = 0; i < visible && (g_bios_scroll + i) < g_bios_count; i++) {
        int idx = g_bios_scroll + i;
        BiosEntry *be = &g_bios[idx];
        float iy = list_y + (float)i * ITEM_H;

        /* Selección */
        int is_selected = (!cfg->auto_bios &&
                           strcmp(be->path, cfg->bios_path) == 0);
        if (idx == g_bios_sel) {
            draw_rounded_rect(list_x, iy + 1, list_w, ITEM_H - 2,
                              COL_SEL_BG, 4.0f);
            vita2d_draw_rectangle(list_x, iy + 3, 3, ITEM_H - 6, COL_BLUE_LIGHT);
        }

        /* Indicador de validez */
        float ix = list_x + 12;
        if (be->valid) {
            draw_text(ix, iy + 6, "OK", COL_SUCCESS, FONT_SCALE_SM);
        } else {
            draw_text(ix, iy + 6, "??", COL_ERROR, FONT_SCALE_SM);
        }

        /* Badge de región */
        float bx = ix + 36;
        unsigned int rc = region_color(be->region);
        const char *rlabel = region_code(be->region);
        float rw = vita2d_pgf_text_width(g_font, FONT_SCALE_SM, rlabel) + 12;
        draw_rounded_rect(bx, iy + 5, rw, 20, rc, 3.0f);
        float rtw = vita2d_pgf_text_width(g_font, FONT_SCALE_SM, rlabel);
        draw_text(bx + (rw - rtw) * 0.5f, iy + 6, rlabel,
                  COL_TEXT_BRIGHT, FONT_SCALE_SM);

        /* Nombre del BIOS */
        float name_x = bx + rw + 10;
        float name_w = list_w - (name_x - list_x) - 100;
        draw_text_clipped(name_x, iy + 6, name_w, be->name,
                          idx == g_bios_sel ? COL_TEXT_BRIGHT : COL_TEXT_MAIN,
                          FONT_SCALE);

        /* Tamaño */
        char size_str[32];
        format_size(be->size, size_str, sizeof(size_str));
        draw_text(list_x + list_w - 80, iy + 7, size_str,
                  COL_TEXT_DIM, FONT_SCALE_SM);

        /* Indicador de selección manual */
        if (is_selected) {
            draw_text(list_x + list_w - 20, iy + 6, "*", COL_BLUE_LIGHT, FONT_SCALE);
        }
    }

    if (g_bios_count == 0 && g_bios_scanned) {
        draw_text(list_x + list_w * 0.5f - 140, list_y + list_h * 0.3f,
                  "No se encontraron archivos de BIOS", COL_TEXT_DIM, FONT_SCALE_LG);
        draw_text(list_x + list_w * 0.5f - 160, list_y + list_h * 0.3f + 40,
                  "Copia tus archivos .bin a:", COL_TEXT_DIM, FONT_SCALE);
        draw_text(list_x + list_w * 0.5f - 110, list_y + list_h * 0.3f + 65,
                  VMENU_BIOS_DIR, COL_TEXT_ACCENT, FONT_SCALE);
    }

    draw_scrollbar(cx + cw - 16, list_y, 8, list_h,
                   g_bios_count, visible, g_bios_scroll);
}

static void handle_bios_input(VitaMenuConfig *cfg, SceCtrlData *pad,
                               SceCtrlData *old_pad) {
    int pressed = pad->buttons & ~old_pad->buttons;
    int visible = (CONTENT_H - 68) / ITEM_H;
    if (visible < 1) visible = 1;

    /* X: Seleccionar BIOS manualmente (si auto_bios está OFF) */
        if (pressed & SCE_CTRL_CROSS) {
            if (!cfg->auto_bios && g_bios_count > 0 && g_bios_sel < g_bios_count) {
                BiosEntry *be = &g_bios[g_bios_sel];
                if (be->valid) {
                    safe_strcpy(cfg->bios_path, be->path, VMENU_MAX_PATH);
                    cfg->bios_region = be->region;
                    char msg[VMENU_MAX_MSG];
                snprintf(msg, sizeof(msg), "BIOS seleccionado: %s [%s]",
                         be->name, region_code(be->region));
                set_status(msg);
                save_config_file(cfg);
            } else {
                set_status("Este archivo no parece un BIOS valido");
            }
        } else if (cfg->auto_bios) {
            set_status("Desactiva Auto-BIOS con O para seleccionar manualmente");
        }
    }

    /* O: Toggle auto-BIOS */
    if (pressed & SCE_CTRL_CIRCLE) {
        cfg->auto_bios = !cfg->auto_bios;
        set_status(cfg->auto_bios ? "Auto-BIOS activado" : "Auto-BIOS desactivado (seleccion manual)");
        save_config_file(cfg);
    }

    /* Triángulo: Escanear */
    if (pressed & SCE_CTRL_TRIANGLE) {
        scan_bios_directory();
    }

    /* Navegación */
    if (pressed & SCE_CTRL_UP) {
        if (g_bios_sel > 0) g_bios_sel--;
    }
    if (pressed & SCE_CTRL_DOWN) {
        if (g_bios_sel < g_bios_count - 1) g_bios_sel++;
    }
    if (pressed & SCE_CTRL_L1) {
        g_bios_sel -= visible - 1;
        if (g_bios_sel < 0) g_bios_sel = 0;
    }
    if (pressed & SCE_CTRL_R1) {
        g_bios_sel += visible - 1;
        if (g_bios_sel >= g_bios_count) g_bios_sel = g_bios_count - 1;
    }
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 13 — PESTAÑA: CONFIGURACIÓN
   ══════════════════════════════════════════════════════════════ */

/* Definición de opciones de configuración */
typedef enum {
    CFG_SECTION_VIDEO,
    CFG_OPT_FILTER,
    CFG_OPT_ASPECT,
    CFG_OPT_VSYNC,
    CFG_SECTION_AUDIO,
    CFG_OPT_AUDIO_EN,
    CFG_OPT_VOLUME,
    CFG_SECTION_EMU,
    CFG_OPT_CPU,
    CFG_OPT_FRAMESKIP,
    CFG_OPT_SH2SYNC,
    CFG_SECTION_DISPLAY,
    CFG_OPT_FPS,
    CFG_OPT_BORDERLESS,
    CFG_SECTION_INFO,
    CFG_OPT_CHD_INFO,
    CFG_OPT_BIOS_PATH,
    CFG_COUNT
} CfgOption;

static const char *cfg_labels[CFG_COUNT] = {
    "-- VIDEO --",          /* SECTION */
    "Filtro de video",      /* OPT */
    "Relacion de aspecto",
    "VSync",
    "-- AUDIO --",          /* SECTION */
    "Audio habilitado",
    "Volumen",
    "-- EMULACION --",      /* SECTION */
    "Modo de CPU",
    "Frame Skip",
    "Sincronia SH2",
    "-- PANTALLA --",       /* SECTION */
    "Mostrar FPS",
    "Sin bordes",
    "-- INFORMACION --",    /* SECTION */
    "Info CHD seleccionado",
    "Ruta BIOS actual"
};

static int cfg_is_section[CFG_COUNT] = {
    1, 0, 0, 0,
    1, 0, 0,
    1, 0, 0, 0,
    1, 0, 0,
    1, 0, 0
};

static void draw_config_tab(VitaMenuConfig *cfg) {
    int cx = MARGIN;
    int cy = CONTENT_Y + 8;
    int cw = SCREEN_W - 2 * MARGIN;
    int ch = CONTENT_H - 16;

    draw_rounded_rect(cx, cy, cw, ch, COL_BG_PANEL, 6.0f);

    int list_y = cy + 8;
    int list_h = ch - 16;
    int visible = list_h / ITEM_H;
    if (visible < 1) visible = 1;

    if (g_cfg_sel < g_cfg_scroll) g_cfg_sel = g_cfg_scroll;
    if (g_cfg_sel >= g_cfg_scroll + visible)
        g_cfg_sel = g_cfg_scroll + visible - 1;

    for (int i = 0; i < visible && (g_cfg_scroll + i) < CFG_COUNT; i++) {
        int idx = g_cfg_scroll + i;
        float iy = list_y + (float)i * ITEM_H;

        if (cfg_is_section[idx]) {
            /* Encabezado de sección */
            draw_gradient_h(cx + 20, iy + 4, cw - 40, ITEM_H - 8,
                            COL_BG_LIGHT, COL_BG_MEDIUM);
            float tw = vita2d_pgf_text_width(g_font, FONT_SCALE, cfg_labels[idx]);
            draw_text(cx + (cw - tw) * 0.5f, iy + 5, cfg_labels[idx],
                      COL_BLUE_PALE, FONT_SCALE);
            continue;
        }

        if (idx == g_cfg_sel) {
            draw_rounded_rect(cx + 4, iy + 1, cw - 24, ITEM_H - 2,
                              COL_SEL_BG, 4.0f);
            vita2d_draw_rectangle(cx + 4, iy + 3, 3, ITEM_H - 6, COL_BLUE_LIGHT);
        }

        /* Etiqueta */
        draw_text(cx + 20, iy + 6, cfg_labels[idx],
                  idx == g_cfg_sel ? COL_TEXT_BRIGHT : COL_TEXT_MAIN, FONT_SCALE);

        /* Valor */
        char val_str[64];
        unsigned int val_color = COL_TEXT_ACCENT;
        float vx = cx + cw - 20;

        switch (idx) {
            case CFG_OPT_FILTER:
                switch (cfg->video_filter) {
                    case VMENU_FILTER_NEAREST:   safe_strcpy(val_str, "Nearest", sizeof(val_str)); break;
                    case VMENU_FILTER_BILINEAR:  safe_strcpy(val_str, "Bilinear", sizeof(val_str)); break;
                    case VMENU_FILTER_SCANLINES: safe_strcpy(val_str, "Scanlines", sizeof(val_str)); break;
                    default: safe_strcpy(val_str, "?", sizeof(val_str)); break;
                }
                break;
            case CFG_OPT_ASPECT:
                switch (cfg->aspect_ratio) {
                    case VMENU_ASPECT_4_3:  safe_strcpy(val_str, "4:3", sizeof(val_str)); break;
                    case VMENU_ASPECT_16_9: safe_strcpy(val_str, "16:9", sizeof(val_str)); break;
                    case VMENU_ASPECT_FILL: safe_strcpy(val_str, "Pantalla completa", sizeof(val_str)); break;
                    default: safe_strcpy(val_str, "?", sizeof(val_str)); break;
                }
                break;
            case CFG_OPT_VSYNC:
                safe_strcpy(val_str, cfg->vsync ? "ON" : "OFF", sizeof(val_str));
                val_color = cfg->vsync ? COL_SUCCESS : COL_ERROR;
                break;
            case CFG_OPT_AUDIO_EN:
                safe_strcpy(val_str, cfg->audio_enabled ? "ON" : "OFF", sizeof(val_str));
                val_color = cfg->audio_enabled ? COL_SUCCESS : COL_ERROR;
                break;
            case CFG_OPT_VOLUME:
                snprintf(val_str, sizeof(val_str), "%d%%", cfg->audio_volume);
                /* Barra visual de volumen */
                {
                    float bar_x = vx - 160;
                    float bar_w = 100;
                    float bar_y = iy + 12;
                    float bar_h = 10;
                    float fill_w = bar_w * (float)cfg->audio_volume / 100.0f;
                    draw_rounded_rect(bar_x, bar_y, bar_w, bar_h,
                                      RGBA8(30, 30, 60, 255), 3.0f);
                    if (fill_w > 0) {
                        unsigned int bar_col = (cfg->audio_volume > 80) ? COL_WARNING :
                                               COL_BLUE_MAIN;
                        draw_rounded_rect(bar_x, bar_y, fill_w, bar_h, bar_col, 3.0f);
                    }
                }
                break;
            case CFG_OPT_CPU:
                switch (cfg->cpu_mode) {
                    case VMENU_CPU_INTERP: safe_strcpy(val_str, "Interprete", sizeof(val_str)); break;
                    case VMENU_CPU_RECOMP: safe_strcpy(val_str, "Recompilador", sizeof(val_str)); break;
                    default: safe_strcpy(val_str, "?", sizeof(val_str)); break;
                }
                break;
            case CFG_OPT_FRAMESKIP:
                if (cfg->frame_skip == 0)
                    safe_strcpy(val_str, "OFF", sizeof(val_str));
                else
                    snprintf(val_str, sizeof(val_str), "%d", cfg->frame_skip);
                break;
            case CFG_OPT_SH2SYNC:
                safe_strcpy(val_str, cfg->sh2_sync ? "ON" : "OFF", sizeof(val_str));
                val_color = cfg->sh2_sync ? COL_SUCCESS : COL_ERROR;
                break;
            case CFG_OPT_FPS:
                safe_strcpy(val_str, cfg->show_fps ? "ON" : "OFF", sizeof(val_str));
                val_color = cfg->show_fps ? COL_SUCCESS : COL_ERROR;
                break;
            case CFG_OPT_BORDERLESS:
                safe_strcpy(val_str, cfg->borderless ? "ON" : "OFF", sizeof(val_str));
                val_color = cfg->borderless ? COL_SUCCESS : COL_ERROR;
                break;
            case CFG_OPT_CHD_INFO:
                if (cfg->rom_path[0] && detect_format(cfg->rom_path) == VMENU_FMT_CHD) {
                    ChdInfo chd;
                    if (read_chd_info(cfg->rom_path, &chd) == 0 && chd.valid) {
                        snprintf(val_str, sizeof(val_str),
                                 "v%d | %d hunks | %.0f MB",
                                 chd.version, chd.total_hunks,
                                 (double)chd.total_size / (1024.0 * 1024.0));
                    } else {
                        safe_strcpy(val_str, "Error al leer CHD", sizeof(val_str));
                        val_color = COL_ERROR;
                    }
                } else {
                    safe_strcpy(val_str, "N/A (selecciona un CHD)", sizeof(val_str));
                    val_color = COL_TEXT_DIM;
                }
                break;
            case CFG_OPT_BIOS_PATH:
                if (cfg->bios_path[0]) {
                    /* Mostrar solo el nombre del archivo */
                    const char *slash = strrchr(cfg->bios_path, '/');
                    const char *bname = slash ? slash + 1 : cfg->bios_path;
                    safe_strcpy(val_str, bname, sizeof(val_str));
                } else {
                    safe_strcpy(val_str, "No configurado", sizeof(val_str));
                    val_color = COL_WARNING;
                }
                break;
            default:
                safe_strcpy(val_str, "", sizeof(val_str));
                break;
        }

        float vw = vita2d_pgf_text_width(g_font, FONT_SCALE, val_str);
        draw_text(vx - vw, iy + 6, val_str, val_color, FONT_SCALE);
    }

    draw_scrollbar(cx + cw - 16, list_y, 8, list_h,
                   CFG_COUNT, visible, g_cfg_scroll);

    /* Mensaje de config */
    if (g_cfg_msg[0]) {
        draw_text(cx + 12, cy + ch - 20, g_cfg_msg, COL_SUCCESS, FONT_SCALE_SM);
    }
}

static void handle_config_input(VitaMenuConfig *cfg, SceCtrlData *pad,
                                 SceCtrlData *old_pad) {
    int pressed = pad->buttons & ~old_pad->buttons;
    int held = pad->buttons;
    int visible = (CONTENT_H - 24) / ITEM_H;
    if (visible < 1) visible = 1;

    /* Navegación: saltar secciones */
    if (pressed & SCE_CTRL_DOWN) {
        do {
            g_cfg_sel++;
            if (g_cfg_sel >= CFG_COUNT) { g_cfg_sel = CFG_COUNT - 1; break; }
        } while (cfg_is_section[g_cfg_sel]);
    }
    if (pressed & SCE_CTRL_UP) {
        do {
            g_cfg_sel--;
            if (g_cfg_sel < 0) { g_cfg_sel = 0; break; }
        } while (cfg_is_section[g_cfg_sel]);
    }
    if (pressed & SCE_CTRL_L1) {
        g_cfg_sel -= visible;
        while (g_cfg_sel >= 0 && cfg_is_section[g_cfg_sel]) g_cfg_sel--;
        if (g_cfg_sel < 0) g_cfg_sel = 0;
        while (cfg_is_section[g_cfg_sel]) g_cfg_sel++;
    }
    if (pressed & SCE_CTRL_R1) {
        g_cfg_sel += visible;
        if (g_cfg_sel >= CFG_COUNT) g_cfg_sel = CFG_COUNT - 1;
        while (cfg_is_section[g_cfg_sel]) g_cfg_sel--;
    }

    /* Izquierda/Derecha para cambiar valores */
    int left = pressed & SCE_CTRL_LEFT;
    int right = pressed & SCE_CTRL_RIGHT;

    /* Mantener presionado para cambios rápidos (volumen) */
    static SceUInt64 hold_timer = 0;
    static int hold_dir = 0;
    if ((held & SCE_CTRL_LEFT) && !(held & SCE_CTRL_RIGHT)) {
        if (hold_dir != -1) { hold_timer = sceKernelGetProcessTimeWide(); hold_dir = -1; }
        else if (sceKernelGetProcessTimeWide() - hold_timer > 400000ULL) {
            left = 1;
            hold_timer = sceKernelGetProcessTimeWide();
        }
    } else if ((held & SCE_CTRL_RIGHT) && !(held & SCE_CTRL_LEFT)) {
        if (hold_dir != 1) { hold_timer = sceKernelGetProcessTimeWide(); hold_dir = 1; }
        else if (sceKernelGetProcessTimeWide() - hold_timer > 400000ULL) {
            right = 1;
            hold_timer = sceKernelGetProcessTimeWide();
        }
    } else {
        hold_dir = 0;
    }

    if (left || right) {
        int dir = right ? 1 : -1;
        switch (g_cfg_sel) {
            case CFG_OPT_FILTER:
                cfg->video_filter += dir;
                if (cfg->video_filter < 0) cfg->video_filter = VMENU_FILTER_COUNT - 1;
                if (cfg->video_filter >= VMENU_FILTER_COUNT) cfg->video_filter = 0;
                break;
            case CFG_OPT_ASPECT:
                cfg->aspect_ratio += dir;
                if (cfg->aspect_ratio < 0) cfg->aspect_ratio = VMENU_ASPECT_COUNT - 1;
                if (cfg->aspect_ratio >= VMENU_ASPECT_COUNT) cfg->aspect_ratio = 0;
                break;
            case CFG_OPT_VSYNC:
                cfg->vsync = !cfg->vsync;
                break;
            case CFG_OPT_AUDIO_EN:
                cfg->audio_enabled = !cfg->audio_enabled;
                break;
            case CFG_OPT_VOLUME:
                cfg->audio_volume += dir * 5;
                if (cfg->audio_volume < 0) cfg->audio_volume = 0;
                if (cfg->audio_volume > 100) cfg->audio_volume = 100;
                break;
            case CFG_OPT_CPU:
                cfg->cpu_mode += dir;
                if (cfg->cpu_mode < 0) cfg->cpu_mode = VMENU_CPU_COUNT - 1;
                if (cfg->cpu_mode >= VMENU_CPU_COUNT) cfg->cpu_mode = 0;
                break;
            case CFG_OPT_FRAMESKIP:
                cfg->frame_skip += dir;
                if (cfg->frame_skip < 0) cfg->frame_skip = 4;
                if (cfg->frame_skip > 4) cfg->frame_skip = 0;
                break;
            case CFG_OPT_SH2SYNC:
                cfg->sh2_sync = !cfg->sh2_sync;
                break;
            case CFG_OPT_FPS:
                cfg->show_fps = !cfg->show_fps;
                break;
            case CFG_OPT_BORDERLESS:
                cfg->borderless = !cfg->borderless;
                break;
            default:
                break;
        }
        save_config_file(cfg);
    }

    /* X: Toggle para opciones booleanas */
    if (pressed & SCE_CTRL_CROSS) {
        switch (g_cfg_sel) {
            case CFG_OPT_VSYNC:      cfg->vsync = !cfg->vsync; break;
            case CFG_OPT_AUDIO_EN:   cfg->audio_enabled = !cfg->audio_enabled; break;
            case CFG_OPT_SH2SYNC:    cfg->sh2_sync = !cfg->sh2_sync; break;
            case CFG_OPT_FPS:        cfg->show_fps = !cfg->show_fps; break;
            case CFG_OPT_BORDERLESS: cfg->borderless = !cfg->borderless; break;
            default: break;
        }
        save_config_file(cfg);
    }
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 14 — PANTALLA DE CARGA
   ══════════════════════════════════════════════════════════════ */
static int load_thread_func(SceSize args, void *argp) {
    LoadThreadData *data = (LoadThreadData *)argp;
    if (!data || !data->callback) {
        if (data) {
            safe_strcpy(data->error, "Error interno: callback nulo",
                        sizeof(data->error));
            data->result = -1;
        }
        return -1;
    }
    data->result = data->callback(data->config, data->error,
                                  (int)sizeof(data->error));
    return data->result;
}

static void start_loading(const VitaMenuConfig *cfg, VitaMenuLoadCallback cb) {
    g_load_data.config = cfg;
    g_load_data.callback = cb;
    g_load_data.error[0] = '\0';
    g_load_data.result = -1;
    g_load_state = VMENU_LOAD_RUNNING;
    g_load_start = sceKernelGetProcessTimeWide();

    g_load_tid = sceKernelCreateThread("vmenu_load", load_thread_func,
                                       0x10000100, 0x400000, 0, 0, NULL);
    if (g_load_tid < 0) {
        g_load_state = VMENU_LOAD_ERROR;
        safe_strcpy(g_load_data.error, "Error al crear hilo de carga",
                    sizeof(g_load_data.error));
        return;
    }

    int res = sceKernelStartThread(g_load_tid, sizeof(g_load_data), &g_load_data);
    if (res < 0) {
        g_load_state = VMENU_LOAD_ERROR;
        safe_strcpy(g_load_data.error, "Error al iniciar hilo de carga",
                    sizeof(g_load_data.error));
        sceKernelDeleteThread(g_load_tid);
        g_load_tid = -1;
    }
}

static void update_loading(void) {
    if (g_load_state != VMENU_LOAD_RUNNING) return;

    SceUInt64 elapsed = sceKernelGetProcessTimeWide() - g_load_start;
    if (elapsed > (SceUInt64)LOAD_TIMEOUT_SEC * 1000000ULL) {
        g_load_state = VMENU_LOAD_ERROR;
        if (g_load_tid >= 0) {
            sceKernelDeleteThread(g_load_tid);
            g_load_tid = -1;
        }
        snprintf(g_load_data.error, sizeof(g_load_data.error),
                 "Timeout: la carga excedio %d segundos", LOAD_TIMEOUT_SEC);
        return;
    }

    if (g_load_tid < 0) {
        g_load_state = VMENU_LOAD_ERROR;
        safe_strcpy(g_load_data.error, "Hilo de carga invalido",
                    sizeof(g_load_data.error));
        return;
    }

    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(SceKernelThreadInfo);
    int res = sceKernelGetThreadInfo(g_load_tid, &info);

    if (res < 0) {
        /* Thread info falló, asumir que terminó */
        g_load_state = (g_load_data.result == 0) ?
                        VMENU_LOAD_DONE : VMENU_LOAD_ERROR;
        if (g_load_data.result != 0 && g_load_data.error[0] == '\0') {
            safe_strcpy(g_load_data.error, "Error desconocido en la carga",
                        sizeof(g_load_data.error));
        }
        sceKernelDeleteThread(g_load_tid);
        g_load_tid = -1;
        return;
    }

    if (info.status == SCE_THREAD_DORMANT) {
        g_load_state = (g_load_data.result == 0) ?
                        VMENU_LOAD_DONE : VMENU_LOAD_ERROR;
        if (g_load_data.result != 0 && g_load_data.error[0] == '\0') {
            snprintf(g_load_data.error, sizeof(g_load_data.error),
                     "Error de carga (codigo: %d)", g_load_data.result);
        }
        sceKernelDeleteThread(g_load_tid);
        g_load_tid = -1;
    }
}

static void draw_loading_screen(void) {
    float cx = SCREEN_W * 0.5f;
    float cy = SCREEN_H * 0.5f;

    /* Panel oscuro */
    draw_rounded_rect(cx - 200, cy - 80, 400, 160, COL_BG_PANEL, 8.0f);
    vita2d_draw_rectangle(cx - 200, cy - 80, 400, 2, COL_BLUE_MAIN);

    /* Spinner */
    SceUInt64 t = sceKernelGetProcessTimeWide();
    float angle = (float)(t % 2000000ULL) / 2000000.0f * M_PI * 2.0f;
    draw_spinner(cx, cy - 25, 24.0f, angle);

    /* Texto de carga */
    float tw = vita2d_pgf_text_width(g_font, FONT_SCALE, "Cargando juego...");
    draw_text(cx - tw * 0.5f, cy + 15, "Cargando juego...",
              COL_TEXT_BRIGHT, FONT_SCALE);

    /* Tiempo transcurrido */
    SceUInt64 elapsed_sec = (sceKernelGetProcessTimeWide() - g_load_start) / 1000000ULL;
    char time_str[32];
    snprintf(time_str, sizeof(time_str), "%llu s", (unsigned long long)elapsed_sec);
    float tsw = vita2d_pgf_text_width(g_font, FONT_SCALE_SM, time_str);
    draw_text(cx - tsw * 0.5f, cy + 40, time_str, COL_TEXT_DIM, FONT_SCALE_SM);

    /* Nombre del archivo */
    if (g_load_data.config) {
        const char *name = strrchr(g_load_data.config->rom_path, '/');
        name = name ? name + 1 : g_load_data.config->rom_path;
        float nw = vita2d_pgf_text_width(g_font, FONT_SCALE_SM, name);
        draw_text(cx - nw * 0.5f, cy + 58, name, COL_TEXT_ACCENT, FONT_SCALE_SM);
    }
}

static void draw_error_screen(void) {
    float cx = SCREEN_W * 0.5f;
    float cy = SCREEN_H * 0.5f;

    /* Panel de error */
    draw_rounded_rect(cx - 260, cy - 90, 520, 180, COL_BG_PANEL, 8.0f);
    vita2d_draw_rectangle(cx - 260, cy - 90, 520, 3, COL_ERROR);

    /* Icono de error */
    draw_text(cx - 20, cy - 65, "!", COL_ERROR, FONT_SCALE_LG * 1.5f);

    /* Título */
    char title[] = "Error al cargar";
    float ttw = vita2d_pgf_text_width(g_font, FONT_SCALE_LG, title);
    draw_text(cx - ttw * 0.5f + 15, cy - 65, title, COL_ERROR, FONT_SCALE_LG);

    /* Mensaje de error */
    if (g_load_data.error[0]) {
        /* Truncar si es muy largo */
        char err_display[256];
        safe_strcpy(err_display, g_load_data.error, sizeof(err_display));
        float ew = vita2d_pgf_text_width(g_font, FONT_SCALE, err_display);
        if (ew > 480) {
            int len = (int)strlen(err_display);
            while (len > 0) {
                err_display[len] = '\0';
                ew = vita2d_pgf_text_width(g_font, FONT_SCALE, err_display);
                if (ew <= 460) { safe_strcat(err_display, "...", sizeof(err_display)); break; }
                len--;
            }
        }
        float ew2 = vita2d_pgf_text_width(g_font, FONT_SCALE, err_display);
        draw_text(cx - ew2 * 0.5f, cy - 20, err_display, COL_TEXT_MAIN, FONT_SCALE);
    }

    /* Instrucción */
    char instr[] = "Presiona X para volver al menu";
    float iw = vita2d_pgf_text_width(g_font, FONT_SCALE_SM, instr);
    draw_text(cx - iw * 0.5f, cy + 30, instr, COL_TEXT_DIM, FONT_SCALE_SM);
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 15 — DIBUJO PRINCIPAL (TÍTULO, PESTAÑAS, ESTADO)
   ══════════════════════════════════════════════════════════════ */
static const char *tab_names[VMENU_TAB_COUNT] = {
    "ROMs", "BIOS", "Config"
};

static void draw_title_bar(void) {
    /* Fondo con gradiente */
    draw_gradient_v(0, 0, SCREEN_W, TITLE_H, COL_BG_LIGHT, COL_BG_DARK);
    /* Línea inferior azul */
    vita2d_draw_rectangle(0, TITLE_H - 2, SCREEN_W, 2, COL_BLUE_MAIN);

    /* Título */
    draw_text(MARGIN, 12, "YabauseVita", COL_TEXT_BRIGHT, FONT_SCALE_LG);

    /* Versión sutil */
    draw_text(SCREEN_W - MARGIN - 80, 18, "v1.0", COL_TEXT_DIM, FONT_SCALE_SM);
}

static void draw_tab_bar(void) {
    /* Fondo */
    vita2d_draw_rectangle(0, TITLE_H, SCREEN_W, TAB_H, COL_BG_MEDIUM);
    /* Línea inferior */
    vita2d_draw_rectangle(0, TITLE_H + TAB_H - 1, SCREEN_W, 1, COL_TAB_BORDER);

    float tab_w = (float)SCREEN_W / VMENU_TAB_COUNT;

    for (int i = 0; i < VMENU_TAB_COUNT; i++) {
        float tx = (float)i * tab_w;
        int active = (i == g_active_tab);

        if (active) {
            /* Fondo activo con gradiente */
            draw_gradient_v(tx + 2, TITLE_H + 2, tab_w - 4, TAB_H - 3,
                            COL_BLUE_DARK, COL_BG_MEDIUM);
            /* Línea inferior brillante */
            vita2d_draw_rectangle(tx + 8, TITLE_H + TAB_H - 3,
                                  tab_w - 16, 3, COL_BLUE_LIGHT);
            /* Sutil glow */
            vita2d_draw_rectangle(tx + 20, TITLE_H + TAB_H - 6,
                                  tab_w - 40, 6, COL_BLUE_GLOW);
        }

        /* Texto de pestaña */
        unsigned int tc = active ? COL_TEXT_BRIGHT : COL_TEXT_DIM;
        float tw = vita2d_pgf_text_width(g_font, FONT_SCALE, tab_names[i]);
        draw_text(tx + (tab_w - tw) * 0.5f, TITLE_H + 8, tab_names[i],
                  tc, FONT_SCALE);
    }
}

static void draw_status_bar(void) {
    float sy = SCREEN_H - STATUS_H - HINTS_H;

    if (status_visible()) {
        draw_gradient_h(0, sy, SCREEN_W, STATUS_H,
                        COL_BG_DARK, COL_BG_MEDIUM);
        vita2d_draw_rectangle(0, sy, SCREEN_W, 1, COL_BLUE_DARK);
        draw_text(MARGIN, sy + 1, g_status_msg, COL_TEXT_ACCENT, FONT_SCALE_SM);
    }
}

static void draw_hints_bar(int tab) {
    float hy = SCREEN_H - HINTS_H;
    draw_gradient_v(0, hy, SCREEN_W, HINTS_H, COL_BG_MEDIUM, COL_BG_DARK);
    vita2d_draw_rectangle(0, hy, SCREEN_W, 1, COL_TAB_BORDER);

    float x = MARGIN;
    float y = hy + 3;

    if (tab == VMENU_TAB_ROMS) {
        draw_text(x, y, "X:Seleccionar", COL_TEXT_DIM, FONT_SCALE_SM);
        x += 130;
        draw_text(x, y, "O:Salir", COL_TEXT_DIM, FONT_SCALE_SM);
        x += 80;
        draw_text(x, y, "/:Padre", COL_TEXT_DIM, FONT_SCALE_SM);
        x += 80;
        draw_text(x, y, "L/R:Pagina", COL_TEXT_DIM, FONT_SCALE_SM);
    } else if (tab == VMENU_TAB_BIOS) {
        draw_text(x, y, "X:Seleccionar", COL_TEXT_DIM, FONT_SCALE_SM);
        x += 130;
        draw_text(x, y, "O:Auto-BIOS", COL_TEXT_DIM, FONT_SCALE_SM);
        x += 110;
        draw_text(x, y, "/:Escanear", COL_TEXT_DIM, FONT_SCALE_SM);
        x += 100;
        draw_text(x, y, "L/R:Pagina", COL_TEXT_DIM, FONT_SCALE_SM);
    } else {
        draw_text(x, y, "<>:Cambiar", COL_TEXT_DIM, FONT_SCALE_SM);
        x += 120;
        draw_text(x, y, "X:Toggle", COL_TEXT_DIM, FONT_SCALE_SM);
        x += 90;
        draw_text(x, y, "L/R:Pagina", COL_TEXT_DIM, FONT_SCALE_SM);
    }

    /* Info de archivos en esquina derecha */
    if (tab == VMENU_TAB_ROMS && g_file_count > 0) {
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "%d archivos", g_file_count);
        float cw = vita2d_pgf_text_width(g_font, FONT_SCALE_SM, count_str);
        draw_text(SCREEN_W - MARGIN - cw, y, count_str, COL_TEXT_DIM, FONT_SCALE_SM);
    } else if (tab == VMENU_TAB_BIOS && g_bios_count > 0) {
        char count_str[32];
        snprintf(count_str, sizeof(count_str), "%d BIOS", g_bios_count);
        float cw = vita2d_pgf_text_width(g_font, FONT_SCALE_SM, count_str);
        draw_text(SCREEN_W - MARGIN - cw, y, count_str, COL_TEXT_DIM, FONT_SCALE_SM);
    }
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 16 — BUCLE PRINCIPAL DEL MENÚ
   ══════════════════════════════════════════════════════════════ */
int vita_menu_run(VitaMenuConfig *config, VitaMenuLoadCallback load_cb) {
    if (!config) return -1;

    SceCtrlData pad, old_pad;
    memset(&pad, 0, sizeof(pad));
    memset(&old_pad, 0, sizeof(old_pad));

    /* Escanear BIOS al iniciar */
    scan_bios_directory();

    /* Escanear directorio de ROMs */
    safe_strcpy(g_current_dir, VMENU_ROM_DIR, sizeof(g_current_dir));
    scan_directory(g_current_dir);

    /* Si hay ROM reciente, navegar a su directorio */
    if (config->recent_count > 0 && config->recent_games[0][0]) {
        char recent_dir[VMENU_MAX_PATH];
        safe_strcpy(recent_dir, config->recent_games[0], sizeof(recent_dir));
        char *slash = strrchr(recent_dir, '/');
        if (slash) {
            *slash = '\0';
            if (sceIoGetstat(recent_dir, NULL) >= 0) {
                safe_strcpy(g_current_dir, recent_dir, sizeof(g_current_dir));
                scan_directory(g_current_dir);
                /* Seleccionar el archivo reciente */
                for (int i = 0; i < g_file_count; i++) {
                    if (strcmp(g_files[i].path, config->recent_games[0]) == 0) {
                        g_file_sel = i;
                        break;
                    }
                }
            }
        }
    }

    int result = -1;
    int running = 1;
    SceUInt64 last_time = sceKernelGetProcessTimeWide();

    while (running) {
        SceUInt64 now = sceKernelGetProcessTimeWide();
        float dt = (float)(now - last_time) / 1000000.0f;
        if (dt > 0.1f) dt = 0.1f; /* Cap delta time */
        last_time = now;

        /* Leer input */
        memset(&pad, 0, sizeof(pad));
        sceCtrlPeekBufferPositive(0, &pad, 1);

        /* ── Estado de carga ─────────────────────────────── */
        if (g_load_state == VMENU_LOAD_RUNNING) {
            update_loading();

            /* Dibujar */
            vita2d_start_drawing();
            vita2d_clear_screen(COL_BG_DARK);
            update_particles(dt);
            draw_particles();
            draw_loading_screen();
            vita2d_end_drawing();
            vita2d_swap_buffers();

            if (g_load_state == VMENU_LOAD_DONE) {
                result = 0;
                running = 0;
            } else if (g_load_state == VMENU_LOAD_ERROR) {
                /* Esperar a que el usuario presione X para volver */
                int error_done = 0;
                while (!error_done) {
                    memset(&pad, 0, sizeof(pad));
                    sceCtrlPeekBufferPositive(0, &pad, 1);
                    int ep = pad.buttons;
                    if (ep & SCE_CTRL_CROSS) error_done = 1;

                    vita2d_start_drawing();
                    vita2d_clear_screen(COL_BG_DARK);
                    update_particles(dt);
                    draw_particles();
                    draw_error_screen();
                    vita2d_end_drawing();
                    vita2d_swap_buffers();
                }
                g_load_state = VMENU_LOAD_IDLE;
                set_status("Vuelve a intentar seleccionar el juego");
            }
            memcpy(&old_pad, &pad, sizeof(pad));
            continue;
        }

        /* ── Cambio de pestañas (L1/R1 en estado normal) ── */
        int pressed = pad.buttons & ~old_pad.buttons;

        if (!(pad.buttons & (SCE_CTRL_L1 | SCE_CTRL_R1))) {
            /* Solo cambiar pestaña si no estamos en la lista */
            /* (L1/R1 se usan para paginación dentro de pestañas) */
        }

        /* Start: cambiar pestaña */
        if (pressed & SCE_CTRL_START) {
            g_active_tab = (g_active_tab + 1) % VMENU_TAB_COUNT;
        }

        /* Select: pestaña anterior */
        if (pressed & SCE_CTRL_SELECT) {
            g_active_tab = (g_active_tab - 1 + VMENU_TAB_COUNT) % VMENU_TAB_COUNT;
        }

        /* ── Manejar input por pestaña ───────────────────── */
        int should_load = 0;

        switch (g_active_tab) {
            case VMENU_TAB_ROMS:
                should_load = handle_roms_input(config, &pad, &old_pad);
                break;
            case VMENU_TAB_BIOS:
                handle_bios_input(config, &pad, &old_pad);
                break;
            case VMENU_TAB_CONFIG:
                handle_config_input(config, &pad, &old_pad);
                break;
        }

        /* O en pestaña ROMs: salir del menú */
        if (g_active_tab == VMENU_TAB_ROMS && (pressed & SCE_CTRL_CIRCLE)) {
            result = -1;
            running = 0;
            break;
        }

        /* Iniciar carga si se solicitó */
        if (should_load && load_cb) {
            start_loading(config, load_cb);
            memcpy(&old_pad, &pad, sizeof(pad));
            continue;
        } else if (should_load && !load_cb) {
            /* Sin callback: retornar directamente */
            result = 0;
            running = 0;
            break;
        }

        /* ── Dibujar ────────────────────────────────────── */
        vita2d_start_drawing();
        vita2d_clear_screen(COL_BG_DARK);

        update_particles(dt);
        draw_particles();

        draw_title_bar();
        draw_tab_bar();

        switch (g_active_tab) {
            case VMENU_TAB_ROMS:   draw_roms_tab(config); break;
            case VMENU_TAB_BIOS:   draw_bios_tab(config); break;
            case VMENU_TAB_CONFIG: draw_config_tab(config); break;
        }

        draw_status_bar();
        draw_hints_bar(g_active_tab);

        vita2d_end_drawing();
        vita2d_swap_buffers();

        memcpy(&old_pad, &pad, sizeof(pad));
    }

    /* Guardar configuración al salir */
    save_config_file(config);

    return result;
}

/* ══════════════════════════════════════════════════════════════
   SECCIÓN 17 — API PÚBLICA
   ══════════════════════════════════════════════════════════════ */
int vita_menu_init(void) {
    /* Iniciar vita2d */
    vita2d_init_advanced(0x800000);
    vita2d_set_clear_color(COL_BG_DARK);

    /* Cargar fuente del sistema */
    g_font = vita2d_load_default_pgf();
    if (!g_font) {
        vita2d_fini();
        return -1;
    }

    /* Iniciar controlador */
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG_WIDE);

    /* Iniciar partículas */
    init_particles();

    /* Crear directorios necesarios */
    sceIoMkdir(VMENU_ROM_DIR, 0777);
    sceIoMkdir(VMENU_BIOS_DIR, 0777);
    sceIoMkdir(VMENU_SAVES_DIR, 0777);

    /* Asegurar que el directorio base existe */
    sceIoMkdir("ux0:data", 0777);
    sceIoMkdir("ux0:data/yabause", 0777);

    return 0;
}

void vita_menu_cleanup(void) {
    if (g_font) {
        vita2d_free_pgf(g_font);
        g_font = NULL;
    }
    vita2d_fini();
}
