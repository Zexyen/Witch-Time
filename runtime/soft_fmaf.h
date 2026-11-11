/* [witchtime] The lifted code's fmaf (neon.h F_MLA / F_MLS / F_RECPS / F_RSQRTS, the lifter's FMADD family), defined in the game
 * library by game_shim.c so that those calls stay out of the C library: on a CPU without FMA3 (before Intel Haswell and
 * AMD Piledriver) the C library's fmaf is software, and the UCRT's took most of the game thread (issue 155: 6 fps on an
 * Ivy Bridge). Only game_shim.c includes this: the lifted files do not, so nobody's game compiles again for it.
 *
 * Self-test (CI runs it, wtbuild.yml): clang -O1 -DSOFT_FMAF_TEST -x c runtime/soft_fmaf.h -o t && ./t */
#ifndef WT_SOFT_FMAF_H
#define WT_SOFT_FMAF_H
#if defined(__x86_64__)
#include <stdint.h>
#include <string.h>
#include <cpuid.h>
#include <immintrin.h>

/* Without FMA3. The same result as the instruction, bit for bit, in round-to-nearest (the host never leaves it); the
 * method is FreeBSD's and musl's fmaf. x * y is exact in double, so the double sum is the one rounding, and rounding it
 * again to float can only differ from rounding the true sum when the double lies exactly half way between two floats
 * without being exact itself: then the sum's error says which neighbour the true sum is nearer to. */
static float wt_fmaf_soft(float x, float y, float z) {
#pragma STDC FP_CONTRACT OFF
    double xy = (double)x * y;
    double r = xy + z, err;
    uint64_t u;
    memcpy(&u, &r, 8);
    if ((u & 0x1fffffff) != 0x10000000 || (u >> 52 & 0x7ff) == 0x7ff || (r - xy == z && r - z == xy)) return (float)r;
    int neg = (int)(u >> 63);
    err = neg == (z > xy) ? xy - r + z : z - r + xy;
    u += neg == (err < 0) ? 1 : -1;
    memcpy(&r, &u, 8);
    return (float)r;
}

/* the instruction itself (not __builtin_fmaf, which may compile to a call of fmaf: of this file's user) */
__attribute__((target("fma"))) static float wt_fmaf_hw(float x, float y, float z) {
    return _mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(x), _mm_set_ss(y), _mm_set_ss(z)));
}

/* FMA3, and an operating system that keeps the AVX registers (the instruction is VEX-coded) */
static int wt_has_fma3(void) {
    unsigned a, b, c, d;
    if (!__get_cpuid(1, &a, &b, &c, &d) || (c & (1u << 12 | 1u << 27)) != (1u << 12 | 1u << 27)) return 0;    /* FMA, OSXSAVE */
    __asm__("xgetbv" : "=a"(a), "=d"(d) : "c"(0));
    return (a & 6) == 6;
}

#ifdef SOFT_FMAF_TEST
#include <stdio.h>
static uint64_t rng_s = 0x9e3779b97f4a7c15ull;
static uint32_t rng(void) { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return (uint32_t)(rng_s >> 16); }
static float bits(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static int same(float a, float b) { return a != a ? b != b : memcmp(&a, &b, 4) == 0; }
int main(void) {
    if (!wt_has_fma3()) { puts("soft_fmaf: no FMA3 here, nothing to compare with"); return 0; }
    long bad = 0, n = 0;
    for (long i = 0; i < 100000000 && bad < 10; i++, n++) {
        float x = bits(rng()), y = bits(rng()), z = bits(rng());
        switch (i & 7) {    /* random bits rarely cancel or tie: steer most cases to where the second rounding matters */
        case 1: z = -x * y; break;                                                     /* cancellation: the product's lost bits */
        case 2: z = x * y * bits(0x3f800000u + (rng() & 0xff)); break;                 /* the same magnitude */
        case 3: z = bits(rng() & 0x80ffffffu); break;                                  /* subnormal and tiny addends */
        case 4: x = bits((rng() & 0x807fffffu) | 0x1e000000u); y = bits((rng() & 0x807fffffu) | 0x1e800000u); break;  /* tiny products */
        case 5: {   /* the double sum lands exactly half way between two floats, the true sum just beside it: (1 + a)(1 - a) = 1 - a * a */
            float a = (float)(1 + (rng() & 127)) * 0x1p-23f;
            z = bits((rng() & 0x807fffffu) | 0x3f800000u); x = (1 + a) * (rng() & 1 ? 0x1p-12f : -0x1p-12f); y = (1 - a) * 0x1p-12f;
            break;
        }
        case 6: x = bits((rng() & 0x80ffffffu) | 0x3f000000u); y = bits((rng() & 0x80ffffffu) | 0x3f000000u); z = bits((rng() & 0x80ffffffu) | 0x3f000000u); break;
        }
        float s = wt_fmaf_soft(x, y, z), h = wt_fmaf_hw(x, y, z);
        if (!same(s, h)) { bad++; printf("soft_fmaf: fmaf(%a, %a, %a) = %a, the instruction gives %a\n", x, y, z, s, h); }
    }
    printf("soft_fmaf: %ld cases, %ld differ\n", n, bad);
    return bad != 0;
}
#endif
#endif
#endif
