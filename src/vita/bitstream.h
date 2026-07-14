#ifndef BITSTREAM_H
#define BITSTREAM_H

#include <stdint.h>

struct bitstream {
    uint32_t buffer;
    int bits;
    const uint8_t *read;
    uint32_t doffset;
    uint32_t dlength;
};

struct bitstream *create_bitstream(const void *src, uint32_t srclength);
void delete_bitstream(struct bitstream *bs);
uint32_t bitstream_peek(struct bitstream *bs, int numbits);
void bitstream_remove(struct bitstream *bs, int numbits);
uint32_t bitstream_read(struct bitstream *bs, int numbits);
uint32_t bitstream_flush(struct bitstream *bs);
int bitstream_overflow(struct bitstream *bs);

#endif
