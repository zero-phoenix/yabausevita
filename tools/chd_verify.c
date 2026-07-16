/*
 * chd_verify.c — Harness de verificación del lector CHD (host PC)
 *
 * Uso:
 *   chd_verify gen <dir>              genera test.bin + test.cue sintéticos
 *   chd_verify check <test.chd> <test.bin>   verifica TOC y TODOS los sectores
 *
 * Disco sintético:
 *   Track 1  MODE1/2352  2000 sectores  (datos pseudoaleatorios + ECC válido)
 *   Track 2  AUDIO       1500 sectores  PREGAP 00:02:00 (silencio virtual)
 *                                       (onda senoidal → codec FLAC)
 *   Track 3  AUDIO       1200 sectores  INDEX 00 de 150 frames EN el bin
 *                                       (ruido → codecs LZMA/zlib en audio)
 *
 * La verificación compara la salida de CHDCDReadSectorFAD contra el BIN
 * original byte a byte en todo el disco, incluyendo zonas de pregap.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>

#include <libchdr/cdrom.h>   /* ecc_generate */
#include "../src/vita/cd_chd.h"

#define T1_SECTORS 2000
#define T2_SECTORS 1500
#define T3_PREGAP  150
#define T3_SECTORS 1200

#define T1_FAD 150
#define T2_START_FAD (T1_FAD + T1_SECTORS + 150)          /* 2300 */
#define T3_FILE_FAD  (T2_START_FAD + T2_SECTORS)           /* 3800: inicio INDEX00 */
#define T3_START_FAD (T3_FILE_FAD + T3_PREGAP)             /* 3950 */
#define LEADOUT_FAD  (T3_START_FAD + T3_SECTORS)           /* 5150 */

static uint32_t xs_state;
static uint32_t xs_next(void)
{
    xs_state ^= xs_state << 13;
    xs_state ^= xs_state >> 17;
    xs_state ^= xs_state << 5;
    return xs_state;
}

