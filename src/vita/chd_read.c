#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <zlib.h>
#include "chd_read.h"
#include "bitstream.h"
#include "huffman.h"
#include "lzma_dec.h"

extern int vita_log(const char *fmt, ...);

/* ── LZMA allocator (malloc/free) for the public-domain LZMA SDK ── */
static void *lzma_alloc(ISzAllocPtr p, size_t size) {
    (void)p;
    if (size == 0) size = 1;
    return malloc(size);
}
static void lzma_free(ISzAllocPtr p, void *address) {
    (void)p;
    free(address);
}
static ISzAlloc g_lzma_alloc = { lzma_alloc, lzma_free };

/* Decompress a single LZMA stream.
   props = 5-byte LZMA properties header (lc/lp/pb + dictSize).
   Returns bytes written to out, or -1 on failure. */
static int lzma_decompress_one(const uint8_t *props,
                                const uint8_t *in, uint32_t in_size,
                                uint8_t *out, uint32_t out_cap) {
    CLzmaDec state;
    SRes sres = LzmaDec_Allocate(&state, props, 5, &g_lzma_alloc);
    if (sres != SZ_OK) return -1;
    LzmaDec_Init(&state);

    SizeT in_len = in_size;
    SizeT out_len = out_cap;
    ELzmaStatus status;
    sres = LzmaDec_DecodeToBuf(&state, out, &out_len,
                                (Byte *)in, &in_len,
                                LZMA_FINISH_END, &status);
    LzmaDec_Free(&state, &g_lzma_alloc);
    if (sres != SZ_OK) return -1;
    return (int)out_len;
}


/* All CHD integers are big-endian (Motorola byte order) */
static uint32_t r32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
static uint64_t r64(const uint8_t *p) {
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) | ((uint64_t)p[2] << 40) |
           ((uint64_t)p[3] << 32) | ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8) | p[7];
}
static uint64_t r48(const uint8_t *p) {
    return ((uint64_t)p[0] << 40) | ((uint64_t)p[1] << 32) | ((uint64_t)p[2] << 24) |
           ((uint64_t)p[3] << 16) | ((uint64_t)p[4] << 8) | p[5];
}

static void hexdump_f(const uint8_t *d, int len) {
    char b[512]; int p = 0;
    for (int i = 0; i < len && p < 480; i++)
        p += snprintf(b + p, sizeof(b) - p, "%02x ", d[i]);
    b[p] = 0;
    vita_log("CHD HEX: %s\n", b);
}

