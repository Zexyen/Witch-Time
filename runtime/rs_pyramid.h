/* [issue 93] B3's min/max depth pyramid at a resolution scale. fs5548 stores the 1/2 level (a storage image, so it stays at the
 * game's size: g_rs_keep), fs5550 halves it to 1/4 and 1/8, and fs2263, in a pass with no attachments that cannot be scaled,
 * gathers 2x2 texels of the 1/8 level for each 16-pixel tile to build the tile's light mask. On a scaled 1/8 level those texels
 * are a fraction of the tile (17 % at 2.4), so a foreground edge outside them is left out of the tile's depth range: black
 * corners and cut-out steps along it. The levels below a kept image must stay at the game's size as well.
 *
 * Is a draw from the kept image src into one colour target the next level? fs5550 alone passes: it addresses by pixel position,
 * so it has no inputs, and it works on RGBA16F. fs1727 (a radial mask) reads the same image through a UV, fs1697 (the blur
 * chain) starts from the kept R11G11B10F image, the clouds' targets are not half the image. One texel of play: an odd size
 * halves either way. */
#ifndef RS_PYRAMID_H
#define RS_PYRAMID_H
#include <stdlib.h>
#define RS_PYRAMID_FORMAT 0x29                     /* RGBA16F (tex_decode.h TEX_FMT_RGBA16F) */
static int rs_pyramid_next(int src_format, int src_w, int src_h, unsigned stage_inputs, int format, int w, int h) {
    return !stage_inputs && src_format == RS_PYRAMID_FORMAT && format == RS_PYRAMID_FORMAT && abs(w * 2 - src_w) <= 1 && abs(h * 2 - src_h) <= 1;
}
#endif
