#ifndef HUFFMAN_H
#define HUFFMAN_H

#include <stdint.h>
#include "bitstream.h"

enum huffman_error {
    HUFFERR_NONE = 0,
    HUFFERR_INVALID_DATA,
    HUFFERR_INPUT_BUFFER_TOO_SMALL,
    HUFFERR_INTERNAL_INCONSISTENCY
};

struct node_t {
    struct node_t *parent;
    uint32_t count;
    uint32_t bits;
    uint32_t weight;
    uint32_t numbits;
};

typedef uint32_t lookup_value;

struct huffman_decoder {
    int numcodes;
    int maxbits;
    lookup_value *lookup;
    struct node_t *huffnode;
};

struct huffman_decoder *create_huffman_decoder(int numcodes, int maxbits);
void delete_huffman_decoder(struct huffman_decoder *decoder);
enum huffman_error huffman_import_tree_rle(struct huffman_decoder *decoder, struct bitstream *bitbuf);
uint32_t huffman_decode_one(struct huffman_decoder *decoder, struct bitstream *bitbuf);
enum huffman_error huffman_assign_canonical_codes(struct huffman_decoder *decoder);
void huffman_build_lookup_table(struct huffman_decoder *decoder);

#endif
