/* texpack.rs's way into runtime/tex_decode.c: one texture as XT1 headers describe it. layout: XT1's first layout word, the
 * bytes nvn_gl.c tex_lay takes from the builder (byte 0 block height, byte 1 block width). */
#include <stdint.h>
#include "tex_decode.h"
static int lay_of(TexLayout *L, int target, int fmt, int w, int h, int d, int mips, int layout) {
    int b0 = layout & 255, b1 = layout >> 8 & 255;
    return tex_layout(L, target, fmt, w, h, d, mips, 0, 0, b0 <= 4 ? b0 : -1, b1);
}
long long tx_info(int target, int fmt, int w, int h, int d, int mips, int layout, int *levels, int *layers) {
    TexLayout L;
    if (lay_of(&L, target, fmt, w, h, d, mips, layout)) return -1;
    *levels = L.levels; *layers = L.layers;
    return (long long)L.data_size;
}
void tx_level(int target, int fmt, int w, int h, int d, int mips, int layout, int level, int *lw, int *lh, long long *lin) {
    TexLayout L;
    lay_of(&L, target, fmt, w, h, d, mips, layout);
    *lw = L.lvl_w[level]; *lh = L.lvl_h[level]; *lin = (long long)L.lvl_linear_size[level];
}
int tx_decode(int target, int fmt, int w, int h, int d, int mips, int layout, const uint8_t *src, int level, int layer, uint8_t *tmp, uint8_t *rgba) {
    TexLayout L;
    lay_of(&L, target, fmt, w, h, d, mips, layout);
    tex_deswizzle_level(&L, level, tex_level_ptr(&L, src, level, layer), tmp);
    return tex_decode_rgba8(fmt, tmp, L.lvl_w[level], L.lvl_h[level], rgba);
}
