/* [witchtime] NVN texture storage rules answered by the SDK's own code, ported to Bayonetta 3's sdk (nnSdk / NVN 13.3.2,
 * API 55.12). Same API as B2recomp's port (nvn_sdk.h); the SDK code is lifted from targets/exefs/sdk.elf by
 * tools/wtbuild/src/sdklift.rs. Every address comes from runtime/gen/nvn_sdk_addrs.h, which it writes from the player's
 * own sdk: NVN entry points by name from the proc table, the vtables by their typeinfo names, the format table and
 * chip descriptor by shape (tools/wtbuild/src/nvn_procs.rs gpu_model). Nothing here is a hard-coded sdk address.
 *
 * The memory kind comes from the GPU library: nvnTextureBuilderGetStorageClass and the layout function call the NVN
 * chooser (device +0x990 -> NvRmGpuDeviceChooseKind on the object at device +0x998) -> NvRmNvgpuDevice method +0x30
 * (returns this +0xf0, the HAL object) -> NvRmGpu_HwHalFunctions_GM200_Base method +0x90, which dispatches to +0xa0,
 * +0x120 .. +0x170 and reads the chip descriptor's kind table. The HAL object is what the descriptor's factory builds:
 * 0x38 bytes, [0] vtable, [1] the descriptor ("NVIDIA Tegra/Nintendo Switch", GPU id 0x12b at +0x10, kind table at
 * +0x48, count at +0x50), rest 0 (two factory arguments at [2] / [3] are not read by the kind methods).
 *
 * NVNdevice model: only the fields the lifted code reads (docs/RE_NOTES.md section 6).
 *   +0xcc    u32  0x10000   big page size: the alignment of page-aligned kinds; nvnDeviceGetInteger also answers it.
 *                           Same value as B2's (+0x80) and Smash's (+0x90) on the same GPU: inferred, the writer is
 *                           in device initialisation from the GPU characteristics.
 *   +0x998   ptr  GPU device object
 *   +0x1a10, +0x1a14 u32 0  per-category compression switches; compression is disabled only when one equals 0x76165656.
 *                           nvnDeviceInitialize zeroes them (a 16-byte zero constant); a debug setting can change them.
 *   +0x1b30  u32  0         kind of buffer / linear textures: nvnDeviceInitialize stores 0.
 *   +0x1b34  u8   1         compression supported. Writer not found (stored through a pointer): NOT VERIFIED; 1 matches
 *                           B2's and Smash's choice on the same GPU.
 * The format table (0x30-byte records) and the kind tables are read from the mapped SDK image. */
#include <stdio.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"
#include "nvn_sdk.h"
#include "nvn_c4.h"
#include "nvn_c5.h"
#include "nvn_sdk_addrs.h"

#define SDK(a) (SDK_BASE + (uint64_t)(a))
#define P(v) ((uint64_t)(uintptr_t)(v))
#define FMT_REC 0x30ull

static uint8_t g_dev[0x1c00];                 /* NVNdevice model */
static uint64_t g_gpu_dev[0x100 / 8];         /* NvRmNvgpuDevice: vtable, HAL object at +0xf0 */
static uint64_t g_hal[0x38 / 8];              /* NvRmGpu_HwHalFunctions_GM200_Base */
static volatile LONG g_state;                 /* 0 not built, 1 building, 2 ready */

static void build_model(void) {
    if (!sdk_image_ready()) {
        printf("\nnvn_sdk: sdk.elf is not mapped; the texture storage rules are the SDK's own code and cannot be answered without it.\n");
        report_and_exit(3);
    }
    g_hal[0] = SDK(SDKA_vt_hal_gm200);
    g_hal[1] = SDK(SDKA_chip_descriptor);
    g_gpu_dev[0] = SDK(SDKA_vt_nvgpu_device);
    g_gpu_dev[0xf0 / 8] = P(g_hal);
    *(uint32_t *)(g_dev + 0xcc) = 0x10000;
    *(uint64_t *)(g_dev + 0x998) = P(g_gpu_dev);
    *(uint32_t *)(g_dev + 0x1a10) = 0;
    *(uint32_t *)(g_dev + 0x1a14) = 0;
    *(uint32_t *)(g_dev + 0x1b30) = 0;
    g_dev[0x1b34] = 1;
}
static void model(void) {
    for (;;) {
        LONG s = InterlockedCompareExchange(&g_state, 1, 0);
        if (s == 2) return;
        if (s == 0) { build_model(); InterlockedExchange(&g_state, 2); return; }
        Sleep(0);
    }
}
static const uint8_t *fmt_rec(uint32_t format) {
    return (const uint8_t *)(uintptr_t)(SDK(SDKA_format_table) + FMT_REC * (format < SDKA_format_count ? format : 0));
}

