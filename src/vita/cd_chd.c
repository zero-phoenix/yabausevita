/*
 * cd_chd.c — YabauseVita: CDInterface para imágenes CHD (MAME v1-v5)
 *
 * Diseño: lectura DIRECTA de sectores desde el CHD usando libchdr
 * (la misma librería que usan los cores de RetroArch en PS Vita).
 * No se extrae nada a disco: se decodifica el hunk que contiene el
 * sector pedido y se mantiene una caché LRU pequeña de hunks.
 *
 * Esto elimina:
 *  - el congelamiento de minutos al seleccionar un juego (antes se
 *    descomprimía TODO el disco a un .bin temporal en ux0:)
 *  - la corrupción por el stub de FLAC (cdfl) y por los hunks
 *    auto-referenciados (SELF) que se leían de un fd de solo escritura
 *  - la pérdida del TOC (pistas de audio) al aplanar a .bin
 *
 * Mapeo lógico (FAD) → físico (frame CHD):
 *  - Los frames de cada pista se almacenan consecutivos, con relleno
 *    a múltiplos de CD_TRACK_PADDING(4) entre pistas.
 *  - Metadatos CHT2: "TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d
 *    PREGAP:%d PGTYPE:%s PGSUB:%s POSTGAP:%d"
 *  - Si PGTYPE empieza con 'V', los frames del pregap SÍ están en el
 *    archivo (y FRAMES los incluye). Si no, el pregap es silencio
 *    virtual: ocupa FADs pero no frames.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libchdr/chd.h>
#include <libchdr/cdrom.h>

#include "cd_chd.h"

#ifdef CHD_HOST_TEST
#include <stdarg.h>
static int vita_log(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    int r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
#else
extern int vita_log(const char *fmt, ...);
#endif

#define CHDLOG(...) vita_log(__VA_ARGS__)

/* ── Estado ────────────────────────────────────────────────── */

#define CHD_MAX_TRACKS   99
#define HUNK_CACHE_SIZE  8

typedef struct
{
    u32 track_start_fad;  /* FAD de INDEX 01 (lo que va al TOC) */
    u32 fad_start;        /* primer FAD con datos en el archivo */
    u32 fad_end;          /* fad_start + frames almacenados */
    u32 chd_frame;        /* frame físico CHD correspondiente a fad_start */
    u32 datasize;         /* bytes de datos por frame: 2048/2324/2336/2352 */
    int mode;             /* 0=audio, 1=mode1, 2=mode2 */
    int ctrladr;          /* 0x41 datos / 0x01 audio */
} CHDTrackInfo;

static chd_file *s_chd = NULL;
static const chd_header *s_hdr = NULL;
static CHDTrackInfo s_trk[CHD_MAX_TRACKS];
static int s_numtracks = 0;
static u32 s_leadout_fad = 0;
static u32 s_toc[102];
static u32 s_frames_per_hunk = 0;

static u8  *s_cache_buf[HUNK_CACHE_SIZE];
static u32  s_cache_hunk[HUNK_CACHE_SIZE];
static u32  s_cache_age[HUNK_CACHE_SIZE];
static u32  s_age_counter = 0;

static const u8 s_sync_header[12] =
    { 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };

/* ── Tipos de pista de MAME ────────────────────────────────── */

static int parse_track_type(const char *type, u32 *datasize, int *mode)
{
    if (!strcmp(type, "MODE1_RAW"))      { *datasize = 2352; *mode = 1; }
    else if (!strcmp(type, "MODE2_RAW")) { *datasize = 2352; *mode = 2; }
    else if (!strcmp(type, "AUDIO"))     { *datasize = 2352; *mode = 0; }
    else if (!strcmp(type, "MODE1"))     { *datasize = 2048; *mode = 1; }
    else if (!strcmp(type, "MODE1/2048")){ *datasize = 2048; *mode = 1; }
    else if (!strcmp(type, "MODE2_FORM1")) { *datasize = 2048; *mode = 2; }
    else if (!strcmp(type, "MODE2_FORM2")) { *datasize = 2324; *mode = 2; }
    else if (!strcmp(type, "MODE2_FORM_MIX")) { *datasize = 2336; *mode = 2; }
    else if (!strcmp(type, "MODE2"))     { *datasize = 2336; *mode = 2; }
    else return -1;
    return 0;
}