static uint8_t to_bcd(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void make_mode1_sector(uint8_t *sec, uint32_t fad, uint32_t seed)
{
    static const uint8_t sync[12] =
        { 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };
    uint32_t i;
    memset(sec, 0, 2352);
    memcpy(sec, sync, 12);
    sec[12] = to_bcd(fad / 4500);
    sec[13] = to_bcd((fad / 75) % 60);
    sec[14] = to_bcd(fad % 75);
    sec[15] = 0x01;
    xs_state = seed * 2654435761u + 1;
    for (i = 16; i < 2064; i++)
        sec[i] = (uint8_t)xs_next();
    /* EDC en 2064..2067 lo dejamos en 0 (Yabause no lo verifica);
       ECC P/Q válido para que chdman lo pueda "strip" y regenerar */
    ecc_generate(sec);
}

static void make_audio_sector_sine(uint8_t *sec, uint32_t idx)
{
    uint32_t i;
    for (i = 0; i < 2352 / 4; i++)
    {
        double t = (double)(idx * 588 + i);
        int16_t l = (int16_t)(9000.0 * sin(t * 0.013));
        int16_t r = (int16_t)(7000.0 * sin(t * 0.019 + 0.5));
        sec[i * 4 + 0] = (uint8_t)(l & 0xFF);
        sec[i * 4 + 1] = (uint8_t)((l >> 8) & 0xFF);
        sec[i * 4 + 2] = (uint8_t)(r & 0xFF);
        sec[i * 4 + 3] = (uint8_t)((r >> 8) & 0xFF);
    }
}

static void make_audio_sector_noise(uint8_t *sec, uint32_t seed)
{
    uint32_t i;
    xs_state = seed * 747796405u + 12345;
    for (i = 0; i < 2352; i++)
        sec[i] = (uint8_t)xs_next();
}

static int do_gen(const char *dir)
{
    char path[1024];
    uint8_t sec[2352];
    uint32_t i;
    FILE *f;

    snprintf(path, sizeof(path), "%s/test.bin", dir);
    f = fopen(path, "wb");
    if (!f) { perror("test.bin"); return 1; }

    for (i = 0; i < T1_SECTORS; i++)
    {
        make_mode1_sector(sec, T1_FAD + i, i);
        fwrite(sec, 1, 2352, f);
    }
    for (i = 0; i < T2_SECTORS; i++)
    {
        make_audio_sector_sine(sec, i);
        fwrite(sec, 1, 2352, f);
    }
    for (i = 0; i < T3_PREGAP; i++)
    {
        /* pregap INDEX00 de track 3: casi silencio con rampa */
        uint32_t j;
        for (j = 0; j < 2352; j++) sec[j] = (uint8_t)((i + j) & 0x07);
        fwrite(sec, 1, 2352, f);
    }
    for (i = 0; i < T3_SECTORS; i++)
    {
        make_audio_sector_noise(sec, i);
        fwrite(sec, 1, 2352, f);
    }
    fclose(f);

    snprintf(path, sizeof(path), "%s/test.cue", dir);
    f = fopen(path, "w");
    if (!f) { perror("test.cue"); return 1; }
    fprintf(f,
        "FILE \"test.bin\" BINARY\n"
        "  TRACK 01 MODE1/2352\n"
        "    INDEX 01 00:00:00\n"
        "  TRACK 02 AUDIO\n"
        "    PREGAP 00:02:00\n"
        "    INDEX 01 %02u:%02u:%02u\n"
        "  TRACK 03 AUDIO\n"
        "    INDEX 00 %02u:%02u:%02u\n"
        "    INDEX 01 %02u:%02u:%02u\n",
        T1_SECTORS / 4500, (T1_SECTORS / 75) % 60, T1_SECTORS % 75,
        (T1_SECTORS + T2_SECTORS) / 4500,
        ((T1_SECTORS + T2_SECTORS) / 75) % 60,
        (T1_SECTORS + T2_SECTORS) % 75,
        (T1_SECTORS + T2_SECTORS + T3_PREGAP) / 4500,
        ((T1_SECTORS + T2_SECTORS + T3_PREGAP) / 75) % 60,
        (T1_SECTORS + T2_SECTORS + T3_PREGAP) % 75);
    fclose(f);

    printf("Generado: %u sectores (%.1f MB)\n",
           T1_SECTORS + T2_SECTORS + T3_PREGAP + T3_SECTORS,
           (double)(T1_SECTORS + T2_SECTORS + T3_PREGAP + T3_SECTORS) * 2352 / 1e6);
    return 0;
}

/* offset esperado en el BIN para un FAD dado; -1 = zona de silencio */
static long expected_bin_offset(uint32_t fad)
{
    if (fad >= T1_FAD && fad < T1_FAD + T1_SECTORS)
        return (long)(fad - T1_FAD) * 2352;
    if (fad >= T2_START_FAD && fad < T2_START_FAD + T2_SECTORS)
        return (long)(T1_SECTORS + (fad - T2_START_FAD)) * 2352;
    if (fad >= T3_FILE_FAD && fad < T3_START_FAD + T3_SECTORS)
        return (long)(T1_SECTORS + T2_SECTORS + (fad - T3_FILE_FAD)) * 2352;
    return -1;
}

static int do_check(const char *chd_path, const char *bin_path)
{
    static uint8_t got[2352], want[2352];
    static const uint8_t zeros[2352];
    u32 toc[102];
    FILE *fbin;
    uint32_t fad;
    long mismatches = 0, checked = 0, silent = 0;
    int rc = 0;

    fbin = fopen(bin_path, "rb");
    if (!fbin) { perror(bin_path); return 1; }

    if (CHDCD.Init(chd_path) != 0)
    {
        fprintf(stderr, "FALLO: Init(%s)\n", chd_path);
        return 1;
    }

    CHDCD.ReadTOC(toc);

    printf("TOC[0]=%08X TOC[1]=%08X TOC[2]=%08X TOC[3]=%08X\n",
           toc[0], toc[1], toc[2], toc[3]);
    printf("TOC[99]=%08X TOC[100]=%08X TOC[101]=%08X\n",
           toc[99], toc[100], toc[101]);

    struct { u32 want; u32 got; const char *name; } tocchk[] = {
        { 0x41000000u | T1_FAD,       toc[0],   "track1"  },
        { 0x01000000u | T2_START_FAD, toc[1],   "track2"  },
        { 0x01000000u | T3_START_FAD, toc[2],   "track3"  },
        { 0xFFFFFFFFu,                toc[3],   "track4(vacia)" },
        { 0x41000000u | 0x010000u,    toc[99],  "puntoA0" },
        { 0x01000000u | (3u << 16),   toc[100], "puntoA1" },
        { 0x01000000u | LEADOUT_FAD,  toc[101], "leadout" },
    };
    for (unsigned i = 0; i < sizeof(tocchk)/sizeof(tocchk[0]); i++)
    {
        if (tocchk[i].want != tocchk[i].got)
        {
            printf("TOC MAL %s: esperado %08X, leido %08X\n",
                   tocchk[i].name, tocchk[i].want, tocchk[i].got);
            rc = 1;
        }
    }

    for (fad = T1_FAD; fad < LEADOUT_FAD; fad++)
    {
        long off = expected_bin_offset(fad);

        if (CHDCD.ReadSectorFAD(fad, got) != 1)
        {
            printf("ReadSectorFAD(%u) devolvio error\n", fad);
            rc = 1;
            break;
        }

        if (off < 0)
        {
            memcpy(want, zeros, 2352);
            silent++;
        }
        else
        {
            fseek(fbin, off, SEEK_SET);
            if (fread(want, 1, 2352, fbin) != 2352)
            {
                printf("fread bin fallo en offset %ld\n", off);
                rc = 1;
                break;
            }
        }

        if (memcmp(got, want, 2352) != 0)
        {
            if (mismatches < 5)
            {
                int fb = -1;
                for (int b = 0; b < 2352; b++)
                    if (got[b] != want[b]) { fb = b; break; }
                printf("DIFIERE FAD=%u primer_byte=%d got=%02X want=%02X\n",
                       fad, fb, got[fb], want[fb]);
            }
            mismatches++;
        }
        checked++;
    }

    CHDCD.DeInit();
    fclose(fbin);

    printf("Sectores verificados: %ld (de ellos %ld de silencio)\n", checked, silent);
    printf("Diferencias: %ld\n", mismatches);

    if (mismatches || rc)
    {
        printf("RESULTADO: FALLO\n");
        return 1;
    }
    printf("RESULTADO: OK — lectura CHD byte-exacta en todo el disco\n");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "gen"))
        return do_gen(argv[2]);
    if (argc >= 4 && !strcmp(argv[1], "check"))
        return do_check(argv[2], argv[3]);
    fprintf(stderr, "uso: %s gen <dir> | check <chd> <bin>\n", argv[0]);
    return 2;
}