/* The NVNtextureBuilder of NVN 13.3.2, as its setters write it (nvnTextureBuilderSet*):
 * +8 device, +0x10 storage pool, +0x18 offset, +0x20 flags, +0x24 target, +0x28 width, +0x2c height, +0x30 depth,
 * +0x34 levels, +0x38 format, +0x3c samples, +0x40 swizzle (4 words), +0x50 depth-stencil mode, +0x54 u16 packaged layout
 * (1 | (word & 0xfff) << 4), +0x58 stride, +0x5c GL texture name, +0x60 packaged data. (B2's 3.6.0: the same fields 8 bytes
 * lower, device at +0, stride and GL name the other way round.) */
static void real_builder(uint8_t *rb, const NvnSdkTexBuilder *b) {
    memset(rb, 0, 0x80);
    *(uint64_t *)(rb + 8) = P(g_dev);
    *(uint32_t *)(rb + 0x20) = b->flags;
    *(uint32_t *)(rb + 0x24) = b->target;
    *(uint32_t *)(rb + 0x28) = b->width;
    *(uint32_t *)(rb + 0x2c) = b->height;
    *(uint32_t *)(rb + 0x30) = b->depth;
    *(uint32_t *)(rb + 0x34) = b->levels;
    *(uint32_t *)(rb + 0x38) = b->format;
    *(uint32_t *)(rb + 0x3c) = b->samples;
    if (b->has_layout) *(uint16_t *)(rb + 0x54) = (uint16_t)(1u | ((b->layout & 0xfffu) << 4));
    *(uint32_t *)(rb + 0x58) = b->stride;
    *(uint32_t *)(rb + 0x5c) = b->glname;
}

static uint64_t run(Ctx *c, uint64_t fn, const NvnSdkTexBuilder *b, uint64_t x1) {
    uint8_t rb[0x80];
    model();
    real_builder(rb, b);
    c->x[0] = P(rb); c->x[1] = x1;
    sdk_dispatch(c, SDK(fn));
    return c->x[0];
}
uint64_t nvn_sdk_texture_storage_alignment(Ctx *c, const NvnSdkTexBuilder *b) { return run(c, SDKA_nvnTextureBuilderGetStorageAlignment, b, 0); }
uint64_t nvn_sdk_texture_storage_size(Ctx *c, const NvnSdkTexBuilder *b) { return run(c, SDKA_nvnTextureBuilderGetStorageSize, b, 0); }
uint32_t nvn_sdk_texture_storage_class(Ctx *c, const NvnSdkTexBuilder *b) { return (uint32_t)run(c, SDKA_nvnTextureBuilderGetStorageClass, b, 0); }
uint32_t nvn_sdk_texture_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b) { return (uint32_t)run(c, SDKA_nvnTextureBuilderGetRawStorageClass, b, 0); }
void nvn_sdk_texture_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]) { run(c, SDKA_nvnTextureBuilderGetSparseTileLayout, b, P(out)); }

/* The NVNtexture nvnTextureInitialize builds from a builder without a GL texture name (0x80 bytes zeroed first): a packed
 * record, not a copy of the builder as in NVN 3.6.0.
 *   +8 device, +0x10 pool,
 *   +0x18 u64  offset (48 bits) | levels << 48 | samples << 56
 *   +0x20 u64  width | height (16 bits) << 32 | depth (16 bits) << 48
 *   +0x28 u48  flags (16 bits) | format (9) << 16 | target (7) << 25 | swizzle 4 x 3 bits << 32 | depth-stencil mode (2) << 44
 *              | (format record +0xe != 0) << 46 | (format record +0xf != 0) << 47
 *   +0x48      the layout block, written by the layout function (texture + 0x48, builder): kind = byte +0x59, storage size
 *              = u32 +0x5a
 *   +0x60 GPU address, +0x78 u16 0xffff (a descriptor slot only for format 0x34): pool results, not part of the storage rules.
 * The pool check (pool byte +0x11 bit 0) is a validity check of the caller's pool. A builder with a GL texture name takes the
 * interop path: not modelled. */
