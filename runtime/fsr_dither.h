/* [witchtime] issue 118, step 2: phase p of FSR_DITHER_PHASES of the game's 64 x 64 dither pattern while FSR runs
 * (nvn_vk_fsr.c; test: tests/fsr_dither_test.c). The pattern (RGBA8) holds thresholds in R and G = 255 - R: an object fading
 * out discards where its alpha is below R, the other mesh of a cross-fade where its alpha is below G, so the two cover each
 * pixel once. Its values are not spread evenly (none in 96 .. 127), so the phases rotate RANKS, not values: phase p gives texel
 * i the R and G of the texel p * 4096 / 13 places further in the order of R. Every phase is then a permutation of the game's
 * own values: a frame covers exactly what the game's pattern covers at any alpha. Over the phases a texel meets 13 ranks
 * spread evenly, so the share of frames it is drawn is the share the game's pattern draws at that alpha, and FSR's history
 * averages it. The frame order 8 * frame mod 13 steps by about the golden ratio: any few frames in a row are spread too, and 13
 * shares no factor with the 8 steps of FSR's jitter. B and A are kept. */
#ifndef FSR_DITHER_H
#define FSR_DITHER_H
#include <stdint.h>
#define FSR_DITHER_PHASES 13
static int fsr_dither_phase_of(long long frame) { return (int)((8 * frame) % FSR_DITHER_PHASES); }
static void fsr_dither_phase(const uint8_t *src, uint8_t *dst, int p) {
    static int ord[4096], rank[4096];
    int count[257] = {0};
    for (int i = 0; i < 4096; i++) count[src[i * 4] + 1]++;
    for (int v = 1; v < 257; v++) count[v] += count[v - 1];
    for (int i = 0; i < 4096; i++) { int r = count[src[i * 4]]++; ord[r] = i; rank[i] = r; }      /* stable: equal values keep their order */
    int shift = p * 4096 / FSR_DITHER_PHASES;
    for (int i = 0; i < 4096; i++) {
        int j = ord[(rank[i] + shift) & 4095];
        dst[i * 4] = src[j * 4]; dst[i * 4 + 1] = src[j * 4 + 1]; dst[i * 4 + 2] = src[i * 4 + 2]; dst[i * 4 + 3] = src[i * 4 + 3];
    }
}
/* the game's pattern, as the phases need it: every texel R 1 .. 254 and G = 255 - R */
static int fsr_dither_is_pattern(const uint8_t *src) {
    for (int i = 0; i < 4096; i++) if (src[i * 4] < 1 || src[i * 4] > 254 || src[i * 4] + src[i * 4 + 1] != 255) return 0;
    return 1;
}
#endif
