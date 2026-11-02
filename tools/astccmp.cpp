/* [witchtime] Offline check of the ASTC decoder of runtime/tex_decode.c against ARM's reference codec (astc-encoder,
 * Apache 2.0; not part of this repository) on random 128-bit blocks of every 2D footprint.
 *   linear formats: the reference's HDR profile, float output (Tegra decodes HDR endpoints in the LDR formats too),
 *                   turned into 8 bits as tex_decode.c does;
 *   sRGB formats:   the reference's LDR sRGB profile, 8-bit output, blocks without HDR endpoints.
 * Blocks the reference rejects (error colour / NaN) are left out; a texel more than 1 apart is a failure (exit 1).
 *
 *   git clone --depth 1 https://github.com/ARM-software/astc-encoder /tmp/astc-encoder
 *   S=/tmp/astc-encoder/Source
 *   g++ -O2 -std=c++17 -DASTCENC_SSE=0 -DASTCENC_AVX=0 -DASTCENC_NEON=0 -DASTCENC_POPCNT=0 -DASTCENC_F16C=0 -DASTCENC_SVE=0 \
 *       -DASTCENC_DECOMPRESS_ONLY -I$S -I runtime tools/astccmp.cpp $(ls $S/astcenc_*.cpp | grep -v cli) -x c runtime/tex_decode.c -o build/astccmp
 *   build/astccmp [blocks per footprint, default 20000]
 *   build/astccmp --file <dump>.astc ...   the blocks of a game image dumped with WT_ASTC_DUMP=<dir> (every block compared)
 * Every run first checks tex_layout's mip offsets for a block-width layout against the game's (layout_check).
 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include "astcenc.h"
extern "C" {
#include "tex_decode.h"
}
extern "C" int tex_decode_block_rgba8(int nf, const uint8_t *blk, uint8_t *out, int pitch);
extern "C" unsigned tex_astc_hdr_blocks, tex_astc_bad_blocks;
static const int k_fp[14][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
static uint64_t g_s = 88172645463325252ull;
static uint64_t rnd() { g_s ^= g_s << 13; g_s ^= g_s >> 7; g_s ^= g_s << 17; return g_s; }

/* --file <dir>/<format>_<w>x<h>_<hash>.astc (WT_ASTC_DUMP): the game's own blocks, every block of the image */
static int file_mode(const char *path) {
    unsigned nf; int w, h;
    const char *base = strrchr(path, '/') ? strrchr(path, '/') + 1 : path;
    if (sscanf(base, "%2x_%dx%d_", &nf, &w, &h) != 3 || nf < 0x79 || nf > 0x94) { printf("%s: name not <format>_<w>x<h>_<hash>.astc\n", path); return 2; }
    int srgb = nf >= 0x87, f = (int)(nf - (srgb ? 0x87 : 0x79)), bw = k_fp[f][0], bh = k_fp[f][1], wb = (w + bw - 1) / bw, hb = (h + bh - 1) / bh;
    FILE *fp = fopen(path, "rb");
    if (!fp) { perror(path); return 2; }
    size_t nb = (size_t)wb * hb;
    uint8_t *blocks = (uint8_t *)malloc(nb * 16);
    if (fread(blocks, 16, nb, fp) != nb) { printf("%s: short file\n", path); return 2; }
    fclose(fp);
    astcenc_config cfg;
    astcenc_context *ctx;
    if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_HDR, bw, bh, 1, ASTCENC_PRE_FAST, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) || astcenc_context_alloc(&cfg, 1, &ctx, nullptr)) return 2;
    long diff = 0, bad = 0, hdr = 0, maxd = 0;
    for (size_t k = 0; k < nb; k++) {
        uint8_t ref[144 * 4], us[144 * 4];
        float reff[144 * 4];
        void *data = srgb ? (void *)ref : (void *)reff;
        astcenc_image img;
        img.dim_x = bw; img.dim_y = bh; img.dim_z = 1; img.data_type = srgb ? ASTCENC_TYPE_U8 : ASTCENC_TYPE_F32; img.data = &data;
        astcenc_swizzle sw = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
        astcenc_decompress_image(ctx, blocks + k * 16, 16, &img, &sw, 0);
        astcenc_decompress_reset(ctx);
        if (!srgb) for (int i = 0; i < bw * bh * 4; i++) { float v = reff[i]; ref[i] = (uint8_t)(v != v ? 0 : v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f + 0.5f)); }
        unsigned h0 = tex_astc_hdr_blocks, b0 = tex_astc_bad_blocks;
        tex_decode_block_rgba8((int)nf, blocks + k * 16, us, bw * 4);
        if (tex_astc_bad_blocks != b0) {          /* rejected here: the reference must reject it too (error colour / NaN) */
            bool ref_err = false;
            for (int i = 0; i < bw * bh; i++) ref_err |= srgb ? ref[i * 4] == 255 && ref[i * 4 + 1] == 0 && ref[i * 4 + 2] == 255 && ref[i * 4 + 3] == 255 : reff[i * 4] != reff[i * 4];
            bad++;
            if (!ref_err && diff++ < 5) { printf("  block %zu rejected here, decoded by the reference: ", k); for (int i = 0; i < 16; i++) printf("%02x", blocks[k * 16 + i]); printf("\n"); }
            continue;
        }
        hdr += tex_astc_hdr_blocks != h0;
        int d = 0;
        for (int i = 0; i < bw * bh * 4; i++) { int e = abs(us[i] - ref[i]); if (e > d) d = e; }
        if (d > maxd) maxd = d;
        if (d > 1 && diff++ < 5) { printf("  block %zu (x %zu, y %zu) differs by %d: ", k, k % wb, k / wb, d); for (int i = 0; i < 16; i++) printf("%02x", blocks[k * 16 + i]); printf("\n"); }
    }
    printf("%s: %zu blocks, %ld invalid (left out), %ld with HDR endpoints, %ld more than 1 apart or rejected only here (max %ld)\n", base, nb, bad, hdr, diff, maxd);
    astcenc_context_free(ctx);
    return diff != 0;
}

