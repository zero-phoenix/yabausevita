#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>

#include "chd_read.h"

static int r8(const uint8_t *p) { return p[0]; }

static uint32_t r32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t r64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

static uint64_t r48(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40);
}

int chd_extract(const char *chd_path, const char *bin_path, char *error, int error_size)
{
    SceUID fd = sceIoOpen(chd_path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        if (error) snprintf(error, error_size, "Cannot open %s", chd_path);
        return -1;
    }

    uint8_t raw[168];
    int got = sceIoRead(fd, raw, 168);
    if (got < 12) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "File too small (%d bytes)", got);
        return -1;
    }

    if (memcmp(raw, "MComprHD", 8) != 0) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "Not a CHD file (bad magic)");
        return -1;
    }

    uint32_t version = r32(raw + 12);
    if (version < 1 || version > 8) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "CHD v%u not supported (need v1-v8)", version);
        return -1;
    }

    uint32_t hdr_len = r32(raw + 8);
    uint32_t compression = (got >= 20) ? r32(raw + 16) : 0;
    uint64_t logbytes, logicalsize;
    uint32_t hunkcount;
    uint64_t unitbytes;
    uint32_t map_offset = 0;

    if (version <= 4) {
        if (got < 72) {
            sceIoClose(fd);
            if (error) snprintf(error, error_size, "Truncated v%u header", version);
            return -1;
        }
        logbytes = r64(raw + 16);
        logicalsize = r64(raw + 24);
        hunkcount = r32(raw + 32);
        unitbytes = 512;
        if (version >= 3) {
            if (got >= 88) unitbytes = r64(raw + 80);
        }
        /* v1-v4 map entries are 8 bytes, right after header or at offset in v4 */
        if (version <= 3) {
            map_offset = hdr_len;
        } else {
            /* v4: map_offset at offset 0x58 (88) */
            if (got >= 92) map_offset = r32(raw + 88);
            if (map_offset == 0) map_offset = hdr_len;
        }
    } else {
        /* v5+ */
        if (got < 168) {
            sceIoClose(fd);
            if (error) snprintf(error, error_size, "Truncated v%u header", version);
            return -1;
        }
        logbytes = r64(raw + 60);
        logicalsize = r64(raw + 68);
        hunkcount = r32(raw + 80);
        unitbytes = r64(raw + 84);
        map_offset = r32(raw + 156);
    }

    if (hunkcount == 0 || logbytes == 0) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "Invalid CHD: zero hunks or hunk size");
        return -1;
    }

    if (unitbytes != 2352 && unitbytes != 2048 && unitbytes != 512 && unitbytes != 0) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "Unsupported CHD unit size: %llu", (unsigned long long)unitbytes);
        return -1;
    }
    if (unitbytes == 0) unitbytes = 512;

    SceUID out = sceIoOpen(bin_path, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
    if (out < 0) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "Cannot create %s", bin_path);
        return -1;
    }

    int entry_size = (version <= 4) ? 8 : 16;

    uint8_t *decomp = (uint8_t *)malloc((size_t)logbytes);
    if (!decomp) {
        sceIoClose(fd);
        sceIoClose(out);
        if (error) snprintf(error, error_size, "Out of memory (%llu bytes)", (unsigned long long)logbytes);
        return -1;
    }

    int result = 0;
    uint8_t entry[16];
    uint8_t *comp = NULL;

    for (uint32_t i = 0; i < hunkcount; i++) {
        sceIoLseek(fd, map_offset + i * entry_size, SCE_SEEK_SET);
        if (sceIoRead(fd, entry, entry_size) != entry_size) {
            if (error) snprintf(error, error_size, "Failed to read map entry %u", i);
            result = -1;
            break;
        }

        uint64_t block_off;
        uint64_t block_len;
        uint8_t comp_type = 0;

        if (version <= 4) {
            block_off = r32(entry);
            block_len = r32(entry + 4);
            comp_type = (version >= 2) ? (compression & 0x0F) : 0;
        } else {
            block_off = r48(entry);
            block_len = r48(entry + 8);
            comp_type = entry[14] & 0x0F;
        }

        memset(decomp, 0, (size_t)logbytes);

        if (block_off == 0 || block_len == 0) {
            if (sceIoWrite(out, decomp, (SceSize)logbytes) < 0) {
                if (error) snprintf(error, error_size, "Write error at hunk %u", i);
                result = -1;
                break;
            }
            continue;
        }

        if (block_len == logbytes || comp_type == 0) {
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            if (sceIoRead(fd, decomp, (SceSize)logbytes) != (int)logbytes) {
                if (error) snprintf(error, error_size, "Read error at hunk %u", i);
                result = -1;
                break;
            }
        } else {
            comp = (uint8_t *)realloc(comp, (size_t)block_len);
            if (!comp) {
                if (error) snprintf(error, error_size, "OOM at hunk %u", i);
                result = -1;
                break;
            }
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            if (sceIoRead(fd, comp, (SceSize)block_len) != (int)block_len) {
                if (error) snprintf(error, error_size, "Read error at hunk %u", i);
                result = -1;
                break;
            }

            unsigned long dest = (unsigned long)logbytes;
            int zret;
            if (comp_type <= 1) {
                zret = uncompress(decomp, &dest, comp, (unsigned long)block_len);
            } else {
                z_stream strm;
                memset(&strm, 0, sizeof(strm));
                zret = inflateInit(&strm);
                if (zret == Z_OK) {
                    strm.next_in = comp;
                    strm.avail_in = (unsigned int)block_len;
                    strm.next_out = decomp;
                    strm.avail_out = (unsigned int)logbytes;
                    zret = inflate(&strm, Z_FINISH);
                    inflateEnd(&strm);
                    dest = (unsigned long)logbytes - strm.avail_out;
                }
            }
            if (zret != Z_OK && zret != Z_STREAM_END) {
                if (error) snprintf(error, error_size, "Decompress error at hunk %u (%d)", i, zret);
                result = -1;
                break;
            }
        }

        if (sceIoWrite(out, decomp, (SceSize)logbytes) < 0) {
            if (error) snprintf(error, error_size, "Write error at hunk %u", i);
            result = -1;
            break;
        }
    }

    free(decomp);
    free(comp);
    sceIoClose(out);
    sceIoClose(fd);

    if (result != 0) sceIoRemove(bin_path);

    return result;
}
