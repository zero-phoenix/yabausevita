#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>

#include "chd_read.h"

typedef struct {
    char     tag[8];
    uint32_t header_len;
    uint32_t version;
    uint32_t compress;
    uint8_t  parentsha1[20];
    uint8_t  fileSha1[20];
    uint64_t logbytes;
    uint64_t logical_size;
    uint32_t metaoffset;
    uint32_t hunkcount;
    uint64_t unitbytes;
    uint8_t  reserved[64];
    uint32_t mapoffset;
    uint8_t  reserved2[8];
} __attribute__((packed)) ChdHeader;

static int read_le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static int read_le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24); }
static uint64_t read_le48(const uint8_t *p) {
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

    ChdHeader hdr;
    int bytes = sceIoRead(fd, &hdr, sizeof(hdr));
    if (bytes < (int)sizeof(hdr)) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "Truncated CHD header");
        return -1;
    }

    if (memcmp(hdr.tag, "MComprHD", 8) != 0) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "Not a CHD file (bad magic)");
        return -1;
    }

    uint32_t version = hdr.version;
    if (version < 4 || version > 5) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "CHD v%u not supported (need v4/v5)", version);
        return -1;
    }

    uint64_t logical_bytes = hdr.logbytes;
    uint32_t hunk_count = hdr.hunkcount;
    uint64_t unit_bytes = hdr.unitbytes;

    if (unit_bytes != 2352 && unit_bytes != 2048 && unit_bytes != 512) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "Unsupported CHD unit size: %llu", (unsigned long long)unit_bytes);
        return -1;
    }

    SceUID out = sceIoOpen(bin_path, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
    if (out < 0) {
        sceIoClose(fd);
        if (error) snprintf(error, error_size, "Cannot create %s", bin_path);
        return -1;
    }

    uint32_t map_offset;
    if (version == 5) {
        map_offset = hdr.mapoffset;
    } else {
        /* For CHD v4, the map starts right after the 128-byte header and metadata */
        uint8_t raw[16];
        sceIoLseek(fd, 120, SCE_SEEK_SET);
        sceIoRead(fd, raw, 16);
        map_offset = read_le32(raw + 8);
        if (map_offset == 0) map_offset = 128;
    }

    void *decomp_buf = malloc((size_t)logical_bytes);
    if (!decomp_buf) {
        sceIoClose(fd);
        sceIoClose(out);
        if (error) snprintf(error, error_size, "Out of memory (%llu bytes)", (unsigned long long)logical_bytes);
        return -1;
    }

    int result = 0;
    uint8_t entry[16];
    unsigned long dest_len;

    for (uint32_t i = 0; i < hunk_count; i++) {
        sceIoLseek(fd, map_offset + i * 16, SCE_SEEK_SET);
        if (sceIoRead(fd, entry, 16) != 16) {
            if (error) snprintf(error, error_size, "Failed to read map entry %u", i);
            result = -1;
            break;
        }

        uint64_t block_off = read_le48(entry);
        uint32_t block_len = (uint32_t)read_le48(entry + 8);
        uint8_t  comp_type = entry[14] & 0x0F;

        memset(decomp_buf, 0, (size_t)logical_bytes);
        dest_len = (unsigned long)logical_bytes;

        if (block_off == 0 || block_len == 0) {
            /* Empty / zero-filled hunk — write zeros */
            if (sceIoWrite(out, decomp_buf, (SceSize)logical_bytes) < 0) {
                if (error) snprintf(error, error_size, "Write error at hunk %u", i);
                result = -1;
                break;
            }
            continue;
        }

        if (comp_type == 0) {
            /* Uncompressed */
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            if (sceIoRead(fd, decomp_buf, (SceSize)logical_bytes) != (int)logical_bytes) {
                if (error) snprintf(error, error_size, "Read error at hunk %u", i);
                result = -1;
                break;
            }
        } else if (comp_type == 1) {
            /* zlib (raw deflate, no header) */
            uint8_t *comp = (uint8_t *)malloc(block_len);
            if (!comp) {
                if (error) snprintf(error, error_size, "OOM reading hunk %u", i);
                result = -1;
                break;
            }
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            if (sceIoRead(fd, comp, block_len) != (int)block_len) {
                free(comp);
                if (error) snprintf(error, error_size, "Read error at hunk %u", i);
                result = -1;
                break;
            }
            if (uncompress((uint8_t *)decomp_buf, &dest_len, comp, block_len) != Z_OK) {
                free(comp);
                if (error) snprintf(error, error_size, "Decompress error at hunk %u", i);
                result = -1;
                break;
            }
            free(comp);
        } else if (comp_type == 2) {
            /* zlib+ (with zlib header) — inflate with header */
            uint8_t *comp = (uint8_t *)malloc(block_len);
            if (!comp) {
                if (error) snprintf(error, error_size, "OOM reading hunk %u", i);
                result = -1;
                break;
            }
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            if (sceIoRead(fd, comp, block_len) != (int)block_len) {
                free(comp);
                if (error) snprintf(error, error_size, "Read error at hunk %u", i);
                result = -1;
                break;
            }
            z_stream strm;
            memset(&strm, 0, sizeof(strm));
            if (inflateInit(&strm) != Z_OK) {
                free(comp);
                if (error) snprintf(error, error_size, "inflateInit error at hunk %u", i);
                result = -1;
                break;
            }
            strm.next_in = comp;
            strm.avail_in = block_len;
            strm.next_out = (uint8_t *)decomp_buf;
            strm.avail_out = (unsigned int)logical_bytes;
            int zret = inflate(&strm, Z_FINISH);
            inflateEnd(&strm);
            if (zret != Z_STREAM_END) {
                free(comp);
                if (error) snprintf(error, error_size, "Inflate error at hunk %u (ret=%d)", i, zret);
                result = -1;
                break;
            }
            free(comp);
        } else {
            if (error) snprintf(error, error_size, "Unsupported compression %d at hunk %u", comp_type, i);
            result = -1;
            break;
        }

        if (sceIoWrite(out, decomp_buf, (SceSize)logical_bytes) < 0) {
            if (error) snprintf(error, error_size, "Write error at hunk %u", i);
            result = -1;
            break;
        }
    }

    free(decomp_buf);
    sceIoClose(out);
    sceIoClose(fd);

    if (result != 0) {
        sceIoRemove(bin_path);
    }

    if (result == 0) {
        sceIoGetstat(bin_path, NULL);
    }

    return result;
}