/* ── Caché de hunks ────────────────────────────────────────── */

static void cache_reset(void)
{
    int i;
    for (i = 0; i < HUNK_CACHE_SIZE; i++)
    {
        if (s_cache_buf[i]) { free(s_cache_buf[i]); s_cache_buf[i] = NULL; }
        s_cache_hunk[i] = 0xFFFFFFFF;
        s_cache_age[i] = 0;
    }
    s_age_counter = 0;
}

static const u8 *cache_get_hunk(u32 hunknum)
{
    int i, victim = 0;
    u32 oldest;
    chd_error err;

    for (i = 0; i < HUNK_CACHE_SIZE; i++)
    {
        if (s_cache_hunk[i] == hunknum && s_cache_buf[i])
        {
            s_cache_age[i] = ++s_age_counter;
            return s_cache_buf[i];
        }
    }

    /* miss: elegir la entrada más vieja */
    oldest = s_cache_age[0];
    for (i = 1; i < HUNK_CACHE_SIZE; i++)
    {
        if (s_cache_age[i] < oldest) { oldest = s_cache_age[i]; victim = i; }
    }

    if (!s_cache_buf[victim])
    {
        s_cache_buf[victim] = (u8 *)malloc(s_hdr->hunkbytes);
        if (!s_cache_buf[victim]) return NULL;
    }

    err = chd_read(s_chd, hunknum, s_cache_buf[victim]);
    if (err != CHDERR_NONE)
    {
        CHDLOG("CHD: chd_read hunk %u fallo: %s\n", hunknum, chd_error_string(err));
        s_cache_hunk[victim] = 0xFFFFFFFF;
        return NULL;
    }

    s_cache_hunk[victim] = hunknum;
    s_cache_age[victim] = ++s_age_counter;
    return s_cache_buf[victim];
}

/* ── Metadatos → tabla de pistas + TOC ─────────────────────── */

