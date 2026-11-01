/* #54: the pack of BC7 textures `wtbuild textures` makes from the game's ASTC textures. docs in tex_pack.c */
#ifndef TEX_PACK_H
#define TEX_PACK_H
#include <stddef.h>
#include <stdint.h>
#define TEX_PACK_VERSION 1
#pragma pack(push, 1)
typedef struct {
    uint64_t hash, data_size, offset;
    uint32_t stored, raw, format, width, height;
    uint16_t levels, layers, shard, pad;
    uint32_t pad2;
} TexPackEntry;                                    /* the same 56 bytes in the index and at a shard's end (tools/wtbuild/src/texpack.rs) */
#pragma pack(pop)
uint64_t tex_pack_hash(const uint8_t *p, size_t n);
int tex_pack_open(const char *dir);                /* the number of textures, 0 = no usable pack */
const TexPackEntry *tex_pack_find(uint64_t hash, size_t data_size, int format, int width, int height, int levels, int layers);
int tex_pack_read(const TexPackEntry *e, uint8_t *out);      /* e->raw bytes of BC7 blocks, level after level of each layer; 1 = done */
#endif