static int real_texture(Ctx *c, uint8_t *rt, uint8_t *rb, const NvnSdkTexBuilder *b) {
    if (b->glname) return 0;
    model();
    real_builder(rb, b);
    memset(rt, 0, 0xc0);
    const uint8_t *fr = fmt_rec(b->format);
    *(uint64_t *)(rt + 8) = P(g_dev);
    *(uint64_t *)(rt + 0x18) = (uint64_t)(b->levels & 0xff) << 48 | (uint64_t)(b->samples & 0xff) << 56;
    *(uint64_t *)(rt + 0x20) = b->width | (uint64_t)(b->height & 0xffff) << 32 | (uint64_t)(b->depth & 0xffff) << 48;
    uint64_t h = (b->flags & 0xffffu) | (uint64_t)(b->format & 0x1ff) << 16 | (uint64_t)(b->target & 0x7f) << 25 |
                 (uint64_t)(fr[0xe] != 0) << 46 | (uint64_t)(fr[0xf] != 0) << 47;
    memcpy(rt + 0x28, &h, 6);
    c->x[0] = P(rt + 0x48); c->x[1] = P(rb);
    sdk_dispatch(c, SDK(SDKA_texture_layout));
    return 1;
}
static uint64_t run_tex(Ctx *c, uint64_t fn, const NvnSdkTexBuilder *b, uint64_t x1, int *ok) {
    uint8_t rb[0x80], rt[0xc0];
    *ok = real_texture(c, rt, rb, b);
    if (!*ok) return 0;
    c->x[0] = P(rt); c->x[1] = x1;
    sdk_dispatch(c, SDK(fn));
    return c->x[0];
}
int nvn_sdk_tex_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { int ok; *out = (uint32_t)run_tex(c, SDKA_nvnTextureGetStorageClass, b, 0, &ok); return ok; }
int nvn_sdk_tex_raw_storage_class(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { int ok; *out = (uint32_t)run_tex(c, SDKA_nvnTextureGetRawStorageClass, b, 0, &ok); return ok; }
int nvn_sdk_tex_sparse_layout(Ctx *c, const NvnSdkTexBuilder *b, uint32_t out[4]) { int ok; run_tex(c, SDKA_nvnTextureGetSparseTileLayout, b, P(out), &ok); return ok; }
int nvn_sdk_tex_storage_size(Ctx *c, const NvnSdkTexBuilder *b, uint32_t *out) { int ok; *out = (uint32_t)run_tex(c, SDKA_nvnTextureGetStorageSize, b, 0, &ok); return ok; }

/* Format table: bit 0 of the first word = "this format exists" (nvnTextureBuilderGetStorageSize). */
int nvn_sdk_format_known(uint32_t format) {
    model();
    return format < SDKA_format_count && (fmt_rec(format)[0] & 1);
}

/* Self-test (WT_SDK_SELFTEST): answers read in our sdk's disassembly, plus a sweep that must not stop anywhere
 * (every method the choosers reach must have been lifted). WT_NVN_STORAGE_TABLE=<file> writes the sweep. */
int nvn_sdk_selftest(Ctx *c) {
    int bad = 0, n = 0;
    NvnSdkTexBuilder b;
    const char *out = getenv("WT_NVN_STORAGE_TABLE");
    FILE *f = out && *out ? fopen(out, "w") : NULL;
#define SET(fl, tg, w_, h_, d_, lv, fm, sm) do { memset(&b, 0, sizeof b); b.flags = (fl); b.target = (tg); b.width = (w_); b.height = (h_); b.depth = (d_); b.levels = (lv); b.format = (fm); b.samples = (sm); } while (0)
#define WANT(what, got, want) do { uint64_t g_ = (got), w_ = (want); n++; if (g_ != w_) { bad++; printf("  FAIL nvn storage %s: 0x%llx, expected 0x%llx\n", what, (unsigned long long)g_, (unsigned long long)w_); } } while (0)
    SET(0x40, 1, 64, 64, 1, 1, 0x25, 0); WANT("alignment, flag bit 6", nvn_sdk_texture_storage_alignment(c, &b), 0x80);
    SET(0x10, 1, 64, 64, 1, 1, 0x25, 0); WANT("alignment, flag bit 4", nvn_sdk_texture_storage_alignment(c, &b), 0x20);
    b.stride = 256; WANT("size, flag bit 4 = stride x height", nvn_sdk_texture_storage_size(c, &b), 256 * 64);
    for (uint32_t fm = 1; fm < SDKA_format_count; fm++) {
        if (!nvn_sdk_format_known(fm)) continue;
        uint32_t bpe = fmt_rec(fm)[0x10];
        SET(0, 10, 100, 1, 1, 1, fm, 0);
        WANT("alignment, buffer target = bytes per element, 12 counts as 4", nvn_sdk_texture_storage_alignment(c, &b), bpe == 12 ? 4 : bpe);
        WANT("size, buffer target = width x bytes per element", nvn_sdk_texture_storage_size(c, &b), 100ull * bpe);
        static const uint32_t flags[] = {0, 8, 0x100, 0x108, 0x1008};
        for (int tg = 0; tg < 10; tg++) for (int fi = 0; fi < 5; fi++) for (uint32_t sm = 0; sm <= 4; sm += 2) {
            if ((tg == 5 || tg == 6) != (sm != 0)) continue;
            SET(flags[fi], (uint32_t)tg, 128, tg == 0 || tg == 3 ? 1 : 64, tg == 8 ? 1 : tg == 9 ? 6 : tg == 2 || tg == 4 || tg == 6 || tg == 3 ? 4 : 1, 1, fm, sm);
            uint64_t al = nvn_sdk_texture_storage_alignment(c, &b), sz = nvn_sdk_texture_storage_size(c, &b);
            uint32_t cl = nvn_sdk_texture_storage_class(c, &b);
            n++;
            if (al != 0x200 && al != 0x10000) { bad++; printf("  FAIL nvn storage alignment 0x%llx for format 0x%x target %d flags 0x%x: neither 0x200 nor the page size\n", (unsigned long long)al, fm, tg, flags[fi]); }
            if (al == 0x10000 && (sz & 0xffff)) { bad++; printf("  FAIL nvn storage size 0x%llx not a multiple of the page size for format 0x%x target %d\n", (unsigned long long)sz, fm, tg); }
            if (f) fprintf(f, "%x\t%d\t%x\t%u\t%llx\t%llx\t%x\n", fm, tg, flags[fi], sm, (unsigned long long)al, (unsigned long long)sz, cl);
            uint32_t tsz = 0, tcl = 0;
            n++;
            if (!nvn_sdk_tex_storage_size(c, &b, &tsz) || (sz && tsz != (uint32_t)sz)) { bad++; printf("  FAIL nvn texture storage size: format 0x%x target %d flags 0x%x: texture 0x%x, builder 0x%llx\n", fm, tg, flags[fi], tsz, (unsigned long long)sz); }
            n++;
            if (!nvn_sdk_tex_storage_class(c, &b, &tcl) || tcl != cl) { bad++; printf("  FAIL nvn texture storage class: format 0x%x target %d flags 0x%x: texture 0x%x, builder 0x%x\n", fm, tg, flags[fi], tcl, cl); }
        }
    }
    if (f) fclose(f);
    {
        uint32_t v, sp[4];
        SET(0x10, 1, 64, 64, 1, 1, 0x25, 0); b.stride = 256; v = 0;
        WANT("texture class, flag bit 4: 0x100 | linear kind 0", (nvn_sdk_tex_storage_class(c, &b, &v), v), 0x100);
        WANT("builder class, flag bits 4/6: 0x100 | device +0x1b30", nvn_sdk_texture_storage_class(c, &b), 0x100);
        WANT("builder raw class, flag bits 4/6: 1", nvn_sdk_texture_raw_storage_class(c, &b), 1);
        SET(0, 10, 100, 1, 1, 1, 0x25, 0);
        WANT("builder class, buffer target: 0x100 | device +0x1b30", nvn_sdk_texture_storage_class(c, &b), 0x100);
        SET(0, 1, 256, 256, 1, 1, 0x25, 0); sp[0] = sp[1] = sp[2] = sp[3] = 0xdeadbeef;
        nvn_sdk_texture_sparse_layout(c, &b, sp);
        WANT("sparse layout without the sparse flag: zero words 0,1", (uint64_t)sp[0] | sp[1], 0);
        WANT("sparse layout without the sparse flag: zero words 2,3", (uint64_t)sp[2] | sp[3], 0);
        b.glname = 7; v = 0x55;
        WANT("texture made from an OpenGL name is not modelled", nvn_sdk_tex_storage_class(c, &b, &v), 0);
    }
    printf("  nvn_sdk: %d texture storage cases, %d failed\n", n, bad);
    bad += nvn_c4_selftest();
    bad += nvn_c5_selftest(c);
    return bad;
}