static int build_track_table(void)
{
    int i;
    u32 fad = 150;       /* el área de programa empieza en FAD 150 */
    u32 chd_frame = 0;

    s_numtracks = 0;

    for (i = 0; i < CHD_MAX_TRACKS; i++)
    {
        char meta[256];
        uint32_t metalen = 0;
        int tknum = 0, frames = 0, pregap = 0, postgap = 0;
        char type[64], subtype[32], pgtype[32], pgsub[32];
        CHDTrackInfo *t;
        int pgdata_in_file;

        type[0] = subtype[0] = pgtype[0] = pgsub[0] = '\0';

        if (chd_get_metadata(s_chd, CDROM_TRACK_METADATA2_TAG, i, meta,
                             sizeof(meta), &metalen, NULL, NULL) == CHDERR_NONE)
        {
            if (sscanf(meta, CDROM_TRACK_METADATA2_FORMAT, &tknum, type,
                       subtype, &frames, &pregap, pgtype, pgsub, &postgap) != 8)
            {
                CHDLOG("CHD: metadato CHT2 ilegible: %s\n", meta);
                return -1;
            }
        }
        else if (chd_get_metadata(s_chd, CDROM_TRACK_METADATA_TAG, i, meta,
                                  sizeof(meta), &metalen, NULL, NULL) == CHDERR_NONE)
        {
            if (sscanf(meta, CDROM_TRACK_METADATA_FORMAT, &tknum, type,
                       subtype, &frames) != 4)
            {
                CHDLOG("CHD: metadato CHTR ilegible: %s\n", meta);
                return -1;
            }
            pregap = postgap = 0;
        }
        else
        {
            break;  /* no hay más pistas */
        }

        if (tknum < 1 || tknum > CHD_MAX_TRACKS || frames <= 0)
        {
            CHDLOG("CHD: pista invalida %d (frames=%d)\n", tknum, frames);
            return -1;
        }

        t = &s_trk[s_numtracks];

        if (parse_track_type(type, &t->datasize, &t->mode) != 0)
        {
            CHDLOG("CHD: tipo de pista no soportado: %s\n", type);
            return -1;
        }
        t->ctrladr = (t->mode == 0) ? 0x01 : 0x41;

        pgdata_in_file = (pgtype[0] == 'V');

        if (pgdata_in_file)
        {
            /* FRAMES incluye el pregap; sus datos están en el archivo */
            t->fad_start        = fad;
            t->chd_frame        = chd_frame;
            t->track_start_fad  = fad + pregap;
            t->fad_end          = fad + frames;
            fad += frames;
        }
        else
        {
            /* pregap virtual: ocupa FADs pero no está en el archivo */
            t->track_start_fad  = fad + pregap;
            t->fad_start        = fad + pregap;
            t->chd_frame        = chd_frame;
            t->fad_end          = t->fad_start + frames;
            fad += pregap + frames;
        }

        fad += postgap;

        /* frames físicos rellenados a múltiplo de CD_TRACK_PADDING */
        chd_frame += ((u32)frames + CD_TRACK_PADDING - 1) & ~(CD_TRACK_PADDING - 1);

        CHDLOG("CHD: pista %d %s frames=%d pregap=%d(%s) start_fad=%u file_fad=[%u,%u) chd_frame=%u\n",
               tknum, type, frames, pregap, pgtype[0] ? pgtype : "-",
               t->track_start_fad, t->fad_start, t->fad_end, t->chd_frame);

        s_numtracks++;
    }

    if (s_numtracks == 0)
    {
        CHDLOG("CHD: sin metadatos de pistas (no es un CHD de CD)\n");
        return -1;
    }

    s_leadout_fad = fad;

    /* TOC formato Saturn (ver cdbase.c) */
    memset(s_toc, 0xFF, sizeof(s_toc));
    for (i = 0; i < s_numtracks; i++)
        s_toc[i] = ((u32)s_trk[i].ctrladr << 24) | s_trk[i].track_start_fad;
    s_toc[99]  = ((u32)s_trk[0].ctrladr << 24) | 0x010000;
    s_toc[100] = ((u32)s_trk[s_numtracks-1].ctrladr << 24) | ((u32)s_numtracks << 16);
    s_toc[101] = ((u32)s_trk[s_numtracks-1].ctrladr << 24) | s_leadout_fad;

    CHDLOG("CHD: %d pistas, leadout FAD=%u\n", s_numtracks, s_leadout_fad);
    return 0;
}

/* ── CDInterface ───────────────────────────────────────────── */

static int CHDCDInit(const char *path)
{
    chd_error err;

    memset(s_trk, 0, sizeof(s_trk));
    cache_reset();

    err = chd_open(path, CHD_OPEN_READ, NULL, &s_chd);
    if (err != CHDERR_NONE)
    {
        CHDLOG("CHD: chd_open fallo: %s\n", chd_error_string(err));
        if (err == CHDERR_REQUIRES_PARENT)
            CHDLOG("CHD: este CHD es un delta y requiere su CHD padre\n");
        s_chd = NULL;
        return -1;
    }

    s_hdr = chd_get_header(s_chd);
    if (!s_hdr || s_hdr->unitbytes != CD_FRAME_SIZE ||
        (s_hdr->hunkbytes % CD_FRAME_SIZE) != 0)
    {
        CHDLOG("CHD: no es un CHD de CD-ROM (unitbytes=%u hunkbytes=%u)\n",
               s_hdr ? s_hdr->unitbytes : 0, s_hdr ? s_hdr->hunkbytes : 0);
        chd_close(s_chd);
        s_chd = NULL;
        return -1;
    }

    s_frames_per_hunk = s_hdr->hunkbytes / CD_FRAME_SIZE;

    if (build_track_table() != 0)
    {
        chd_close(s_chd);
        s_chd = NULL;
        return -1;
    }

    CHDLOG("CHD: abierto OK: v%u hunkbytes=%u frames/hunk=%u\n",
           s_hdr->version, s_hdr->hunkbytes, s_frames_per_hunk);
    return 0;
}