/* tex_layout of a layout with a block width (byte 1 = 4): the mip offsets of the game's 1024x1024 ASTC 6x6 effect atlases as they lie in memory */
static int layout_check() {
    static const size_t want[10] = {0, 589824, 851968, 897024, 909312, 912384, 913408, 913920, 914432, 914944};
    TexLayout L;
    tex_layout(&L, TEX_TARGET_2D, 0x7d, 1024, 1024, 1, 10, 0, 0, 3, 4);
    for (int l = 0; l < 10; l++)
        if (L.lvl_offset[l] != want[l]) { printf("LAYOUT MISMATCH: level %d at %zu, the game's at %zu\n", l, L.lvl_offset[l], want[l]); return 1; }
    return 0;
}

int main(int argc, char **argv) {
    if (layout_check()) return 1;
    if (argc > 2 && !strcmp(argv[1], "--file")) { int r = 0; for (int i = 2; i < argc; i++) r |= file_mode(argv[i]); return r; }
    int n = argc > 1 ? atoi(argv[1]) : 20000;
    long fails = 0, checked = 0, hdr_checked = 0;
    for (int srgb = 0; srgb < 2; srgb++)
        for (int f = 0; f < 14; f++) {
            int bw = k_fp[f][0], bh = k_fp[f][1];
            astcenc_config cfg;
            astcenc_context *ctx;
            if (astcenc_config_init(srgb ? ASTCENC_PRF_LDR_SRGB : ASTCENC_PRF_HDR, bw, bh, 1, ASTCENC_PRE_FAST, ASTCENC_FLG_DECOMPRESS_ONLY, &cfg) || astcenc_context_alloc(&cfg, 1, &ctx, nullptr)) { printf("astcenc init failed\n"); return 2; }
            for (int k = 0; k < n; k++) {
                uint8_t blk[16], ref[144 * 4], us[144 * 4];
                float reff[144 * 4];
                for (int i = 0; i < 16; i += 8) { uint64_t v = rnd(); memcpy(blk + i, &v, 8); }
                if (k & 1) blk[0] &= 0xf3;                         /* more of the weight-grid layouts */
                void *data = srgb ? (void *)ref : (void *)reff;
                astcenc_image img;
                img.dim_x = bw; img.dim_y = bh; img.dim_z = 1; img.data_type = srgb ? ASTCENC_TYPE_U8 : ASTCENC_TYPE_F32; img.data = &data;
                astcenc_swizzle sw = {ASTCENC_SWZ_R, ASTCENC_SWZ_G, ASTCENC_SWZ_B, ASTCENC_SWZ_A};
                if (astcenc_decompress_image(ctx, blk, 16, &img, &sw, 0) != ASTCENC_SUCCESS) { printf("astcenc decompress failed\n"); return 2; }
                astcenc_decompress_reset(ctx);
                bool ref_err = false;
                if (!srgb) for (int i = 0; i < bw * bh * 4; i++) {
                    float v = reff[i];
                    if (v != v) ref_err = true;
                    ref[i] = (uint8_t)(v <= 0 ? 0 : v >= 1 ? 255 : (int)(v * 255.0f + 0.5f));
                } else for (int i = 0; i < bw * bh; i++) ref_err |= ref[i * 4] == 255 && ref[i * 4 + 1] == 0 && ref[i * 4 + 2] == 255 && ref[i * 4 + 3] == 255;
                unsigned hdr0 = tex_astc_hdr_blocks, bad0 = tex_astc_bad_blocks;
                tex_decode_block_rgba8((srgb ? 0x87 : 0x79) + f, blk, us, bw * 4);
                bool hdr = tex_astc_hdr_blocks != hdr0, bad = tex_astc_bad_blocks != bad0;
                if (ref_err || bad || (srgb && hdr)) continue;     /* rejected by either (the reference's sRGB profile rejects HDR endpoints) */
                int maxd = 0;
                for (int i = 0; i < bw * bh * 4; i++) { int d = abs(us[i] - ref[i]); if (d > maxd) maxd = d; }
                checked++; hdr_checked += hdr;
                if (maxd > 1 && fails++ < 10) {
                    printf("MISMATCH %s %dx%d (%s endpoints), max difference %d, block ", srgb ? "sRGB" : "linear", bw, bh, hdr ? "HDR" : "LDR", maxd);
                    for (int i = 0; i < 16; i++) printf("%02x", blk[i]);
                    printf("\n");
                }
            }
            astcenc_context_free(ctx);
        }
    printf("astccmp: %ld valid blocks compared (%ld with HDR endpoints), %ld more than 1 apart\n", checked, hdr_checked, fails);
    return fails != 0;
}
