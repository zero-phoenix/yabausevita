#include <stdlib.h>
#include "bitstream.h"

int bitstream_overflow(struct bitstream *bs) {
    return (bs->doffset - bs->bits / 8) > bs->dlength;
}

struct bitstream *create_bitstream(const void *src, uint32_t srclength) {
    struct bitstream *bs = (struct bitstream *)malloc(sizeof(struct bitstream));
    if (!bs) return NULL;
    bs->buffer = 0;
    bs->bits = 0;
    bs->read = (const uint8_t *)src;
    bs->doffset = 0;
    bs->dlength = srclength;
    return bs;
}

void delete_bitstream(struct bitstream *bs) {
    free(bs);
}

uint32_t bitstream_peek(struct bitstream *bs, int numbits) {
    if (numbits == 0) return 0;
    while (numbits > bs->bits) {
        if (bs->doffset < bs->dlength)
            bs->buffer |= bs->read[bs->doffset] << (24 - bs->bits);
        bs->doffset++;
        bs->bits += 8;
    }
    return bs->buffer >> (32 - numbits);
}

void bitstream_remove(struct bitstream *bs, int numbits) {
    bs->buffer <<= numbits;
    bs->bits -= numbits;
}

uint32_t bitstream_read(struct bitstream *bs, int numbits) {
    uint32_t result = bitstream_peek(bs, numbits);
    bitstream_remove(bs, numbits);
    return result;
}

uint32_t bitstream_flush(struct bitstream *bs) {
    while (bs->bits >= 8) {
        bs->doffset--;
        bs->bits -= 8;
    }
    bs->bits = 0;
    bs->buffer = 0;
    return bs->doffset;
}
