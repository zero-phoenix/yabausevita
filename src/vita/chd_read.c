#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>
#include "chd_read.h"

extern int vita_log(const char *fmt, ...);

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint64_t rd64(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}
static uint64_t rd48(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40);
}

static void hexdump(const uint8_t *data, int len) {
    char buf[256];
    int pos = 0;
    for (int i = 0; i < len && pos < 240; i += 1) {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "%02x ", data[i]);
    }
    buf[pos] = '\0';
    vita_log("CHD HEX: %s\n", buf);
}

int chd_extract(const char *chd_path, const char *bin_path, char *error, int error_size)
{
    vita_log("chd_extract: opening %s\n", chd_path);
    SceUID fd = sceIoOpen(chd_path, SCE_O_RDONLY, 0);
    if (fd < 0) {
        snprintf(error, error_size, "Cannot open %s", chd_path);
        return -1;
    }

    uint8_t raw[168];
    int got = sceIoRead(fd, raw, 168);
    vita_log("chd_extract: read %d bytes\n", got);
    hexdump(raw, got > 64 ? 64 : got);

    if (got < 16) {
        sceIoClose(fd);
        snprintf(error, error_size, "File too small (%d bytes)", got);
        return -1;
    }

    if (memcmp(raw, "MComprHD", 8) != 0) {
        sceIoClose(fd);
        snprintf(error, error_size, "Not a CHD file (bad magic)");
        return -1;
    }

    uint32_t version = rd32(raw + 12);
    uint32_t hdr_len = rd32(raw + 8);
    vita_log("chd_extract: hdr_len=%u version=%u\n", hdr_len, version);

    if (version < 1 || version > 8) {
        sceIoClose(fd);
        snprintf(error, error_size, "CHD v%u unsupported", version);
        return -1;
    }

    int min_hdr = (version <= 2) ? 40 : (version == 3) ? 100 : (version == 4) ? 124 : 160;
    if (got < min_hdr) {
        sceIoClose(fd);
        snprintf(error, error_size, "Truncated header (%d < %d)", got, min_hdr);
        return -1;
    }

    uint64_t logbytes = 0, logicalsize = 0, unitbytes = 512;
    uint32_t hunkcount = 0, compression = 0, map_offset = 0;

    if (version == 1 || version == 2) {
        logbytes    = rd64(raw + 16);
        logicalsize = rd64(raw + 24);
        hunkcount   = rd32(raw + 32);
        compression = rd32(raw + 36);
        map_offset  = hdr_len;
    } else if (version == 3) {
        logbytes    = rd64(raw + 16);
        logicalsize = rd64(raw + 24);
        hunkcount   = rd32(raw + 32);
        compression = rd32(raw + 36);
        map_offset  = hdr_len;
    } else if (version == 4) {
        logbytes    = rd64(raw + 16);
        logicalsize = rd64(raw + 24);
        hunkcount   = rd32(raw + 32);
        compression = rd32(raw + 36);
        map_offset  = rd32(raw + hdr_len - 24);
        if (map_offset < hdr_len) map_offset = hdr_len;
    } else {
        compression = rd32(raw + 16);
        logbytes    = rd64(raw + 60);
        logicalsize = rd64(raw + 68);
        hunkcount   = rd32(raw + 80);
        unitbytes   = rd64(raw + 84);
        map_offset  = rd32(raw + 156);
        if (map_offset == 0) map_offset = hdr_len;
    }

    vita_log("chd_extract: logbytes=%llu hunkcount=%u compression=%u map_offset=%u\n",
             logbytes, hunkcount, compression, map_offset);

    if (hunkcount == 0 || logbytes == 0) {
        sceIoClose(fd);
        snprintf(error, error_size, "Invalid CHD: zero hunks or hunk size");
        return -1;
    }

    SceUID out = sceIoOpen(bin_path, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
    if (out < 0) {
        sceIoClose(fd);
        snprintf(error, error_size, "Cannot create %s", bin_path);
        return -1;
    }

    int entry_size;
    if (version == 1 || version == 2) entry_size = 8;
    else if (version == 3 || version == 4) entry_size = 12;
    else entry_size = 16;

    uint8_t *decomp = (uint8_t *)malloc((size_t)logbytes);
    if (!decomp) {
        sceIoClose(fd); sceIoClose(out);
        snprintf(error, error_size, "Out of memory");
        return -1;
    }

    int result = 0;
    uint8_t entry[16];
    uint8_t *comp = NULL;

    for (uint32_t i = 0; i < hunkcount; i++) {
        sceIoLseek(fd, map_offset + i * entry_size, SCE_SEEK_SET);
        if (sceIoRead(fd, entry, entry_size) != entry_size) {
            snprintf(error, error_size, "Cannot read map entry %u", i);
            result = -1; break;
        }

        uint64_t block_off;
        uint64_t block_len;
        uint8_t  comp_type;

        if (version == 1 || version == 2) {
            block_off = rd32(entry);
            block_len = rd32(entry + 4);
            comp_type = compression & 0x0F;
        } else if (version == 3) {
            block_off = rd32(entry);
            uint32_t raw_len = rd32(entry + 4);
            if (raw_len & 0x80000000UL) {
                comp_type = 1;
                block_len = raw_len & 0x7FFFFFFFUL;
            } else {
                comp_type = 0;
                block_len = raw_len;
            }
        } else if (version == 4) {
            block_off = rd32(entry);
            uint32_t raw_len = rd32(entry + 4);
            comp_type = (raw_len >> 28) & 0x0F;
            block_len = raw_len & 0x0FFFFFFFUL;
        } else {
            block_off = rd48(entry);
            block_len = rd48(entry + 8);
            comp_type = entry[14] & 0x0F;
        }

        memset(decomp, 0, (size_t)logbytes);

        if (block_off == 0 || block_len == 0) {
            sceIoWrite(out, decomp, (SceSize)logbytes);
            continue;
        }

        if (comp_type == 0 || block_len == logbytes) {
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            sceIoRead(fd, decomp, (SceSize)logbytes);
        } else {
            comp = (uint8_t *)realloc(comp, (size_t)block_len);
            if (!comp) { result = -1; break; }
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            sceIoRead(fd, comp, (SceSize)block_len);

            unsigned long dest = (unsigned long)logbytes;
            int zret;
            if (comp_type == 1) {
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
                    dest = (unsigned long)logbytes - strm.avail_out;
                    inflateEnd(&strm);
                }
            }
            if (zret != Z_OK && zret != Z_STREAM_END) {
                snprintf(error, error_size, "Decompress err hunk %u comp=%d zret=%d", i, comp_type, zret);
                result = -1; break;
            }
        }

        sceIoWrite(out, decomp, (SceSize)logbytes);
    }

    free(decomp); free(comp);
    sceIoClose(out); sceIoClose(fd);

    if (result != 0) sceIoRemove(bin_path);
    return result;
}