int chd_extract(const char *chd_path, const char *bin_path, char *error, int error_size)
{
    if (error_size > 0) error[0] = '\0';
    vita_log("chd_extract: %s\n", chd_path);
    SceUID fd = sceIoOpen(chd_path, SCE_O_RDONLY, 0);
    if (fd < 0) { snprintf(error, error_size, "Cannot open"); return -1; }

    uint8_t raw[192];
    int got = sceIoRead(fd, raw, 192);
    if (got < 16) { sceIoClose(fd); snprintf(error, error_size, "Too small"); return -1; }
    if (memcmp(raw, "MComprHD", 8) != 0) { sceIoClose(fd); snprintf(error, error_size, "Bad magic"); return -1; }

    uint32_t hdr_len = r32(raw + 8);
    uint32_t version = r32(raw + 12);
    if (version < 1 || version > 5) {
        sceIoClose(fd);
        snprintf(error, error_size, "CHD v%u unsupported", version);
        return -1;
    }
    vita_log("  hdr_len=%u version=%u\n", hdr_len, version);

    uint64_t logbytes = 0, map_offset = 0, meta_offset = 0;
    uint32_t hunkcount = 0, hunkbytes = 0, unitbytes = 0;
    uint32_t compression = 0;

    if (version == 1) {
        if (got < 76) { sceIoClose(fd); snprintf(error, error_size, "Truncated v1"); return -1; }
        /* v1 header: length(8)+ver(12)+flags(16)+compression(20)+hunksize(24)+totalhunks(28) */
        /* +cylinders(32)+heads(36)+sectors(40)+md5(44)+parentmd5(60) = 76 bytes */
        compression = r32(raw + 20);
        uint32_t hunksize = r32(raw + 24);  /* 512-byte sectors per hunk */
        hunkcount   = r32(raw + 28);
        logbytes    = (uint64_t)hunksize * 512;
        hunkbytes   = (uint32_t)logbytes;
        map_offset  = hdr_len;
    } else if (version == 2) {
        if (got < 80) { sceIoClose(fd); snprintf(error, error_size, "Truncated v2"); return -1; }
        compression = r32(raw + 20);
        uint32_t hunksize = r32(raw + 24);
        hunkcount   = r32(raw + 28);
        uint32_t seclen = r32(raw + 76);
        logbytes    = (uint64_t)hunksize * seclen;
        hunkbytes   = (uint32_t)logbytes;
        map_offset  = hdr_len;
    } else if (version == 3) {
        if (got < 120) { sceIoClose(fd); snprintf(error, error_size, "Truncated v3"); return -1; }
        compression = r32(raw + 20);
        hunkcount   = r32(raw + 24);
        logbytes    = r64(raw + 28);    /* logicalbytes (total size) */
        meta_offset = r64(raw + 36);
        hunkbytes   = r32(raw + 76);
        map_offset  = hdr_len;
    } else if (version == 4) {
        if (got < 108) { sceIoClose(fd); snprintf(error, error_size, "Truncated v4"); return -1; }
        compression = r32(raw + 20);
        hunkcount   = r32(raw + 24);
        logbytes    = r64(raw + 28);
        meta_offset = r64(raw + 36);
        hunkbytes   = r32(raw + 44);
        map_offset  = hdr_len;
    } else {
        /* v5: logicalbytes at offset 32 = TOTAL logical size in bytes */
        /* hunkcount is NOT stored; calculated from logicalbytes/hunkbytes */
        if (got < 124) { sceIoClose(fd); snprintf(error, error_size, "Truncated v5"); return -1; }
        logbytes   = r64(raw + 32);   /* total logical size in bytes */
        map_offset = r64(raw + 40);   /* offset to map */
        meta_offset= r64(raw + 48);   /* offset to metadata */
        hunkbytes  = r32(raw + 56);   /* bytes per hunk */
        unitbytes  = r32(raw + 60);   /* bytes per unit */
        hunkcount  = (uint32_t)((logbytes + hunkbytes - 1) / hunkbytes);
    }

    vita_log("  hunkbytes=%u hunkcount=%u unitbytes=%u\n  logbytes=%llu map_offset=%llu\n",
             hunkbytes, hunkcount, unitbytes, (unsigned long long)logbytes,
             (unsigned long long)map_offset);

    if (hunkcount == 0 || hunkbytes == 0) {
        sceIoClose(fd);
        snprintf(error, error_size, "Invalid CHD (zero hunks/bytes)");
        return -1;
    }

    /* For v1/v2, logbytes is per-hunk, not total. total = logbytes * hunkcount */
    uint64_t total_bytes = version <= 2 ? (logbytes * hunkcount) : logbytes;
    uint32_t hunk_sz = version <= 2 ? (uint32_t)logbytes : hunkbytes;

    if (hunk_sz == 0) { sceIoClose(fd); snprintf(error, error_size, "Zero hunk size"); return -1; }

    SceUID out = sceIoOpen(bin_path, SCE_O_CREAT | SCE_O_WRONLY | SCE_O_TRUNC, 0777);
    if (out < 0) { sceIoClose(fd); snprintf(error, error_size, "Cannot create bin"); return -1; }

    uint8_t *decomp = (uint8_t *)malloc(hunk_sz);
    uint8_t *comp = NULL;
    int result = 0;

    int entry_size;
    if (version <= 2) entry_size = 8;
    else if (version <= 4) entry_size = 16;
    else entry_size = 16; /* v5 map header is same as entry size for processing */

    /* v1/v2: compression type from header */
    uint8_t v1_comp = (version <= 2) ? (compression & 0x0F) : 0;

    /* v5 map: compressed (Huffman) or uncompressed (uint32) */
    int v5_compressed = 1;
    uint8_t *v5_map = NULL;
    uint64_t v5_first_offs = 0;  /* absolute file offset of first hunk data */
    if (version == 5) {
        uint32_t c0 = r32(raw + 16);
        if (c0 == 0) {
            v5_compressed = 0;
        } else {
            /* read 16-byte map header */
            uint8_t map_hdr[16];
            sceIoLseek(fd, map_offset, SCE_SEEK_SET);
            if (sceIoRead(fd, map_hdr, 16) != 16) {
                sceIoClose(fd); sceIoClose(out); free(decomp);
                snprintf(error, error_size, "Cannot read v5 map hdr"); return -1;
            }
            uint32_t mapbytes = r32(map_hdr);
            v5_first_offs = r48(map_hdr + 4);
            uint8_t lengthbits = map_hdr[12];
            uint8_t selfbits = map_hdr[13];
            uint8_t parentbits = map_hdr[14];

            /* read huffman-encoded map data at map_offset+16 */
            uint8_t *compressed = (uint8_t *)malloc(mapbytes);
            if (!compressed) { result = -1; goto done; }
            sceIoLseek(fd, map_offset + 16, SCE_SEEK_SET);
            if (sceIoRead(fd, compressed, mapbytes) != mapbytes) {
                free(compressed); result = -1; goto done;
            }
            struct bitstream *bitbuf = create_bitstream(compressed, mapbytes);
            if (!bitbuf) { free(compressed); result = -1; goto done; }

            unsigned long rawmap_size = (unsigned long)hunkcount * 12;
            v5_map = (uint8_t *)malloc(rawmap_size);
            if (!v5_map) { free(compressed); delete_bitstream(bitbuf); result = -1; goto done; }

            struct huffman_decoder *decoder = create_huffman_decoder(16, 8);
            if (!decoder) { free(compressed); delete_bitstream(bitbuf); free(v5_map); v5_map = NULL; result = -1; goto done; }

            enum huffman_error herr = huffman_import_tree_rle(decoder, bitbuf);
            if (herr != HUFFERR_NONE) {
                free(compressed); delete_bitstream(bitbuf); delete_huffman_decoder(decoder);
                free(v5_map); v5_map = NULL; result = -1; goto done;
            }

            /* decode compression types */
            int repcount = 0;
            uint8_t lastcomp = 0;
            for (uint32_t hn = 0; hn < hunkcount; hn++) {
                uint8_t *rawmap = v5_map + (hn * 12);
                if (repcount > 0) {
                    rawmap[0] = lastcomp;
                    repcount--;
                } else {
                    uint8_t val = (uint8_t)huffman_decode_one(decoder, bitbuf);
                    if (val == 7) { /* COMPRESSION_RLE_SMALL */
                        rawmap[0] = lastcomp;
                        repcount = 2 + (int)huffman_decode_one(decoder, bitbuf);
                    } else if (val == 8) { /* COMPRESSION_RLE_LARGE */
                        rawmap[0] = lastcomp;
                        repcount = 2 + 16 + ((int)huffman_decode_one(decoder, bitbuf) << 4);
                        repcount += (int)huffman_decode_one(decoder, bitbuf);
                    } else {
                        rawmap[0] = val;
                        lastcomp = val;
                    }
                }
            }

            /* decode length/offset/crc for each hunk */
            uint64_t curoffset = v5_first_offs;
            uint32_t last_self = 0;
            uint64_t last_parent = 0;
            for (uint32_t hn = 0; hn < hunkcount; hn++) {
                uint8_t *rawmap = v5_map + (hn * 12);
                uint64_t offset = curoffset;
                uint32_t length = 0;
                uint16_t crc = 0;
                switch (rawmap[0]) {
                    case 0: case 1: case 2: case 3: /* COMPRESSION_TYPE_0..3 */
                        curoffset += length = bitstream_read(bitbuf, lengthbits);
                        crc = (uint16_t)bitstream_read(bitbuf, 16);
                        break;
                    case 4: /* COMPRESSION_NONE */
                        curoffset += length = hunkbytes;
                        crc = (uint16_t)bitstream_read(bitbuf, 16);
                        break;
                    case 5: /* COMPRESSION_SELF */
                        last_self = offset = bitstream_read(bitbuf, selfbits);
                        break;
                    case 6: /* COMPRESSION_PARENT */
                        offset = bitstream_read(bitbuf, parentbits);
                        last_parent = offset;
                        break;
                    case 9: /* COMPRESSION_SELF_1: last_self++, falls through */
                        last_self++;
                    case 10: /* COMPRESSION_SELF_0 */
                        rawmap[0] = 5; /* COMPRESSION_SELF */
                        offset = last_self;
                        break;
                    case 11: /* COMPRESSION_PARENT_SELF */
                        rawmap[0] = 6; /* COMPRESSION_PARENT */
                        last_parent = offset = (((uint64_t)hn) * (uint64_t)hunkbytes) / unitbytes;
                        break;
                    case 12: /* COMPRESSION_PARENT_1: last_parent += hunkbytes/unitbytes, falls through */
                        last_parent += hunkbytes / unitbytes;
                    case 13: /* COMPRESSION_PARENT_0 */
                        rawmap[0] = 6;
                        offset = last_parent;
                        break;
                }
                /* write expanded entry: UINT24 length, UINT48 offset, uint16 crc */
                rawmap[1] = (uint8_t)(length >> 16);
                rawmap[2] = (uint8_t)(length >> 8);
                rawmap[3] = (uint8_t)(length);
                rawmap[4] = (uint8_t)(offset >> 40);
                rawmap[5] = (uint8_t)(offset >> 32);
                rawmap[6] = (uint8_t)(offset >> 24);
                rawmap[7] = (uint8_t)(offset >> 16);
                rawmap[8] = (uint8_t)(offset >> 8);
                rawmap[9] = (uint8_t)(offset);
                rawmap[10] = (uint8_t)(crc >> 8);
                rawmap[11] = (uint8_t)(crc);
            }

            delete_huffman_decoder(decoder);
            delete_bitstream(bitbuf);
            free(compressed);
        }
    }
    for (uint32_t i = 0; i < hunkcount; i++)
    {
        uint64_t block_off = 0;
        uint32_t block_len = 0;
        uint8_t  comp_type = 0;

        if (version <= 2) {
            /* v1/v2: packed uint64_t - offset[44] + length[20] */
            uint8_t mentry[8];
            sceIoLseek(fd, map_offset + i * 8, SCE_SEEK_SET);
            if (sceIoRead(fd, mentry, 8) != 8) { result = -1; break; }
            uint64_t pack = r64(mentry);
            block_off = pack & ((1ULL << 44) - 1);
            block_len = (uint32_t)(pack >> 44);
            comp_type = v1_comp;
        } else if (version <= 4) {
            /* v3/v4: 16-byte entry */
            uint8_t mentry[16];
            sceIoLseek(fd, map_offset + i * 16, SCE_SEEK_SET);
            if (sceIoRead(fd, mentry, 16) != 16) { result = -1; break; }
            block_off = r64(mentry);
            uint8_t flags   = mentry[15];
            uint16_t len_lo = ((uint16_t)mentry[12] << 8) | mentry[13];
            uint8_t  len_hi = mentry[14];
            block_len = ((uint32_t)len_hi << 16) | len_lo;
            comp_type = (version == 4) ? (flags & 0x0F) : (compression & 0x0F);
            /* v3: bit 7 of flags indicates zlib; if not set and block_len == hunk_sz, uncompressed */
            if (version == 3 && (flags & 0x80))
                comp_type = 1; /* zlib */
        } else if (v5_map) {
            uint8_t *me = v5_map + i * 12;
            comp_type = me[0];
            block_len = ((uint32_t)me[1] << 16) | ((uint32_t)me[2] << 8) | me[3];
            block_off = r48(me + 4);
            if (comp_type == 5) { /* SELF: copy from hunk block_off */
                /* For sequential extraction, read from previously written output */
                sceIoLseek(out, (SceOff)block_off * hunk_sz, SCE_SEEK_SET);
                sceIoRead(out, decomp, hunk_sz);
                sceIoLseek(out, 0, SCE_SEEK_END);
                sceIoWrite(out, decomp, hunk_sz);
                continue;
            }
            if (comp_type == 6) { /* PARENT: no parent available */
                memset(decomp, 0, hunk_sz);
                sceIoWrite(out, decomp, hunk_sz);
                continue;
            }
        } else if (version == 5) {
            /* v5 uncompressed: map entries at map_offset (4 bytes each) */
            uint8_t mentry[4];
            sceIoLseek(fd, map_offset + i * 4, SCE_SEEK_SET);
            if (sceIoRead(fd, mentry, 4) != 4) { result = -1; break; }
            block_off = (uint64_t)r32(mentry) * hunk_sz;
            block_len = hunk_sz;
            comp_type = 0;
        }

        memset(decomp, 0, hunk_sz);

        if (block_off == 0 || block_len == 0) {
            sceIoWrite(out, decomp, hunk_sz);
            continue;
        }

        if (comp_type == 0 || comp_type == 4 || block_len >= hunk_sz) {
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            sceIoRead(fd, decomp, hunk_sz);
        } else if (comp_type == 2) {
            /* CD_LZ: LZMA compression. CHD stores raw LZMA stream per hunk
               (no 8-byte uncompressed-size trailer; 5-byte props header at start). */
            comp = (uint8_t *)realloc(comp, block_len);
            if (!comp) { result = -1; break; }
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            sceIoRead(fd, comp, block_len);

            if (block_len > 5) {
                const uint8_t *props = comp;
                const uint8_t *lz_in = comp + 5;
                uint32_t lz_size = block_len - 5;
                int written = lzma_decompress_one(props, lz_in, lz_size,
                                                   decomp, hunk_sz);
                if (written < 0) {
                    /* Fallback: try without skipping the 5-byte header
                       (some CHD variants don't embed props per hunk). */
                    written = lzma_decompress_one(props, comp, block_len,
                                                   decomp, hunk_sz);
                }
                if (written < 0) {
                    /* Last resort: copy literal (data will be corrupt but
                       extraction proceeds — audio/video artifacts expected). */
                    vita_log("CHD: hunk %u CD_LZ decode failed, literal copy\n", i);
                    memset(decomp, 0, hunk_sz);
                }
            } else {
                vita_log("CHD: hunk %u CD_LZ too small (%u bytes)\n", i, block_len);
                memset(decomp, 0, hunk_sz);
            }
        } else if (comp_type == 3) {
            /* CD_FL: FLAC stub - copy raw data literal (libflac not available) */
            vita_log("CHD: hunk %u CD_FL (FLAC) stub, copying literal %u bytes\n", i, block_len);
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            sceIoRead(fd, decomp, block_len < hunk_sz ? block_len : hunk_sz);
        } else if (comp_type == 1 && version == 5) {
            /* CD_ZL v5: per-sector zlib inflate */
            comp = (uint8_t *)realloc(comp, block_len);
            if (!comp) { result = -1; break; }
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            sceIoRead(fd, comp, block_len);
            uint8_t *p_in = comp;
            uint8_t *p_out = decomp;
            uint32_t remaining = block_len;
            while (remaining > 2 && p_out < decomp + hunk_sz) {
                z_stream zs;
                memset(&zs, 0, sizeof(zs));
                if (inflateInit(&zs) != Z_OK) break;
                zs.next_in = p_in;
                zs.avail_in = remaining;
                zs.next_out = p_out;
                zs.avail_out = (unsigned)(decomp + hunk_sz - p_out);
                int r = inflate(&zs, Z_SYNC_FLUSH);
                inflateEnd(&zs);
                if (r != Z_OK && r != Z_STREAM_END) break;
                uint32_t consumed = (uint32_t)zs.total_in;
                if (consumed == 0) break;
                p_in += consumed;
                remaining -= consumed;
                p_out += zs.total_out;
            }
        } else if (comp_type == 1) {
            /* CD_ZL v1-v4: single uncompress */
            comp = (uint8_t *)realloc(comp, block_len);
            if (!comp) { result = -1; break; }
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            sceIoRead(fd, comp, block_len);
            unsigned long dest = hunk_sz;
            int zret = uncompress(decomp, &dest, comp, block_len);
            if (zret != Z_OK) {
                snprintf(error, error_size, "Decompress hunk %u type=%d zret=%d", i, comp_type, zret);
                result = -1; break;
            }
        } else {
            comp = (uint8_t *)realloc(comp, block_len);
            if (!comp) { result = -1; break; }
            sceIoLseek(fd, block_off, SCE_SEEK_SET);
            sceIoRead(fd, comp, block_len);
            unsigned long dest = hunk_sz;
            z_stream strm; memset(&strm, 0, sizeof(strm));
            int zret = inflateInit(&strm);
            if (zret == Z_OK) {
                strm.next_in = comp; strm.avail_in = block_len;
                strm.next_out = decomp; strm.avail_out = hunk_sz;
                zret = inflate(&strm, Z_FINISH);
                dest = hunk_sz - strm.avail_out;
                inflateEnd(&strm);
            }
            if (zret != Z_OK && zret != Z_STREAM_END) {
                snprintf(error, error_size, "Decompress hunk %u type=%d zret=%d", i, comp_type, zret);
                result = -1; break;
            }
        }
        sceIoWrite(out, decomp, hunk_sz);
    }

done:
    if (result != 0 && error[0] == 0)
        snprintf(error, error_size, "CHD extraction error (code %d)", result);
    free(decomp); free(comp); free(v5_map);
    sceIoClose(out); sceIoClose(fd);
    if (result != 0) sceIoRemove(bin_path);
    return result;
}
