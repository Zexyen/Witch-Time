/* Self-test of runtime/fsr_dither.h (issue 118, step 2):
 *   cc -Iruntime tests/fsr_dither_test.c -o /tmp/fsr_dither_test && /tmp/fsr_dither_test */
#include "fsr_dither.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    static uint8_t src[64 * 64 * 4], dst[FSR_DITHER_PHASES][64 * 64 * 4];
    for (int i = 0; i < 64 * 64; i++) {      /* like the game's: 2 x 2 blocks, R 1 .. 254 with no values in 96 .. 127, G = 255 - R */
        int x = i % 64, y = i / 64, r = 1 + ((x / 2) * 37 + (y / 2) * 91) % 222;
        if (r >= 96) r += 32;
        src[i * 4] = (uint8_t)r; src[i * 4 + 1] = (uint8_t)(255 - r); src[i * 4 + 2] = (uint8_t)(i & 255); src[i * 4 + 3] = (uint8_t)(i * 7);
    }
    assert(fsr_dither_is_pattern(src));
    { uint8_t bad[64 * 64 * 4]; memcpy(bad, src, sizeof bad); bad[1] = 7; assert(!fsr_dither_is_pattern(bad)); bad[1] = src[1]; bad[0] = 0; bad[1] = 255; assert(!fsr_dither_is_pattern(bad)); }
    for (int p = 0; p < FSR_DITHER_PHASES; p++) fsr_dither_phase(src, dst[p], p);
    assert(!memcmp(dst[0], src, sizeof src));                                  /* phase 0 is the game's pattern */
    int cdf[256];                                                              /* texels drawn at alpha a by the game's pattern: R <= a */
    for (int a = 0; a < 256; a++) { cdf[a] = 0; for (int i = 0; i < 4096; i++) cdf[a] += src[i * 4] <= a; }
    for (int p = 0; p < FSR_DITHER_PHASES; p++) {
        for (int a = 0; a < 256; a += 3) { int n = 0; for (int i = 0; i < 4096; i++) n += dst[p][i * 4] <= a; assert(n == cdf[a]); }      /* each frame covers what the game's does */
        for (int i = 0; i < 4096; i++) { uint8_t *d = &dst[p][i * 4]; assert(d[0] + d[1] == 255 && d[2] == src[i * 4 + 2] && d[3] == src[i * 4 + 3]); }      /* complementary; B, A kept */
    }
    /* per texel, the share of frames drawn follows the game's coverage: over all 13 phases within 1.5 frames, and over any 3 or
     * 5 frames in a row of the frame order (8 * frame mod 13) within 1.5 */
    int worst13 = 0, worst5 = 0, worst3 = 0;
    for (int i = 0; i < 4096; i++) for (int a = 0; a < 256; a += 5) {
        double want = cdf[a] / 4096.0;
        int n13 = 0; for (int p = 0; p < FSR_DITHER_PHASES; p++) n13 += dst[p][i * 4] <= a;
        double e = n13 - want * 13; if (e < 0) e = -e; if (e * 10 > worst13) worst13 = (int)(e * 10);
        assert(e <= 1.5);
        for (int f0 = 0; f0 < 26; f0++) for (int len = 3; len <= 5; len += 2) {
            int n = 0; for (int f = f0; f < f0 + len; f++) n += dst[fsr_dither_phase_of(f)][i * 4] <= a;
            double el = n - want * len; if (el < 0) el = -el;
            if (len == 3 && el * 10 > worst3) worst3 = (int)(el * 10);
            if (len == 5 && el * 10 > worst5) worst5 = (int)(el * 10);
            assert(el <= 1.5);
        }
    }
    printf("fsr_dither self-test: all checks passed (worst error in frames: 13 phases %.1f, any 5 in a row %.1f, any 3 in a row %.1f)\n", worst13 / 10.0, worst5 / 10.0, worst3 / 10.0);
    return 0;
}