static int CHDCDDeInit(void)
{
    if (s_chd)
    {
        chd_close(s_chd);
        s_chd = NULL;
    }
    s_hdr = NULL;
    cache_reset();
    s_numtracks = 0;
    return 0;
}

static int CHDCDGetStatus(void)
{
    /* 0 = disco presente girando, 2 = sin disco */
    return s_chd != NULL ? 0 : 2;
}

static s32 CHDCDReadTOC(u32 *TOC)
{
    memcpy(TOC, s_toc, 0xCC * 2);
    return (0xCC * 2);
}

/* BCD para la cabecera MSF de sectores sintetizados */
static u8 to_bcd(u32 v) { return (u8)(((v / 10) << 4) | (v % 10)); }

static int CHDCDReadSectorFAD(u32 FAD, void *buffer)
{
    u8 *out = (u8 *)buffer;
    const CHDTrackInfo *t = NULL;
    const u8 *hunk;
    u32 chd_frame, hunknum, offs;
    int i;

    memset(out, 0, 2352);

    if (!s_chd)
        return 0;

    for (i = 0; i < s_numtracks; i++)
    {
        if (FAD >= s_trk[i].fad_start && FAD < s_trk[i].fad_end)
        {
            t = &s_trk[i];
            break;
        }
    }

    if (!t)
    {
        /* pregap virtual, gap entre pistas o leadout: silencio.
           Para FADs claramente fuera de disco devolvemos error. */
        if (FAD >= s_leadout_fad + 150)
        {
            CHDLOG("CHD: lectura fuera de disco FAD=%u (leadout=%u)\n",
                   FAD, s_leadout_fad);
            return 0;
        }
        return 1;
    }

    chd_frame = t->chd_frame + (FAD - t->fad_start);
    hunknum = chd_frame / s_frames_per_hunk;
    offs = (chd_frame % s_frames_per_hunk) * CD_FRAME_SIZE;

    hunk = cache_get_hunk(hunknum);
    if (!hunk)
        return 0;

    if (t->datasize == 2352)
    {
        memcpy(out, hunk + offs, 2352);
        if (t->mode == 0)
        {
            /* CHD guarda el audio big-endian; el Saturn (y los .bin)
               lo esperan little-endian: intercambiar cada muestra */
            u32 j;
            for (j = 0; j < 2352; j += 2)
            {
                u8 tmp = out[j];
                out[j] = out[j + 1];
                out[j + 1] = tmp;
            }
        }
    }
    else
    {
        /* pista "cocida" (2048/2324/2336): sintetizar sector RAW */
        u32 lba_m = FAD / 4500, lba_s = (FAD / 75) % 60, lba_f = FAD % 75;
        memcpy(out, s_sync_header, 12);
        out[12] = to_bcd(lba_m);
        out[13] = to_bcd(lba_s);
        out[14] = to_bcd(lba_f);
        out[15] = (t->mode == 2) ? 0x02 : 0x01;
        if (t->datasize == 2048)
        {
            memcpy(out + 16, hunk + offs, 2048);
            if (t->mode == 1)
                ecc_generate(out);   /* EDC+ECC válidos para lectores exigentes */
        }
        else
        {
            memcpy(out + 16, hunk + offs, t->datasize);
        }
    }

    return 1;
}

CDInterface CHDCD = {
    CDCORE_CHD,
    "CHD Virtual Drive (libchdr)",
    CHDCDInit,
    CHDCDDeInit,
    CHDCDGetStatus,
    CHDCDReadTOC,
    CHDCDReadSectorFAD
};
