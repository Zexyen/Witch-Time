/* runtime/tex_decode.c tex_encode_bc7 through the BC7 decoder, as tests/bc3_encode_test.c does for BC3.
 *   clang -O2 -I runtime tests/bc7_encode_test.c runtime/tex_decode.c -lm -o /tmp/bc7_encode_test && /tmp/bc7_encode_test */
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include "tex_decode.h"

static double psnr(const uint8_t *a, const uint8_t *b, size_t n) {
    double se = 0;
    for (size_t i = 0; i < n; i++) se += (double)(a[i] - b[i]) * (a[i] - b[i]);
    return se ? 10 * log10(255.0 * 255.0 * (double)n / se) : 99;
}
int main(void) {
    static const int sz[][2] = {{64, 64}, {13, 7}, {1, 1}, {3, 5}};
    int bad = 0;
    srand(1);
    for (int s = 0; s < 4; s++)
        for (int kind = 0; kind < 4; kind++) {
            int w = sz[s][0], h = sz[s][1], bw = (w + 3) / 4, bh = (h + 3) / 4;
            uint8_t *src = malloc((size_t)w * h * 4), *out = malloc((size_t)w * h * 4), *blk = malloc((size_t)bw * bh * 16);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                uint8_t *p = src + ((size_t)y * w + x) * 4;
                if (kind == 0) { p[0] = (uint8_t)(x * 255 / w); p[1] = (uint8_t)(y * 255 / h); p[2] = (uint8_t)(255 - x * 255 / w); p[3] = (uint8_t)((x + y) * 255 / (w + h)); }
                else if (kind == 1) { p[0] = 200; p[1] = 30; p[2] = 90; p[3] = 255; }
                else if (kind == 2) { p[0] = p[1] = p[2] = 255; p[3] = (uint8_t)(x * 16 % 256); }      /* a mask: the alpha alone moves */
                else for (int c = 0; c < 4; c++) p[c] = (uint8_t)rand();
            }
            tex_encode_bc7(src, w, h, blk);
            if (tex_decode_rgba8(0x4D, blk, w, h, out)) { printf("decoder refused it  FAIL\n"); bad++; }
            double q = psnr(src, out, (size_t)w * h * 4), need = kind == 3 ? 8 : kind == 0 ? (w >= 64 ? 36 : 0) : kind == 1 ? 50 : 34;      /* as in bc3_encode_test.c: a tiny gradient of red against blue is a plane, no line holds it */
            printf("%dx%d kind %d: PSNR %.1f dB%s\n", w, h, kind, q, q < need ? "  FAIL" : "");
            bad += q < need;
            free(src); free(out); free(blk);
        }
    printf("%s\n", bad ? "FAIL" : "ok");
    return bad != 0;
}
