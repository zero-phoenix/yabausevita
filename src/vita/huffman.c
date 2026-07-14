#include <stdlib.h>
#include <string.h>
#include "bitstream.h"
#include "huffman.h"

#define MAKE_LOOKUP(code, bits) (((code) << 5) | ((bits) & 0x1f))

struct huffman_decoder *create_huffman_decoder(int numcodes, int maxbits) {
    if (maxbits > 24) return NULL;
    struct huffman_decoder *decoder = (struct huffman_decoder *)malloc(sizeof(struct huffman_decoder));
    if (!decoder) return NULL;
    decoder->numcodes = numcodes;
    decoder->maxbits = maxbits;
    decoder->lookup = (lookup_value *)malloc(sizeof(lookup_value) * (1 << maxbits));
    decoder->huffnode = (struct node_t *)malloc(sizeof(struct node_t) * numcodes);
    if (!decoder->lookup || !decoder->huffnode) {
        free(decoder->lookup);
        free(decoder->huffnode);
        free(decoder);
        return NULL;
    }
    memset(decoder->huffnode, 0, sizeof(struct node_t) * numcodes);
    return decoder;
}

void delete_huffman_decoder(struct huffman_decoder *decoder) {
    if (decoder) {
        free(decoder->lookup);
        free(decoder->huffnode);
        free(decoder);
    }
}

uint32_t huffman_decode_one(struct huffman_decoder *decoder, struct bitstream *bitbuf) {
    uint32_t bits = bitstream_peek(bitbuf, decoder->maxbits);
    lookup_value lookup = decoder->lookup[bits];
    bitstream_remove(bitbuf, lookup & 0x1f);
    return lookup >> 5;
}

enum huffman_error huffman_import_tree_rle(struct huffman_decoder *decoder, struct bitstream *bitbuf) {
    int numbits;
    uint32_t curnode;

    if (decoder->maxbits >= 16)
        numbits = 5;
    else if (decoder->maxbits >= 8)
        numbits = 4;
    else
        numbits = 3;

    for (curnode = 0; curnode < (uint32_t)decoder->numcodes;) {
        int nodebits = (int)bitstream_read(bitbuf, numbits);
        if (nodebits != 1) {
            decoder->huffnode[curnode++].numbits = (uint32_t)nodebits;
        } else {
            nodebits = (int)bitstream_read(bitbuf, numbits);
            if (nodebits == 1) {
                decoder->huffnode[curnode++].numbits = 1;
            } else {
                int repcount = (int)bitstream_read(bitbuf, numbits) + 3;
                while (repcount--)
                    decoder->huffnode[curnode++].numbits = (uint32_t)nodebits;
            }
        }
    }

    if (curnode != (uint32_t)decoder->numcodes)
        return HUFFERR_INVALID_DATA;

    enum huffman_error error = huffman_assign_canonical_codes(decoder);
    if (error != HUFFERR_NONE) return error;

    huffman_build_lookup_table(decoder);
    return bitstream_overflow(bitbuf) ? HUFFERR_INPUT_BUFFER_TOO_SMALL : HUFFERR_NONE;
}

enum huffman_error huffman_assign_canonical_codes(struct huffman_decoder *decoder) {
    uint32_t curcode;
    uint32_t curstart = 0;
    uint32_t bithisto[33] = {0};

    for (curcode = 0; curcode < (uint32_t)decoder->numcodes; curcode++) {
        struct node_t *node = &decoder->huffnode[curcode];
        if (node->numbits > (uint32_t)decoder->maxbits)
            return HUFFERR_INTERNAL_INCONSISTENCY;
        if (node->numbits <= 32)
            bithisto[node->numbits]++;
    }

    for (int codelen = 32; codelen > 0; codelen--) {
        uint32_t nextstart = (curstart + bithisto[codelen]) >> 1;
        if (codelen != 1 && nextstart * 2 != (curstart + bithisto[codelen]))
            return HUFFERR_INTERNAL_INCONSISTENCY;
        bithisto[codelen] = curstart;
        curstart = nextstart;
    }

    for (curcode = 0; curcode < (uint32_t)decoder->numcodes; curcode++) {
        struct node_t *node = &decoder->huffnode[curcode];
        if (node->numbits > 0)
            node->bits = bithisto[node->numbits]++;
    }
    return HUFFERR_NONE;
}

void huffman_build_lookup_table(struct huffman_decoder *decoder) {
    for (uint32_t curcode = 0; curcode < (uint32_t)decoder->numcodes; curcode++) {
        struct node_t *node = &decoder->huffnode[curcode];
        if (node->numbits > 0) {
            lookup_value value = MAKE_LOOKUP(curcode, node->numbits);
            int shift = decoder->maxbits - (int)node->numbits;
            lookup_value *dest = &decoder->lookup[node->bits << shift];
            lookup_value *destend = &decoder->lookup[((node->bits + 1) << shift) - 1];
            while (dest <= destend)
                *dest++ = value;
        }
    }
}
