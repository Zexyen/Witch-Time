/* [witchtime] issues 118 / 119: AMD FSR 3.1.4 temporal upscaling as native anti-aliasing (render size = output size).
 * Included at the end of nvn_vk.c (after nvn_vk_draw.c): one translation unit with the Vulkan back end.
 *
 * The engine has no motion vectors it draws (its velocity passes are loaded but never bound: docs/STATUS.md "FSR 3.1"),
 * so the inputs are made here:
 *  - jitter: game_shim_eh.c h_cam_upload shifts the main camera's GPU copy (c3[16..47]) by a Halton(2,3) step of
 *    game_fsr_w x game_fsr_h pixels; the shift is read back from that c3 here, so the render thread never guesses a frame;
 *  - camera motion vectors: one compute pass at the scene's size, from its depth (0..1, not reversed) and this frame's
 *    and the last frame's view-projection without jitter (c3[32..47] of the scene's draws). Things that move on their own
 *    (characters, effects) get the camera's motion only.
 * FSR runs once a frame on the scene's HDR colour (B, R11G11B10F) before the first bloom downsample (or the tonemap if
 * that comes first), and its output is copied back into B: bloom, exposure, the tonemap, FXAA and the HUD follow as before.
 * The passes are AMD's GLSL (third_party/fsr3, MIT, FidelityFX SDK v1.1.4), compiled by our glslang; the host below
 * follows AMD's ffx_fsr3upscaler.cpp (fsr3upscalerDispatch) for constants, resources and the order of the passes. */

#include "mv_match.h"                              /* motion vectors, step 2: last frame's object constants */
__declspec(dllimport) extern volatile int game_fsr_w, game_fsr_h, game_fsr_phases;      /* game_shim_eh.c h_cam_upload: the jitter's pixel size (0 = none) and its phase count */

/* AMD's Fsr3UpscalerConstants (ffx_fsr3upscaler_private.h), the std140 block cbFSR3Upscaler */
typedef struct {
    int32_t renderSize[2], previousFrameRenderSize[2], upscaleSize[2], previousFrameUpscaleSize[2], maxRenderSize[2], maxUpscaleSize[2];
    float deviceToViewDepth[4], jitterOffset[2], previousFrameJitterOffset[2], motionVectorScale[2], downscaleFactor[2], motionVectorJitterCancellation[2];
    float tanHalfFOV, jitterPhaseCount, deltaTime, deltaPreExposure, viewSpaceToMetersFactor, frameIndex;
    float velocityFactor, reactivenessScale, shadingChangeScale, accumulationAddedPerFrame, minDisocclusionAccumulation;
} FsrConst;

/* the GLSL, embedded; #include "x/y.h" is looked up by its file name (AMD's names are unique) */
static const char k_fsr_f0[] = {
#embed "third_party/fsr3/ffx_common_types.h"
, 0};
static const char k_fsr_f1[] = {
#embed "third_party/fsr3/ffx_core.h"
, 0};
static const char k_fsr_f2[] = {
#embed "third_party/fsr3/ffx_core_glsl.h"
, 0};
static const char k_fsr_f3[] = {
#embed "third_party/fsr3/ffx_core_gpu_common.h"
, 0};
static const char k_fsr_f4[] = {
#embed "third_party/fsr3/ffx_core_gpu_common_half.h"
, 0};
static const char k_fsr_f5[] = {
#embed "third_party/fsr3/ffx_core_portability.h"
, 0};
static const char k_fsr_f6[] = {
#embed "third_party/fsr3/ffx_spd.h"
, 0};
static const char k_fsr_f7[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_callbacks_glsl.h"
, 0};
static const char k_fsr_f8[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_resources.h"
, 0};
static const char k_fsr_f9[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_common.h"
, 0};
static const char k_fsr_f10[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_prepare_inputs.h"
, 0};
static const char k_fsr_f11[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_luma_pyramid.h"
, 0};
static const char k_fsr_f12[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_shading_change_pyramid.h"
, 0};
static const char k_fsr_f13[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_shading_change.h"
, 0};
static const char k_fsr_f14[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_prepare_reactivity.h"
, 0};
static const char k_fsr_f15[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_luma_instability.h"
, 0};
static const char k_fsr_f16[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_sample.h"
, 0};
static const char k_fsr_f17[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_upsample.h"
, 0};
static const char k_fsr_f18[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_reproject.h"
, 0};
static const char k_fsr_f19[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_accumulate.h"
, 0};
static const char k_fsr_f20[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_prepare_inputs_pass.glsl"
, 0};
static const char k_fsr_f21[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_luma_pyramid_pass.glsl"
, 0};
static const char k_fsr_f22[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_shading_change_pyramid_pass.glsl"
, 0};
static const char k_fsr_f23[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_shading_change_pass.glsl"
, 0};
static const char k_fsr_f24[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_prepare_reactivity_pass.glsl"
, 0};
static const char k_fsr_f25[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_luma_instability_pass.glsl"
, 0};
static const char k_fsr_f26[] = {
#embed "third_party/fsr3/ffx_fsr3upscaler_accumulate_pass.glsl"
, 0};
static const struct { const char *name, *text; } k_fsr_files[] = {
    {"ffx_common_types.h", k_fsr_f0}, {"ffx_core.h", k_fsr_f1}, {"ffx_core_glsl.h", k_fsr_f2}, {"ffx_core_gpu_common.h", k_fsr_f3},
    {"ffx_core_gpu_common_half.h", k_fsr_f4}, {"ffx_core_portability.h", k_fsr_f5}, {"ffx_spd.h", k_fsr_f6},
    {"ffx_fsr3upscaler_callbacks_glsl.h", k_fsr_f7}, {"ffx_fsr3upscaler_resources.h", k_fsr_f8}, {"ffx_fsr3upscaler_common.h", k_fsr_f9},
    {"ffx_fsr3upscaler_prepare_inputs.h", k_fsr_f10}, {"ffx_fsr3upscaler_luma_pyramid.h", k_fsr_f11}, {"ffx_fsr3upscaler_shading_change_pyramid.h", k_fsr_f12},
    {"ffx_fsr3upscaler_shading_change.h", k_fsr_f13}, {"ffx_fsr3upscaler_prepare_reactivity.h", k_fsr_f14}, {"ffx_fsr3upscaler_luma_instability.h", k_fsr_f15},
    {"ffx_fsr3upscaler_sample.h", k_fsr_f16}, {"ffx_fsr3upscaler_upsample.h", k_fsr_f17}, {"ffx_fsr3upscaler_reproject.h", k_fsr_f18},
    {"ffx_fsr3upscaler_accumulate.h", k_fsr_f19}, {"ffx_fsr3upscaler_prepare_inputs_pass.glsl", k_fsr_f20}, {"ffx_fsr3upscaler_luma_pyramid_pass.glsl", k_fsr_f21},
    {"ffx_fsr3upscaler_shading_change_pyramid_pass.glsl", k_fsr_f22}, {"ffx_fsr3upscaler_shading_change_pass.glsl", k_fsr_f23},
    {"ffx_fsr3upscaler_prepare_reactivity_pass.glsl", k_fsr_f24}, {"ffx_fsr3upscaler_luma_instability_pass.glsl", k_fsr_f25},
    {"ffx_fsr3upscaler_accumulate_pass.glsl", k_fsr_f26},
};
/* AMD's permutation: HDR colour, motion vectors at render size without jitter, depth 0 near / 1 far, no sharpening, fp32,
 * Lanczos computed (no LUT). The first five are AMD's base arguments (CMakeCompileFSR3UpscalerShaders.txt). */
static const char k_fsr_defs[] =
    "#define FFX_GPU 1\n#define FFX_GLSL 1\n#define FFX_HALF 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF 0\n#define FFX_FSR3UPSCALER_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF 1\n#define FFX_FSR3UPSCALER_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_UPSAMPLE_USE_LANCZOS_TYPE 2\n#define FFX_FSR3UPSCALER_OPTION_REPROJECT_USE_LANCZOS_TYPE 0\n"
    "#define FFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT 1\n#define FFX_FSR3UPSCALER_OPTION_LOW_RESOLUTION_MOTION_VECTORS 1\n"
    "#define FFX_FSR3UPSCALER_OPTION_JITTERED_MOTION_VECTORS 0\n#define FFX_FSR3UPSCALER_OPTION_INVERTED_DEPTH 0\n#define FFX_FSR3UPSCALER_OPTION_APPLY_SHARPENING 0\n";
/* our pass: camera motion vectors in UV (to the previous position, without jitter) from depth; M = previous VP x inverse
 * of this frame's VP; sj = (width, height, NDC jitter x, y). NDC y is up, row 0 at y = +1 (nvn_vk_draw.c set_dynamic_state). */
static const char k_fsr_mv[] =
    "#version 450\n#extension GL_EXT_samplerless_texture_functions : require\nlayout(local_size_x = 8, local_size_y = 8) in;\n"
    "layout(set = 0, binding = 0) uniform texture2D depth_tex;\nlayout(set = 0, binding = 1, rg16f) writeonly uniform image2D mv_out;\n"
    "layout(set = 0, binding = 2, std140) uniform U { mat4 m; vec4 sj; } u;\n"
    "void main() {\n  ivec2 p = ivec2(gl_GlobalInvocationID.xy);\n  if (p.x >= int(u.sj.x) || p.y >= int(u.sj.y)) return;\n"
    "  vec2 uv = (vec2(p) + 0.5) / u.sj.xy;\n  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - u.sj.zw;\n"
    "  vec4 q = u.m * vec4(ndc, texelFetch(depth_tex, p, 0).r, 1.0);\n  vec2 pn = q.xy / q.w;\n"
    "  imageStore(mv_out, p, vec4((pn.x - ndc.x) * 0.5, (ndc.y - pn.y) * 0.5, 0.0, 0.0));\n}\n";

static const char *fsr_file(const char *name) {
    for (size_t i = 0; i < sizeof k_fsr_files / sizeof k_fsr_files[0]; i++) if (!strcmp(k_fsr_files[i].name, name)) return k_fsr_files[i].text;
    return NULL;
}
typedef struct { char *p; size_t n, cap; } FsrStr;
static void fsr_put(FsrStr *s, const char *t, size_t n) {
    if (s->n + n + 1 > s->cap) { s->cap = (s->n + n + 1) * 2; s->p = realloc(s->p, s->cap); }
    memcpy(s->p + s->n, t, n); s->n += n; s->p[s->n] = 0;
}
/* a file with its includes in place; a file not embedded is left out (ffx_core_cpu.h / _hlsl.h sit in branches that are off) */
static void fsr_emit(FsrStr *s, const char *name, int top) {
    const char *t = fsr_file(name);
    if (!t) return;
    while (*t) {
        const char *e = strchr(t, '\n');
        size_t n = e ? (size_t)(e - t) + 1 : strlen(t);
        const char *a = t;
        while (a < t + n && (*a == ' ' || *a == '\t')) a++;
        if (!strncmp(a, "#include", 8)) {
            const char *q = memchr(a, '"', (size_t)(t + n - a)), *q2 = q ? memchr(q + 1, '"', (size_t)(t + n - q - 1)) : NULL;
            if (q2) { const char *b = q2; while (b > q + 1 && b[-1] != '/') b--; char inc[96]; snprintf(inc, sizeof inc, "%.*s", (int)(q2 - b), b); fsr_emit(s, inc, 0); }
        } else if (strncmp(a, "#extension GL_GOOGLE_include_directive", 38)) {
            fsr_put(s, t, n);
            if (top && !strncmp(a, "#version", 8)) { fsr_put(s, k_fsr_defs, sizeof k_fsr_defs - 1); top = 0; }
        }
        t += n;
    }
}

/* ---- resources (AMD's internal and shared resources, plus our motion vectors and output) */
enum { FI_ACC1, FI_ACC2, FI_LUMA1, FI_LUMA2, FI_FP16X1, FI_SHADING, FI_LOCKS, FI_UP1, FI_UP2, FI_SPD, FI_FDMIP1, FI_LH1, FI_LH2, FI_ATOMIC,
       FI_REACT, FI_LUT, FI_DEFR, FI_INFO, FI_DDEPTH, FI_DMV, FI_PREVD, FI_MV, FI_OUT, FI_N };
static const struct { VkFormat f; uint8_t size; } k_fi[FI_N] = {      /* size: 0 the scene's (render), 1 half of it, 2 one texel, 3 the 128 x 1 LUT, 4 the output's (AMD's maxUpscaleSize) */
    {VK_FORMAT_R8_UNORM, 0}, {VK_FORMAT_R8_UNORM, 0}, {VK_FORMAT_R16_SFLOAT, 0}, {VK_FORMAT_R16_SFLOAT, 0}, {VK_FORMAT_R16_SFLOAT, 0},
    {VK_FORMAT_R8_UNORM, 1}, {VK_FORMAT_R8_UNORM, 4}, {VK_FORMAT_R16G16B16A16_SFLOAT, 4}, {VK_FORMAT_R16G16B16A16_SFLOAT, 4},
    {VK_FORMAT_R16G16_SFLOAT, 1}, {VK_FORMAT_R16_SFLOAT, 1}, {VK_FORMAT_R16G16B16A16_SFLOAT, 0}, {VK_FORMAT_R16G16B16A16_SFLOAT, 0},
    {VK_FORMAT_R32_UINT, 2}, {VK_FORMAT_R8G8B8A8_UNORM, 0}, {VK_FORMAT_R16_SNORM, 3}, {VK_FORMAT_R8_UNORM, 2}, {VK_FORMAT_R32G32B32A32_SFLOAT, 2},
    {VK_FORMAT_R32_SFLOAT, 0}, {VK_FORMAT_R16G16_SFLOAT, 0}, {VK_FORMAT_R32_UINT, 0}, {VK_FORMAT_R16G16_SFLOAT, 0}, {VK_FORMAT_UNDEFINED, 4},
};
typedef struct { VTex *vt; VkImageView all, lvl[6]; } FImg;

/* ---- passes: bindings as AMD's pass files define them (checked against the SPIR-V) */
enum { K_SRV, K_UAV, K_CB };
enum { R_COLOR = FI_N, R_DEPTH, R_ACC_S, R_ACC_U, R_UP_S, R_UP_U, R_LH_S, R_LH_U, R_LUMA_C, R_LUMA_P, R_SPD0, R_CB_FSR = R_SPD0 + 6, R_CB_SPD, R_CB_MV };
enum { P_MV, P_PREP, P_LUMA, P_SCPYR, P_SC, P_REACT, P_INST, P_ACC, P_N };
typedef struct { const char *file; uint8_t n; struct { uint8_t b, k, r; } b[15]; } FPass;
#define S_(b, r) {b, K_SRV, r}
#define U_(b, r) {b, K_UAV, r}
#define C_(b, r) {b, K_CB, r}
static const FPass k_fpass[P_N] = {
    {NULL, 3, {S_(0, R_DEPTH), U_(1, FI_MV), C_(2, R_CB_MV)}},
    {"ffx_fsr3upscaler_prepare_inputs_pass.glsl", 9, {S_(0, FI_MV), S_(1, R_DEPTH), S_(2, R_COLOR), U_(3, FI_DMV), U_(4, FI_DDEPTH), U_(5, FI_PREVD), U_(6, FI_FP16X1), U_(7, R_LUMA_C), C_(8, R_CB_FSR)}},
    {"ffx_fsr3upscaler_luma_pyramid_pass.glsl", 13, {S_(0, R_LUMA_C), S_(1, FI_FP16X1), U_(2, FI_ATOMIC), U_(3, FI_INFO), U_(4, R_SPD0), U_(5, R_SPD0 + 1), U_(6, R_SPD0 + 2), U_(7, R_SPD0 + 3),
                                                    U_(8, R_SPD0 + 4), U_(9, R_SPD0 + 5), U_(10, FI_FDMIP1), C_(11, R_CB_FSR), C_(12, R_CB_SPD)}},
    {"ffx_fsr3upscaler_shading_change_pyramid_pass.glsl", 13, {S_(0, R_LUMA_C), S_(1, R_LUMA_P), S_(2, FI_DMV), S_(3, FI_INFO), U_(4, FI_ATOMIC), U_(5, R_SPD0), U_(6, R_SPD0 + 1), U_(7, R_SPD0 + 2),
                                                              U_(8, R_SPD0 + 3), U_(9, R_SPD0 + 4), U_(10, R_SPD0 + 5), C_(11, R_CB_FSR), C_(12, R_CB_SPD)}},
    {"ffx_fsr3upscaler_shading_change_pass.glsl", 3, {S_(0, FI_SPD), U_(1, FI_SHADING), C_(2, R_CB_FSR)}},
    {"ffx_fsr3upscaler_prepare_reactivity_pass.glsl", 13, {S_(0, FI_PREVD), S_(1, FI_DMV), S_(2, FI_DDEPTH), S_(3, FI_DEFR), S_(4, FI_DEFR), S_(5, R_ACC_S), S_(6, FI_SHADING), S_(7, R_LUMA_C),
                                                          S_(8, FI_INFO), U_(9, FI_REACT), U_(10, FI_LOCKS), U_(11, R_ACC_U), C_(12, R_CB_FSR)}},
    {"ffx_fsr3upscaler_luma_instability_pass.glsl", 10, {S_(0, FI_INFO), S_(1, FI_REACT), S_(2, FI_DMV), S_(3, FI_INFO), S_(4, R_LH_S), S_(5, FI_FDMIP1), S_(6, R_LUMA_C), U_(7, R_LH_U),
                                                        U_(8, FI_FP16X1), C_(9, R_CB_FSR)}},
    {"ffx_fsr3upscaler_accumulate_pass.glsl", 13, {S_(0, FI_INFO), S_(1, FI_REACT), S_(2, FI_DMV), S_(3, R_UP_S), S_(4, FI_LUT), S_(5, FI_FDMIP1), S_(6, R_LUMA_C), S_(7, FI_FP16X1),
                                                  S_(8, R_COLOR), U_(9, R_UP_U), U_(10, FI_OUT), U_(11, FI_LOCKS), C_(12, R_CB_FSR)}},
};
#undef S_
#undef U_
#undef C_

static struct {
    int ok;                                        /* 0 not tried, 1 ready, -1 not available here (logged once) */
    VkPipeline pipe[P_N]; VkPipelineLayout pl[P_N]; VkDescriptorSetLayout dl[P_N]; VkSampler smp[2];
    VkBuffer ubo; VMem ubo_mem; uint32_t ubo_pos, ubo_al, cb_off[3];
    int w, h, uw, uh, blit; FImg img[FI_N];        /* w, h: the scene's (render) size; uw, uh: the output's; blit: the output is RGBA16F (no storage for the scene's format) */
    Tex *col; LONG64 up_frame, missed;             /* col: the scene colour of the last run; up_frame: FI_OUT filled from it without FSR (fsr_up_view) */
    FsrConst k; uint32_t res_index; int fresh;    /* fresh: resources just made: clear them, reset */
    double prev_vp[16]; float prev_fwd[3], prev_pos[3]; int have_prev;
    LONG64 frame, ran, depth_frame, cam_frame; LONG64 t_last;
    Tex *depth; float c3[64]; int cam_jit;           /* cam_jit: the c3 kept this frame carries our jitter ([1][2] != 0: Halton(k, 3) - 0.5 never is) */
    VkImageView col_view, depth_view; VkImageLayout col_layout, depth_layout;
    LONG64 runs, resets;
} F;

static int fsr_want(void) { rs_scale(); return g_rs_mode >= 1 && F.ok >= 0; }      /* the mode as latched with the scales (rs_scale) */
/* the scene's scale is the picture's / this (AMD's ratios); 1 where FSR cannot run, so the scene is never left small without it */
static const char *fsr_unable(void);
static int dlss_run(VTex *vc, VTex *vd, int w, int h, int uw, int uh, int reset); static void dlss_present(void); static void dlss_shutdown(void);      /* nvn_vk_dlss.c (issue 190) */
static float fsr_ratio(int mode) {
    static const float k[6] = {1.0f, 1.0f, 1.5f, 1.7f, 2.0f, 3.0f};
    if (mode > 5) mode -= 5;                        /* [witchtime] issue 190: dlaa .. dlss_ultra_performance (6 .. 10) draw at the scale of the FSR mode of the same rank, which runs in their place where DLSS cannot */
    if (mode < 2 || mode > 5 || F.ok < 0) return 1.0f;      /* (FSR failing after this was latched: fsr_run applies the scales again) */
    if (V.device && fsr_unable()) return 1.0f;      /* fsr_setup's checks, once the device is made */
    return k[mode];
}
static int fsr_hdr(const VTex *vt) { return vt->fmt == VK_FORMAT_B10G11R11_UFLOAT_PACK32 || vt->fmt == VK_FORMAT_R16G16B16A16_SFLOAT; }
static void fsr_off(const char *why) { if (F.ok >= 0) printf("nvn_vk_fsr: FSR is off: %s\n", why); F.ok = -1; game_fsr_w = game_fsr_h = 0; }

static VkImageView fsr_view(VTex *vt, int lvl0, int nlvl) {
    VView *n = calloc(1, sizeof *n);
    VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = vt->img; ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.format = vt->fmt;
    ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ci.subresourceRange.baseMipLevel = (uint32_t)lvl0; ci.subresourceRange.levelCount = (uint32_t)nlvl; ci.subresourceRange.layerCount = 1;
    VKC(vkCreateImageView(V.device, &ci, NULL, &n->view));
    n->vtype = -2; n->next = vt->views; vt->views = n; R.views++;      /* vt_destroy destroys it with the image */
    return n->view;
}
static void fsr_img(FImg *im, VkFormat f, int w, int h, int levels, int storage) {
    VTex *vt = calloc(1, sizeof *vt);
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    VkMemoryRequirements rq;
    vt->magic = 0x56544558; vt->fmt = f; vt->aspect = VK_IMAGE_ASPECT_COLOR_BIT; vt->w = w; vt->h = h; vt->d = 1; vt->levels = levels; vt->layers = 1; vt->samples = 1; vt->storage = storage;      /* bit 1: also a colour attachment (FI_MV: the velocity pass) */
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f; ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1; ci.mipLevels = (uint32_t)levels; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0) | (storage & 2 ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    VKC(vkCreateImage(V.device, &ci, NULL, &vt->img));
    vkGetImageMemoryRequirements(V.device, vt->img, &rq);
    vt->mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VKC(vkBindImageMemory(V.device, vt->img, vt->mem.mem, vt->mem.off));
    vt->next = g_vtex_all; if (g_vtex_all) g_vtex_all->prev = vt; g_vtex_all = vt;
    R.images_live++;
    image_barrier(vt->img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
    vt->layout = VK_IMAGE_LAYOUT_GENERAL;
    im->vt = vt; im->all = fsr_view(vt, 0, levels);
    for (int l = 0; l < 6; l++) im->lvl[l] = l < levels ? fsr_view(vt, l, 1) : im->all;
}

static const char *fsr_unable(void) {               /* what the device lacks for FSR, NULL when nothing (physical-device queries only: any thread) */
    static char m[96];
    if (!D.has_push) return "the device has no VK_KHR_push_descriptor";
    if (!D.f2.features.shaderStorageImageWriteWithoutFormat) return "the device has no shaderStorageImageWriteWithoutFormat";
    for (int i = 0; i < FI_N; i++)
        if (k_fi[i].f && i != FI_LUT && !fmt_supported(k_fi[i].f, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { snprintf(m, sizeof m, "no storage images of format %d", (int)k_fi[i].f); return m; }
    if (!fmt_supported(VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT)) return "no image atomics on R32_UINT";
    return NULL;
}
static int fsr_setup(void) {                       /* render thread, once: samplers, the uniform ring, the 8 pipelines */
    const char *why = fsr_unable();
    if (why) { fsr_off(why); return 0; }
    LONG64 t0 = tk();
    for (int i = 0; i < 2; i++) {
        VkSamplerCreateInfo si = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = i ? VK_FILTER_LINEAR : VK_FILTER_NEAREST; si.mipmapMode = i ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; si.maxLod = VK_LOD_CLAMP_NONE;
        VKC(vkCreateSampler(V.device, &si, NULL, &F.smp[i]));
    }
    F.ubo_al = (uint32_t)V.props.limits.minUniformBufferOffsetAlignment; if (F.ubo_al < 256) F.ubo_al = 256;
    F.ubo = vbuf_new(64u << 10, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &F.ubo_mem);      /* far more than the 8 submissions that may be in flight */
    for (int p = 0; p < P_N; p++) {
        VkDescriptorSetLayoutBinding lb[18];
        int n = 0;
        for (int i = 0; i < k_fpass[p].n; i++) {
            VkDescriptorSetLayoutBinding b = {k_fpass[p].b[i].b, k_fpass[p].b[i].k == K_SRV ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : k_fpass[p].b[i].k == K_UAV ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
            lb[n++] = b;
        }
        /* [issue 140] AMD's s_PointClamp / s_LinearClamp: pushed with the images (fsr_dispatch). They were immutable samplers that
         * were never pushed: in a push layout those are undefined until pushed (vkCmdPushDescriptorSetKHR), which RADV and the
         * validation layer let pass, and the suspect in an RTX 4090's lost device after about 1100 FSR frames (not proven). */
        if (p != P_MV) for (int i = 0; i < 2; i++) { VkDescriptorSetLayoutBinding b = {1000u + (uint32_t)i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}; lb[n++] = b; }
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; li.bindingCount = (uint32_t)n; li.pBindings = lb;
        VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &F.dl[p]));
        VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &F.dl[p];
        VKC(vkCreatePipelineLayout(V.device, &pli, NULL, &F.pl[p]));
        FsrStr src = {0};
        if (k_fpass[p].file) fsr_emit(&src, k_fpass[p].file, 1); else fsr_put(&src, k_fsr_mv, sizeof k_fsr_mv - 1);
        uint64_t h;
        char what[96]; snprintf(what, sizeof what, "FSR pass %s", k_fpass[p].file ? k_fpass[p].file : "camera motion vectors");
        VkShaderModule mod = src.p ? module_from_glsl(5, src.p, what, &h) : NULL;
        free(src.p);
        if (!mod) { fsr_off("a pass did not compile (the line before names it)"); return 0; }
        VkComputePipelineCreateInfo ci = {VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO; ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT; ci.stage.module = mod; ci.stage.pName = "main"; ci.layout = F.pl[p];
        VkResult r = vkCreateComputePipelines(V.device, g_pcache, 1, &ci, NULL, &F.pipe[p]);
        vkDestroyShaderModule(V.device, mod, NULL);
        if (r != VK_SUCCESS) { fsr_off("vkCreateComputePipelines failed"); return 0; }
    }
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    printf("nvn_vk_fsr: FSR 3.1.4 ready (8 compute pipelines, %.0f ms)\n", (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart);
    F.ok = 1;
    return 1;
}

static int fsr_mv_on(void); static void fsr_mv_dump(Tex *depth, VTex *vd, int w, int h);      /* motion vectors, step 3 (below) */
static void fsr_resize(int w, int h, int uw, int uh, VkFormat col) {      /* the scene's and the output's sizes: every resource made again, history reset */
    for (int i = 0; i < FI_N; i++) if (F.img[i].vt) { vt_garbage(F.img[i].vt); F.img[i].vt = NULL; }
    F.blit = !fmt_supported(col, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    for (int i = 0; i < FI_N; i++) {
        int s = k_fi[i].size, iw = s == 0 ? w : s == 4 ? uw : s == 1 ? w / 2 : s == 3 ? 128 : 1, ih = s == 0 ? h : s == 4 ? uh : s == 1 ? h / 2 : 1, lv = 1;
        if (i == FI_SPD) { int m = iw > ih ? iw : ih; while (m > 1) { m >>= 1; lv++; } }      /* AMD: the whole chain */
        fsr_img(&F.img[i], i == FI_OUT ? (F.blit ? VK_FORMAT_R16G16B16A16_SFLOAT : col) : k_fi[i].f, iw, ih, lv, i == FI_MV && fsr_mv_on() ? 3 : i != FI_LUT);      /* (size 4 also holds the locks and the history) */
    }
    {   /* AMD's Lanczos-2 table (fsr3upscalerCreate): bound by the accumulate pass */
        int16_t lut[128];
        for (int i = 0; i < 128; i++) {
            double pi = 3.14159265358979, x = 2.0 * i / 127.0, y = x < 1e-6 ? 1.0 : (sin(pi * x) / (pi * x)) * (sin(0.5 * pi * x) / (0.5 * pi * x));
            lut[i] = (int16_t)lround(y * 32767.0);
        }
        uint64_t off = ring_raw(lut, sizeof lut);
        VkBufferImageCopy c = {0};
        c.bufferOffset = off; c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent.width = 128; c.imageExtent.height = 1; c.imageExtent.depth = 1;
        cmd_begin();
        vkCmdCopyBufferToImage(R.cmd, R.ring, F.img[FI_LUT].vt->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    }
    F.w = w; F.h = h; F.uw = uw; F.uh = uh; F.fresh = 1;
    printf("nvn_vk_fsr: resources for %dx%d, output %dx%d (%s)\n", w, h, uw, uh, F.blit ? "RGBA16F, blitted" : "in the scene's format");
}

static void fsr_clear(int i, float v0, float v1, uint32_t u0) {
    VkClearColorValue c; memset(&c, 0, sizeof c);
    if (k_fi[i].f == VK_FORMAT_R32_UINT) c.uint32[0] = u0; else { c.float32[0] = v0; c.float32[1] = v1; }
    VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, 1};
    vkCmdClearColorImage(R.cmd, F.img[i].vt->img, VK_IMAGE_LAYOUT_GENERAL, &c, 1, &r);
}
static void fsr_barrier(VkPipelineStageFlags src, VkAccessFlags sa, VkPipelineStageFlags dst, VkAccessFlags da) {
    VkMemoryBarrier b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = sa; b.dstAccessMask = da;
    vkCmdPipelineBarrier(R.cmd, src, dst, 0, 1, &b, 0, NULL, 0, NULL);
}
static uint32_t fsr_ubo(const void *p, uint32_t n) {
    uint32_t a = F.ubo_al, size = 64u << 10;
    if (F.ubo_pos + n > size) F.ubo_pos = 0;
    uint32_t off = F.ubo_pos;
    memcpy((uint8_t *)F.ubo_mem.map + off, p, n);
    F.ubo_pos = (off + n + a - 1) / a * a;
    return off;
}
static int fsr_res(int r) {                        /* the image a binding names this frame (AMD: odd and even frames swap the histories) */
    int odd = (int)(F.res_index & 1);
    switch (r) {
    case R_ACC_S: return odd ? FI_ACC2 : FI_ACC1;  case R_ACC_U: return odd ? FI_ACC1 : FI_ACC2;
    case R_UP_S: return odd ? FI_UP2 : FI_UP1;     case R_UP_U: return odd ? FI_UP1 : FI_UP2;
    case R_LH_S: return odd ? FI_LH2 : FI_LH1;     case R_LH_U: return odd ? FI_LH1 : FI_LH2;
    case R_LUMA_C: return odd ? FI_LUMA2 : FI_LUMA1; case R_LUMA_P: return odd ? FI_LUMA1 : FI_LUMA2;
    default: return r;
    }
}
static void fsr_dispatch(int p, uint32_t gx, uint32_t gy) {
    VkWriteDescriptorSet w[15];
    VkDescriptorImageInfo ii[15];
    VkDescriptorBufferInfo bi[15];
    const FPass *ps = &k_fpass[p];
    for (int i = 0; i < ps->n; i++) {
        int r = ps->b[i].r;
        memset(&w[i], 0, sizeof w[i]);
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstBinding = ps->b[i].b; w[i].descriptorCount = 1;
        if (ps->b[i].k == K_CB) {
            bi[i].buffer = F.ubo; bi[i].offset = F.cb_off[r - R_CB_FSR]; bi[i].range = 256;      /* each block has 256 bytes of its own (fsr_ubo) */
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; w[i].pBufferInfo = &bi[i];
            continue;
        }
        ii[i].sampler = VK_NULL_HANDLE; ii[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        if (r == R_COLOR) { ii[i].imageView = F.col_view; ii[i].imageLayout = F.col_layout; }
        else if (r == R_DEPTH) { ii[i].imageView = F.depth_view; ii[i].imageLayout = F.depth_layout; }
        else if (r >= R_SPD0 && r < R_SPD0 + 6) ii[i].imageView = F.img[FI_SPD].lvl[r - R_SPD0];
        else { FImg *im = &F.img[fsr_res(r)]; ii[i].imageView = ps->b[i].k == K_UAV ? im->lvl[0] : im->all; }
        w[i].descriptorType = ps->b[i].k == K_SRV ? VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE; w[i].pImageInfo = &ii[i];
    }
    int n = ps->n;                                 /* (at most 13 + the 2 samplers: the arrays' 15) */
    if (p != P_MV) for (int i = 0; i < 2; i++, n++) {      /* the samplers at 1000 / 1001 (fsr_setup) */
        memset(&w[n], 0, sizeof w[n]); memset(&ii[n], 0, sizeof ii[n]);
        ii[n].sampler = F.smp[i];
        w[n].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[n].dstBinding = 1000u + (uint32_t)i; w[n].descriptorCount = 1; w[n].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER; w[n].pImageInfo = &ii[n];
    }
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.pipe[p]);
    vkCmdPushDescriptorSetKHR(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.pl[p], 0, (uint32_t)n, w);
    ckpt(3, NULL, NULL, NULL);                     /* WT_VK_CHECKPOINTS: the dump names FSR's pass (hb[0]) */
    if (g_ck_on && g_ckn) g_ck[(g_ckn - 1) % CK_N].hb[0] = (uint64_t)p;
    vkCmdDispatch(R.cmd, gx ? gx : 1, gy ? gy : 1, 1);
    ckpt(2, NULL, NULL, NULL);
    fsr_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);      /* the next frame's clears too */
}
static void fsr_inv4(const double *m, double *o) {      /* 4x4 inverse, row major (cofactors); a singular matrix gives the identity */
    double t[16];
    t[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    t[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    t[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    t[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    t[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    t[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    t[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    t[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    t[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    t[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    t[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    t[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    t[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    t[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    t[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    t[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    double det = m[0] * t[0] + m[1] * t[4] + m[2] * t[8] + m[3] * t[12];
    for (int i = 0; i < 16; i++) o[i] = det != 0.0 ? t[i] / det : (i % 5 == 0);
}

/* render thread, at a draw whose fragment stage reads `col` (the scene's colour) with `depth` the scene's depth */
static void fsr_run(Tex *col, Tex *depth, int fx) {
    VTex *vc = vt_get(col), *vd = vt_get(depth);
    int w = vc->w, h = vc->h, uw = w, uh = h;
    if (vd->w != w || vd->h != h) return;
    if (vc->rs) rs_size(col->b.width, col->b.height, 1, &uw, &uh);      /* the output: as the tonemap's target, which has the scene's game size at the picture's scale */
    if (uw < w || uh < h) uw = w, uh = h;
    if (F.ok == 0 && !fsr_setup()) { if (rs_render_scale() != rs_scale()) rs_apply(); return; }      /* a quality mode without FSR: the scene back at the picture's scale (fsr_ratio is 1 now) */
    pass_end();
    cmd_begin();
    pt_mark(w, h, 0, 0x118, 0, 0, 1);              /* WT_VK_PASSTIME: FSR (and its copy) as a kind of its own, "format 0x118" */
    fsr_barrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);      /* FI_OUT: an earlier tonemap's read, fsr_up_view's blit */
    if (w != F.w || h != F.h || uw != F.uw || uh != F.uh || !F.img[FI_OUT].vt || (!F.blit && F.img[FI_OUT].vt->fmt != vc->fmt)) fsr_resize(w, h, uw, uh, vc->fmt);
    F.col = col;
    LONG64 now = tk();
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    double dt = F.t_last ? (double)(now - F.t_last) / (double)fq.QuadPart : 1.0 / 60.0;
    F.t_last = now;

    /* the camera of the scene's draws: c3 rows (h_cam_upload): [0..15] view, [16..31] projection, [32..47] VP, [48..63] inverse view */
    const float *m = F.c3;
    double jx = m[30] != 0.0f ? (double)m[18] / m[30] : 0.0, jy = m[30] != 0.0f ? (double)m[22] / m[30] : 0.0;      /* the projection's [0][2], [1][2]: 0 but for our jitter */
    if (fabs(jx) * w > 1.15 || fabs(jy) * h > 1.15) jx = jy = 0.0;      /* an off-centre projection of the game's own, not our jitter (1.15: a camera
                                                                         jittered for the last, smaller dynamic-resolution size reaches 1.09) */
    double vp[16], inv[16], mm[16];
    for (int i = 0; i < 16; i++) vp[i] = m[32 + i];
    for (int c = 0; c < 4; c++) { vp[c] -= jx * vp[12 + c]; vp[4 + c] -= jy * vp[12 + c]; }      /* without the jitter: row 0 / 1 -= j * row 3 */
    float fwd[3] = {-m[8], -m[9], -m[10]}, pos[3] = {m[51], m[55], m[59]};
    int cut = 0;
    if (F.have_prev) {
        float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]), pl = sqrtf(F.prev_fwd[0] * F.prev_fwd[0] + F.prev_fwd[1] * F.prev_fwd[1] + F.prev_fwd[2] * F.prev_fwd[2]);
        float dot = fl > 0 && pl > 0 ? (fwd[0] * F.prev_fwd[0] + fwd[1] * F.prev_fwd[1] + fwd[2] * F.prev_fwd[2]) / (fl * pl) : 1.0f;
        float dx = pos[0] - F.prev_pos[0], dy = pos[1] - F.prev_pos[1], dz = pos[2] - F.prev_pos[2];
        cut = dot < 0.9f || dx * dx + dy * dy + dz * dz > 25.0f;      /* shortcut: a camera cut is guessed (25 degrees or 5 units in a frame); a game signal would be exact */
    }
    int reset = F.fresh || !F.have_prev || F.ran != F.frame - 1 || cut;
    F.ran = F.frame;
    {   /* our motion vectors: M = previous VP x inverse(VP) (both without jitter), uploaded column major */
        float u[20];
        if (reset) { for (int i = 0; i < 16; i++) mm[i] = i % 5 == 0; }
        else { fsr_inv4(vp, inv); for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { double s = 0; for (int k = 0; k < 4; k++) s += F.prev_vp[r * 4 + k] * inv[k * 4 + c]; mm[r * 4 + c] = s; } }
        for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) u[c * 4 + r] = (float)mm[r * 4 + c];
        u[16] = (float)w; u[17] = (float)h; u[18] = (float)jx; u[19] = (float)jy;
        F.cb_off[2] = fsr_ubo(u, sizeof u);
    }
    memcpy(F.prev_vp, vp, sizeof vp); memcpy(F.prev_fwd, fwd, sizeof fwd); memcpy(F.prev_pos, pos, sizeof pos); F.have_prev = 1;

    {   /* AMD's constants (fsr3upscalerDispatch, setupDeviceDepthToViewSpaceDepthParams) */
        FsrConst *k = &F.k;
        double p11 = m[21], a = m[26], b = m[27], fovy = p11 > 0 ? 2.0 * atan(1.0 / p11) : 1.0;
        double zn = a != 0 ? b / a : 0.1, zf = a + 1.0 != 0 ? b / (a + 1.0) : 5000.0;      /* [2][2] = -F/(F-N), [2][3] = -NF/(F-N) */
        if (!(zn > 0) || !(zf > zn)) { zn = 0.1; zf = 5000.0; }
        double t = tan(0.5 * fovy), asp = (double)w / h, q = zf / (zn - zf);
        k->previousFrameRenderSize[0] = k->renderSize[0]; k->previousFrameRenderSize[1] = k->renderSize[1];
        k->previousFrameUpscaleSize[0] = k->upscaleSize[0]; k->previousFrameUpscaleSize[1] = k->upscaleSize[1];
        k->renderSize[0] = k->maxRenderSize[0] = w; k->renderSize[1] = k->maxRenderSize[1] = h;
        k->upscaleSize[0] = k->maxUpscaleSize[0] = uw; k->upscaleSize[1] = k->maxUpscaleSize[1] = uh;
        k->deviceToViewDepth[0] = (float)(-q); k->deviceToViewDepth[1] = (float)(q * zn); k->deviceToViewDepth[2] = (float)(t * asp); k->deviceToViewDepth[3] = (float)t;
        k->previousFrameJitterOffset[0] = k->jitterOffset[0]; k->previousFrameJitterOffset[1] = k->jitterOffset[1];
        k->jitterOffset[0] = (float)(jx * w * 0.5); k->jitterOffset[1] = (float)(-jy * h * 0.5);      /* pixels, y down (AMD: projection += (2x/w, -2y/h)) */
        k->motionVectorScale[0] = k->motionVectorScale[1] = 1.0f;      /* ours are in UV already */
        k->downscaleFactor[0] = (float)w / uw; k->downscaleFactor[1] = (float)h / uh;
        k->motionVectorJitterCancellation[0] = k->motionVectorJitterCancellation[1] = 0.0f;
        k->tanHalfFOV = (float)(t * asp);
        {   double r = (double)uw / w;          /* ffxFsr3UpscalerGetJitterPhaseCount, rounded (our sizes are rounded): 8 at 1:1, 18 at 1.5, 32 at 2 */
            int ph = (int)(8.0 * r * r + 0.5);
            k->jitterPhaseCount = (float)ph; game_fsr_phases = ph; }
        k->deltaTime = (float)(dt < 0 ? 0 : dt > 1 ? 1 : dt);
        k->deltaPreExposure = 1.0f; k->viewSpaceToMetersFactor = 1.0f;
        k->frameIndex = reset ? 0.0f : k->frameIndex + 1.0f;
        k->velocityFactor = 1.0f; k->reactivenessScale = 1.0f; k->shadingChangeScale = 1.0f; k->accumulationAddedPerFrame = 1.0f / 3.0f; k->minDisocclusionAccumulation = -1.0f / 3.0f;
        F.cb_off[0] = fsr_ubo(k, sizeof *k);
        uint32_t spd[6] = {0, 0, 0, 0, (uint32_t)w, (uint32_t)h};      /* ffxSpdSetup over the whole render size: mips, work groups, offset 0, size */
        uint32_t gx = (uint32_t)(w - 1) / 64 + 1, gy = (uint32_t)(h - 1) / 64 + 1, mx = (uint32_t)(w > h ? w : h), mips = 0;
        while (mx >>= 1) mips++;
        spd[0] = mips > 12 ? 12 : mips; spd[1] = gx * gy;
        F.cb_off[1] = fsr_ubo(spd, sizeof spd);
    }

    /* inputs: the scene's colour and depth as sampled images */
    {   EView e;
        eview_of(col, vc, NULL, &e); e.lvl0 = 0; e.nlvl = 1; e.dsmode = 0; e.swz[0] = 2; e.swz[1] = 3; e.swz[2] = 4; e.swz[3] = 5;
        F.col_view = vt_view(vc, &e, DIM_2D, 0);
        eview_of(depth, vd, NULL, &e); e.lvl0 = 0; e.nlvl = 1; e.dsmode = 0; e.swz[0] = 2; e.swz[1] = 3; e.swz[2] = 4; e.swz[3] = 5;
        F.depth_view = vt_view(vd, &e, DIM_2D, 0);
        vt_layout(vc, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); vt_layout(vd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        F.col_layout = vc->layout; F.depth_layout = vd->layout;
    }
    /* clears (AMD: on the first run, on a reset, and the three of every frame) */
    if (F.fresh) { for (int i = 0; i < FI_N; i++) if (i != FI_LUT) fsr_clear(i, 0, 0, 0); fsr_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT); }
    if (reset) { fsr_clear(fsr_res(R_ACC_S), 0, 0, 0); fsr_clear(FI_INFO, -1.0f, 1.0f, 0); F.resets++; }
    fsr_clear(FI_PREVD, 0, 0, 0x3f800000u);         /* 1.0f as bits: the farthest depth, before the atomic minimum (depth not inverted) */
    fsr_clear(FI_ATOMIC, 0, 0, 0);
    fsr_clear(FI_SPD, 0, 0, 0);
    F.fresh = 0;
    fsr_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);

    uint32_t src_x = (uint32_t)(w + 7) / 8, src_y = (uint32_t)(h + 7) / 8, sc_x = (uint32_t)(w / 2 + 7) / 8, sc_y = (uint32_t)(h / 2 + 7) / 8;
    uint32_t spd_x = (uint32_t)(w - 1) / 64 + 1, spd_y = (uint32_t)(h - 1) / 64 + 1;
    fsr_dispatch(P_MV, src_x, src_y);
    if (!reset) fsr_mv_dump(depth, vd, w, h);      /* motion vectors, step 3: the objects' own over the camera's (fsr_mv_pass) */
    if (g_rs_mode < 6 || !dlss_run(vc, vd, w, h, uw, uh, reset)) {      /* [witchtime] issue 190: DLSS writes FI_OUT from the same inputs (nvn_vk_dlss.c) */
    fsr_dispatch(P_PREP, src_x, src_y);
    fsr_dispatch(P_LUMA, spd_x, spd_y);
    fsr_dispatch(P_SCPYR, spd_x, spd_y);
    fsr_dispatch(P_SC, sc_x, sc_y);
    fsr_dispatch(P_REACT, src_x, src_y);
    fsr_dispatch(P_INST, src_x, src_y);
    fsr_dispatch(P_ACC, (uint32_t)(uw + 7) / 8, (uint32_t)(uh + 7) / 8);      /* at the output's size (AMD: dispatchDst) */
    F.res_index = (F.res_index + 1) % 16;
    }

    /* the result into the scene's colour: the bloom reads it there (a 2 x 2 gather that needs the scene's size); at another size
     * the tonemap reads FI_OUT itself (fsr_up_view) and this is a linear reduction */
    VkImageSubresourceLayers sl = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (uw != w || uh != h) {
        if (fmt_supported(vc->fmt, VK_FORMAT_FEATURE_BLIT_DST_BIT) && rs_filter(F.img[FI_OUT].vt) == VK_FILTER_LINEAR) {      /* shortcut: one linear step also above 2:1 (Ultra Performance): bloom may shimmer a little there; reduce in 2:1 steps if it shows */
            vt_layout(vc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit b; memset(&b, 0, sizeof b);
            b.srcSubresource = b.dstSubresource = sl; b.srcOffsets[1].x = uw; b.srcOffsets[1].y = uh; b.dstOffsets[1].x = w; b.dstOffsets[1].y = h; b.srcOffsets[1].z = b.dstOffsets[1].z = 1;
            vkCmdBlitImage(R.cmd, F.img[FI_OUT].vt->img, VK_IMAGE_LAYOUT_GENERAL, vc->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
        }
    } else if (F.blit) {
        vt_layout(vc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageBlit b; memset(&b, 0, sizeof b);
        b.srcSubresource = b.dstSubresource = sl; b.srcOffsets[1].x = b.dstOffsets[1].x = w; b.srcOffsets[1].y = b.dstOffsets[1].y = h; b.srcOffsets[1].z = b.dstOffsets[1].z = 1;
        vkCmdBlitImage(R.cmd, F.img[FI_OUT].vt->img, VK_IMAGE_LAYOUT_GENERAL, vc->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    } else {
        vt_layout(vc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy c; memset(&c, 0, sizeof c);
        c.srcSubresource = c.dstSubresource = sl; c.extent.width = (uint32_t)w; c.extent.height = (uint32_t)h; c.extent.depth = 1;
        vkCmdCopyImage(R.cmd, F.img[FI_OUT].vt->img, VK_IMAGE_LAYOUT_GENERAL, vc->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    game_fsr_w = w; game_fsr_h = h;
    if (F.runs++ == 0) printf("nvn_vk_fsr: first run at %dx%d to %dx%d (%d jitter phases) before a %s draw, jitter %.3f, %.3f px\n", w, h, uw, uh, game_fsr_phases, fx == FX_DOWN ? "bloom downsample" : "tonemap",
                              (double)F.k.jitterOffset[0], (double)F.k.jitterOffset[1]);
    else if (F.runs % 1800 == 0) printf("nvn_vk_fsr: %lld runs, %lld history resets, %lld tonemaps without FSR, jitter %.3f, %.3f px\n", (long long)F.runs, (long long)F.resets, (long long)F.missed, (double)F.k.jitterOffset[0], (double)F.k.jitterOffset[1]);
}

/* ---- issue 118, step 2: the dither. The depth pre-pass of fading objects (near the camera, LOD cross-fades: ZMap_*, 492 stages)
 * discards against a 64 x 64 pattern at the pixel, texelFetch(t14, uint(gl_FragCoord) & 63), threshold R * c7[4] + G * c7[5],
 * the same pixels every frame, so FSR keeps it as a fixed screen-door. While FSR runs the scene's draws get one of 13 phases
 * of the pattern each frame instead (fsr_dither.h): each a permutation of the game's thresholds, so a frame covers what the
 * game's does and over frames a pixel is drawn in the same share, which FSR's history makes a fade of. All draws of a frame get
 * the same phase. The pattern is learnt from a depth-only draw into the scene's depth, read back once and kept only if it is
 * the game's kind (R 1 .. 254, G = 255 - R): unit 14 is an ordinary material texture in many other stages. Shadow casters
 * dither in the shadow map: they keep theirs. */
#include "fsr_dither.h"
#define FSR_DN FSR_DITHER_PHASES
typedef struct { Tex *t; VTex *src; VkImage img; VTex *ph[FSR_DN]; VkImageView view[FSR_DN]; uint8_t swz[4]; } FsrDither;      /* src, img, swz: what the phases were made from; ph[0] NULL: not the game's kind of pattern */
static FsrDither g_fd[4];
static int g_nfd;
static int fsr_scene_depth(void) { Tex *d = R.depth.tex; int e = d ? d->b.width * 9 - d->b.height * 16 : 0; return d && !d->dead && d->b.width >= 640 && e <= 128 && e >= -128; }
/* nvn_vk_draw.c resolve_sampler, texture unit u of stage s: the view of this frame's phase of the pattern, NULL to keep the game's */
static VkImageView fsr_dither_view(int s, unsigned u, Tex *t, VTex *vt, const View *view) {
    if (s != 1 || u != 14 || t->b.width != 64 || t->b.height != 64 || vt->samples != 1 || vt->fmt != VK_FORMAT_R8G8B8A8_UNORM || vt->w != 64 || vt->h != 64 || !fsr_scene_depth()) return NULL;
    int i;
    for (i = 0; i < g_nfd && g_fd[i].t != t; i++) ;
    if (i == g_nfd) {
        if (R.ncol) return NULL;                   /* learnt only from a depth-only draw */
        if (g_nfd == 4) {                          /* full: the slot of a texture the game has finalised */
            for (i = 0; i < 4 && !g_fd[i].t->dead; i++) ;
            if (i == 4) { static int said; if (!said++) printf("nvn_vk_fsr: a fifth dither pattern is left as the game has it\n"); return NULL; }
            for (int k = 0; k < FSR_DN; k++) if (g_fd[i].ph[k]) vt_garbage(g_fd[i].ph[k]);
            memset(&g_fd[i], 0, sizeof g_fd[i]);
        } else i = g_nfd++;
        g_fd[i].t = t;
        printf("nvn_vk_fsr: dither pattern %d: a 64 x 64 texture (format 0x%x) read at unit 14 by the depth pre-pass\n", i + 1, t->b.format);
    }
    {   /* TEST ONLY: WT_FSR_DITHER=0 keeps the game's fixed pattern; =alt: fixed in every other 30 frames (an A/B in one run) */
        static int mode = -1;
        if (mode < 0) { const char *e = getenv("WT_FSR_DITHER"); mode = !e ? 1 : !strcmp(e, "alt") ? 2 : atoi(e) != 0; }
        if (game_fsr_w <= 0 || !fsr_want() || !mode || (mode == 2 && (F.frame / 30) % 2)) return NULL;
    }
    EView e;
    eview_of(t, vt, view, &e);
    if (e.format != t->b.format) return NULL;
    FsrDither *d = &g_fd[i];
    if (d->src != vt || d->img != vt->img || memcmp(d->swz, e.swz, 4)) {      /* made (again): level 0 read back once, 16 phases uploaded */
        static const VkComponentSwizzle sw[6] = {VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
        static uint8_t px[64 * 64 * 4], ph[64 * 64 * 4];
        for (int k = 0; k < FSR_DN; k++) if (d->ph[k]) { vt_garbage(d->ph[k]); d->ph[k] = NULL; d->view[k] = NULL; }
        d->src = vt; d->img = vt->img; memcpy(d->swz, e.swz, 4);
        VkBufferImageCopy c;
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent.width = 64; c.imageExtent.height = 64; c.imageExtent.depth = 1;
        VMem m;
        VkBuffer b = vbuf_new(sizeof px, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m);
        pass_end(); cmd_begin();
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        vkCmdCopyImageToBuffer(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, b, 1, &c);
        fsr_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);      /* a fence alone does not make it visible */
        cmd_wait_all();                            /* once per pattern: a few ms */
        memcpy(px, m.map, sizeof px);
        vkDestroyBuffer(V.device, b, NULL); vmem_free(m);
        cmd_begin();
        if (!fsr_dither_is_pattern(px)) { printf("nvn_vk_fsr: dither pattern %d is not the game's kind (R 1 .. 254, G = 255 - R): left as it is\n", i + 1); return NULL; }
        for (int k = 0; k < FSR_DN; k++) {
            fsr_dither_phase(px, ph, k);
            c.bufferOffset = ring_raw(ph, sizeof ph);      /* (may submit: before the commands that use it) */
            cmd_begin();
            FImg im;
            fsr_img(&im, vt->fmt, 64, 64, 1, 0);
            d->ph[k] = im.vt;
            vkCmdCopyBufferToImage(R.cmd, R.ring, im.vt->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            VkImageViewCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            ci.image = im.vt->img; ci.viewType = VK_IMAGE_VIEW_TYPE_2D; ci.format = vt->fmt;
            ci.components.r = sw[e.swz[0] < 6 ? e.swz[0] : 0]; ci.components.g = sw[e.swz[1] < 6 ? e.swz[1] : 0]; ci.components.b = sw[e.swz[2] < 6 ? e.swz[2] : 0]; ci.components.a = sw[e.swz[3] < 6 ? e.swz[3] : 0];
            ci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; ci.subresourceRange.levelCount = 1; ci.subresourceRange.layerCount = 1;
            VView *n = calloc(1, sizeof *n);
            VKC(vkCreateImageView(V.device, &ci, NULL, &n->view));
            n->vtype = -2; n->next = im.vt->views; im.vt->views = n; R.views++;
            d->view[k] = n->view;
        }
        fsr_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        printf("nvn_vk_fsr: dither pattern %d: %d phases made\n", i + 1, FSR_DN);
    }
    return d->ph[0] ? d->view[fsr_dither_phase_of(F.frame)] : NULL;
}
/* nvn_vk_draw.c resolve_sampler: a tonemap (FX_GRAIN) into a target at the picture's scale (rs_promote) reads FSR's output in place
 * of the scene colour (the 2:1 bloom reads the scene colour at its own size, see fsr_run). In a frame FSR did not run, a linear
 * enlargement of the scene colour (counted: the 1800-run line). NULL: the game's image. */
static VkImageView fsr_up_view(int s, Tex *t, VTex *vt, const View *view) {
    if (s != 1 || t != F.col || !F.img[FI_OUT].vt || (F.uw == F.w && F.uh == F.h) || !R.prog[1] || stage_fx(&R.prog[1]->st[1]) != FX_GRAIN || !R.ncol || !R.col[0].tex || !fsr_want()) return NULL;
    Tex *c = R.col[0].tex;
    if (!rs_post_has(c) && !(c->vk && ((VTex *)c->vk)->post)) return NULL;      /* (a promoted target has no image until its pass) */
    EView e;
    eview_of(t, vt, view, &e);
    if (e.format != t->b.format || e.lvl0 != 0 || e.swz[0] != 2 || e.swz[1] != 3 || e.swz[2] != 4 || e.swz[3] != 5) return NULL;
    if (F.ran != F.frame && F.up_frame != F.frame) {
        if (vt->w != F.w || vt->h != F.h || !fmt_supported(F.img[FI_OUT].vt->fmt, VK_FORMAT_FEATURE_BLIT_DST_BIT) || rs_filter(vt) != VK_FILTER_LINEAR) return NULL;
        pass_end(); cmd_begin();
        fsr_barrier(VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);      /* an earlier frame's reads and writes of FI_OUT */
        vt_layout(vt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkImageBlit b; memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = vt->w; b.srcOffsets[1].y = vt->h; b.dstOffsets[1].x = F.uw; b.dstOffsets[1].y = F.uh; b.srcOffsets[1].z = b.dstOffsets[1].z = 1;
        vkCmdBlitImage(R.cmd, vt->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, F.img[FI_OUT].vt->img, VK_IMAGE_LAYOUT_GENERAL, 1, &b, VK_FILTER_LINEAR);
        fsr_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
        F.up_frame = F.frame; F.missed++;
    }
    return F.img[FI_OUT].all;
}
static int fsr_reads(VStage *fs, Tex *want) {      /* the fragment stage samples this texture at some unit */
    for (int i = 0; fs && i < fs->nsmp; i++) {
        Tex *t = NULL; View view; SamplerParams sp;
        if (fs->smp[i].unit < 32 && (nvn_front_handle(R.tpool, R.spool, R.nvn_tex[1][fs->smp[i].unit], &t, &view, &sp) & 1) && t == want) return 1;
    }
    return 0;
}

/* nvn_vk_draw.c draw_prepare, before its textures: notes the scene's draws (depth and camera), runs FSR at the first draw that
 * reads the finished scene (the first bloom downsample, or the tonemap) */
static void fsr_draw(VStage *fs) {
    if (!fsr_want()) return;
    Tex *c = R.ncol ? R.col[0].tex : NULL, *d = R.depth.tex;
    if (c && d && !c->dead && !d->dead && c->vk && d->vk && c->b.width == d->b.width && c->b.height == d->b.height && c->b.width >= 640 && !wintex_is(c)) {
        VTex *vc = c->vk, *vd = d->vk;
        if (fsr_hdr(vc) && (vd->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && vc->samples == 1 && vd->samples == 1) {      /* a draw of the scene (opaque or transparent) */
            const Range *u = &R.ubo[0][0];
            F.depth = d; F.depth_frame = F.frame;
            if (u->addr && u->size >= 64 * 4) {
                const float *q = (const float *)P(u->addr);
                int jit = q[22] != 0.0f;      /* a perspective camera (projection row 3 = 0, 0, -1, 0); a jittered one wins over another in the frame */
                if (q[28] == 0.0f && q[29] == 0.0f && q[30] < -0.5f && q[31] == 0.0f && (F.cam_frame != F.frame || jit || !F.cam_jit)) { memcpy(F.c3, q, sizeof F.c3); F.cam_frame = F.frame; F.cam_jit = jit; }
            }
            return;
        }
    }
    int fx = fs && R.prog[1] ? stage_fx(&R.prog[1]->st[1]) : 0;
    if ((fx == FX_GRAIN || fx == FX_DOWN) && F.ran != F.frame && F.depth && F.depth_frame == F.frame && F.cam_frame == F.frame && !F.depth->dead)
        for (int i = 0; i < fs->nsmp; i++) {
            unsigned un = fs->smp[i].unit;
            Tex *t = NULL; View view; SamplerParams sp;
            if (un >= 32 || !(nvn_front_handle(R.tpool, R.spool, R.nvn_tex[1][un], &t, &view, &sp) & 1) || !t || t->dead || !t->vk) continue;
            if (t != c && t->b.width == F.depth->b.width && t->b.height == F.depth->b.height && fsr_hdr(t->vk) && ((VTex *)t->vk)->samples == 1) { fsr_run(t, F.depth, fx); break; }
        }
    /* upscaling: the target of a tonemap that reads FSR's scene colour goes to the picture's scale (before this draw's pass) */
    if (fx == FX_GRAIN && c && !c->dead && c->vk && !((VTex *)c->vk)->post && F.col && (F.uw != F.w || F.uh != F.h) && fsr_reads(fs, F.col)) rs_promote(c);
}
/* Motion vectors, step 2 (mv_match.h). nvn_vk_draw.c draw_prepare: a draw of the depth pre-pass (the scene's depth, no colour)
 * whose vertex stage reads constant buffer 14, the object's bone palette or instance records: the block is kept for the next
 * frame. fsr_mv_last gives last frame's bytes of the same palette: the game library names the slot it was in
 * (game_mv_pairs, game_shim_eh.c h_palette_upload). The velocity pass (step 3, below) draws with them.
 * TEST ONLY, WT_FSR_MVLOG=1: every 300 frames, how many blocks of the frame that ended have last frame's bytes, and how many
 * pre-pass draws take a vertex stream from a compute buffer or read a storage buffer in the vertex stage (no such draw has a
 * previous position from step 1 alone). */
typedef struct { volatile uint64_t dst, prev; } GameMvPair;
__declspec(dllimport) extern GameMvPair game_mv_pairs[1 << 16];
static MvStore g_mv;
static struct { int on, draws, blocks, paired, unnamed, gone, moved, cb_draws, ssbo_draws, replayed; } g_mvlog = {-1};
static const uint8_t *fsr_mv_last(uint64_t addr, uint32_t size, int *why) {      /* why: 1 the game named no slot, 2 that slot was not drawn last frame */
    const GameMvPair *p = &game_mv_pairs[(uint16_t)(((addr ^ 0) * 0x9e3779b97f4a7c15ull) >> 48)];      /* mv_hash(addr, 0) of game_shim_eh.c */
    uint64_t d = p->dst, prev = p->prev;
    if (d != addr || p->dst != addr || !prev) prev = addr;      /* not written since it was noted, or by other code: the same slot, if it was drawn last frame */
    const uint8_t *b = mv_last(&g_mv, prev, size);
    if (!b && prev == addr) { *why = 1; return NULL; }
    *why = b ? 0 : 2;
    return b;
}
/* Motion vectors, step 3: the velocity pass. A pre-pass draw whose palette changed since the last frame is drawn once more,
 * after the camera's vectors are made (fsr_run), into FI_MV: with the game's vertex stage translated to give last frame's
 * position too (mx_translate.c prev_cbufs: last frame's c3 and c14 at bindings 43 and 54), a fragment stage of ours, and a
 * depth test for equality against the scene's depth, so only the pixels the object owns in the finished depth get its vector
 * (a pixel its pre-pass stage discarded holds another depth). The game's own draws and pipelines are not touched.
 * The draw is noted while the game's is prepared (fsr_mv_prepare before the ring's last uploads, fsr_mv_record with the
 * vertex input, fsr_mv_commit with the draw's numbers) and replayed by fsr_mv_pass; what it reads stays in the stream ring or
 * in kept vertex buffers until then. Left on the camera's vector: instanced and tessellated draws, stages that sample a texture
 * or read a storage buffer in the vertex stage, draws fed by a compute buffer, an object in its first frame.
 * A stage's pipeline is made by a thread of its own from the stage's first draw (40 ms each on a cold cache, measured: on the
 * render thread that was a hitch a stage); until it is ready the stage's draws keep the camera's vector.
 * WT_FSR_MV=0: no velocity pass. */
typedef struct { uint32_t id; VkFormat dfmt; volatile LONG state; VkPipeline p; uint32_t prev; } MvPipe;      /* state: 0 free, 2 being made, 1 ready, -1 this stage has no previous position; prev: MxOut.prev_cbufs */
typedef struct MvJob { struct MvJob *next; MvPipe *mp; uint32_t key[4], cdata_size; int code_len; uint8_t data[1]; } MvJob;      /* data: a copy of the code, then of its constants (the program may go) */
static MvJob *g_mvj_head, **g_mvj_tail = &g_mvj_head;
static CRITICAL_SECTION g_mvj_lock; static CONDITION_VARIABLE g_mvj_cv; static HANDLE g_mvj_thread; static volatile LONG g_mvj_quit;
typedef struct {
    uint32_t gen; VkPipeline p;
    uint32_t vnb, vna; VkVertexInputBindingDescription2EXT vbd[17]; VkVertexInputAttributeDescription2EXT vad[16]; VkBuffer vb[17]; VkDeviceSize vo[17];
    int nb; struct { uint32_t binding; VkBuffer buf; VkDeviceSize off, range; } b[20];
    VkViewport vp; VkRect2D sc; uint32_t cull, ff, dbe, topo, rst, pc[3]; float bias[3];
    int indexed; VkBuffer ib; VkDeviceSize iboff; VkIndexType it; uint32_t count, inst, first, first_inst; int32_t base;
} MvDraw;
typedef struct { uint8_t b[0x1000]; uint32_t n, ring_gen; uint64_t addr, ring_off; LONG64 frame, ring_frame; } MvCam;
static MvPipe g_mvp[4096];                      /* shortcut: never emptied; a session with more vertex stage variants leaves the later ones on the camera's vector */
static VkPipelineLayout g_mv_pl; static VkDescriptorSetLayout g_mv_dl; static VkShaderModule g_mv_fs[32];
static MvCam g_mvcam[2];                           /* the pre-pass's camera (c3) of this frame and the last, by the frame's parity */
static struct { MvDraw *d; int n, cap, open; Tex *depth; VkImageView dview; LONG64 frame; int pipes, bad; double pipe_ms; } g_mvd;      /* pipes, bad, pipe_ms: the pipeline thread's counts, for the log */
static int fsr_mv_on(void) { static int on = -1; if (on < 0) { const char *e = getenv("WT_FSR_MV"); on = !(e && !atoi(e)) && D.has_vids && fmt_supported(VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT); } return on; }

/* the pipeline thread: one stage's velocity pipeline */
static void fsr_mv_build(MvJob *j) {
    MvPipe *mp = j->mp;
    LONG64 t0 = tk();
    MxIn in; MxOut out;
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.stage = 0; in.code = j->data; in.code_len = j->code_len; in.cdata = j->cdata_size ? j->data + j->code_len : NULL; in.cdata_size = j->cdata_size;
    in.int_attrs = j->key[0]; in.sint_attrs = j->key[1]; in.int_tex = j->key[2]; in.sint_tex = j->key[3];
    in.target = 1; in.ssbo_base = 20; in.image_base = 32; in.wmask_binding = 38; in.loopdebug = vk_loopdebug();      /* as vstage_build, with no forced outputs */
    in.prev_cbufs = 1u << 3 | 1u << 14; in.prev_base = 40;
    if (!mx_translate(&in, &out)) { g_mvd.bad++; InterlockedExchange(&mp->state, -1); return; }
    VkShaderModule vm = (out.prev_cbufs >> 14 & 1) && !out.nsmp && !out.ssbos && !out.images && out.prev_loc < 32 ? module_from_glsl(0, out.glsl, "motion vectors, a vertex stage", &g_spv_unused) : NULL;
    free(out.glsl);
    if (!vm) { g_mvd.bad++; InterlockedExchange(&mp->state, -1); return; }
    if (!g_mv_fs[out.prev_loc]) {
        char fs[700];
        snprintf(fs, sizeof fs, "#version 460\nlayout(location = %d) in vec4 mx_prev;\nlayout(location = 0) out vec2 mv;\n"
                 "layout(push_constant) uniform PC { layout(offset = 16) vec4 sj; vec2 pj; };\n"      /* sj: width, height, this frame's NDC jitter; pj: last frame's (as k_fsr_mv) */
                 "void main() {\n  if (mx_prev.w <= 0.0) discard;\n  vec2 uv = gl_FragCoord.xy / sj.xy;\n  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0) - sj.zw;\n"
                 "  vec2 pn = mx_prev.xy / mx_prev.w - pj;\n  mv = vec2((pn.x - ndc.x) * 0.5, (ndc.y - pn.y) * 0.5);\n}\n", out.prev_loc);
        g_mv_fs[out.prev_loc] = module_from_glsl(1, fs, "motion vectors, the fragment stage", &g_spv_unused);
        if (!g_mv_fs[out.prev_loc]) { vkDestroyShaderModule(V.device, vm, NULL); g_mvd.bad++; InterlockedExchange(&mp->state, -1); return; }
    }
    VkPipelineShaderStageCreateInfo st[2];
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT; st[0].module = vm; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = g_mv_fs[out.prev_loc]; st[1].pName = "main";
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = VK_TRUE; dss.depthCompareOp = VK_COMPARE_OP_EQUAL;      /* no depth write */
    VkPipelineColorBlendAttachmentState ba;
    memset(&ba, 0, sizeof ba); ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT;
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &ba;
    VkDynamicState dyn[9] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY,
                             VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE, VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_VERTEX_INPUT_EXT};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 9; ds.pDynamicStates = dyn;
    VkFormat cf = VK_FORMAT_R16G16_SFLOAT;
    VkPipelineRenderingCreateInfo ren = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ren.colorAttachmentCount = 1; ren.pColorAttachmentFormats = &cf; ren.depthAttachmentFormat = mp->dfmt;
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gi.pNext = &ren; gi.stageCount = 2; gi.pStages = st; gi.pInputAssemblyState = &ia; gi.pViewportState = &vps;
    gi.pRasterizationState = &rs; gi.pMultisampleState = &ms; gi.pDepthStencilState = &dss; gi.pColorBlendState = &cb; gi.pDynamicState = &ds; gi.layout = g_mv_pl;
    VkResult r = vkCreateGraphicsPipelines(V.device, g_pcache, 1, &gi, NULL, &mp->p);
    vkDestroyShaderModule(V.device, vm, NULL);
    if (r != VK_SUCCESS) { mp->p = NULL; g_mvd.bad++; InterlockedExchange(&mp->state, -1); return; }
    mp->prev = out.prev_cbufs; g_mvd.pipes++;
    {   LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); g_mvd.pipe_ms += (double)(tk() - t0) * 1000.0 / (double)fq.QuadPart; }
    InterlockedExchange(&mp->state, 1);
}
static DWORD WINAPI fsr_mv_thread(LPVOID arg) {
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    for (;;) {
        EnterCriticalSection(&g_mvj_lock);
        while (!g_mvj_head && !g_mvj_quit) SleepConditionVariableCS(&g_mvj_cv, &g_mvj_lock, INFINITE);
        MvJob *j = g_mvj_quit ? NULL : g_mvj_head;
        if (j) { g_mvj_head = j->next; if (!g_mvj_head) g_mvj_tail = &g_mvj_head; }
        LeaveCriticalSection(&g_mvj_lock);
        if (!j) return 0;
        fsr_mv_build(j);
        free(j);
    }
}
/* the velocity pipeline of a vertex stage (render thread); NULL: none, or not made yet (asked for at the first call) */
static MvPipe *fsr_mv_pipe(VStage *vs, VkFormat dfmt) {
    MvPipe *mp = NULL;
    for (unsigned i = vs->id & 4095, n = 0; n < 4096; i = (i + 1) & 4095, n++) {
        if (g_mvp[i].state && g_mvp[i].id == vs->id && g_mvp[i].dfmt == dfmt) return g_mvp[i].state == 1 ? &g_mvp[i] : NULL;
        if (!g_mvp[i].state) { mp = &g_mvp[i]; break; }
    }
    SSrc src;
    if (!mp) return NULL;                          /* (a full table: the camera's vector) */
    mp->id = vs->id; mp->dfmt = dfmt; mp->state = -1;
    if (vs->nsmp || vs->ssbos || vs->images || !ssrc_of(&src, R.prog[0], 0)) return NULL;
    if (!g_mv_pl) {                                /* one layout: the vertex stage's constant buffers 1 .. 17, last frame's c3 and c14 */
        VkDescriptorSetLayoutBinding lb[19];
        for (uint32_t n = 0; n < 19; n++) { VkDescriptorSetLayoutBinding b = {n < 17 ? n + 1 : n == 17 ? 43u : 54u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL}; lb[n] = b; }
        VkDescriptorSetLayoutCreateInfo li = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR; li.bindingCount = 19; li.pBindings = lb;
        VKC(vkCreateDescriptorSetLayout(V.device, &li, NULL, &g_mv_dl));
        VkPushConstantRange pr[2] = {{VK_SHADER_STAGE_VERTEX_BIT, 0, 16}, {VK_SHADER_STAGE_FRAGMENT_BIT, 16, 24}};
        VkPipelineLayoutCreateInfo pli = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 1; pli.pSetLayouts = &g_mv_dl; pli.pushConstantRangeCount = 2; pli.pPushConstantRanges = pr;
        VKC(vkCreatePipelineLayout(V.device, &pli, NULL, &g_mv_pl));
    }
    if (!g_mvj_thread) { InitializeCriticalSection(&g_mvj_lock); InitializeConditionVariable(&g_mvj_cv); g_mvj_thread = CreateThread(NULL, 0, fsr_mv_thread, NULL, 0, NULL); }
    uint32_t cn = src.cdata ? src.cdata_size : 0;
    MvJob *j = malloc(sizeof *j + (size_t)src.code_len + cn);
    j->next = NULL; j->mp = mp; memcpy(j->key, vs->key, sizeof j->key); j->code_len = src.code_len; j->cdata_size = cn;
    memcpy(j->data, src.code, (size_t)src.code_len); if (cn) memcpy(j->data + src.code_len, src.cdata, cn);
    mp->state = 2;
    EnterCriticalSection(&g_mvj_lock); *g_mvj_tail = j; g_mvj_tail = &j->next; LeaveCriticalSection(&g_mvj_lock);
    WakeConditionVariable(&g_mvj_cv);
    return NULL;
}
/* nvn_vk_draw.c draw_prepare, with the draw's buffers (the ring may still wrap): the pre-pass's block is noted, and when it
 * changed since the last frame the draw's last-frame blocks go into the ring */
static int fsr_mv_prepare(VStage *vs, int tess, MvPrep *o) {
    const Range *u = &R.ubo[0][11], *u3 = &R.ubo[0][0];
    g_mvd.open = 0;
    if (R.ncol || !(vs->cbufs >> 14 & 1) || !u->addr || u->size > MV_MAX_BLOCK || !fsr_want() || !fsr_scene_depth()) return 0;
    int fresh = mv_note(&g_mv, u->addr, (uint32_t)u->size, P(u->addr)), why;
    const uint8_t *last = fsr_mv_last(u->addr, (uint32_t)u->size, &why);
    if (g_mvlog.on < 0) g_mvlog.on = getenv("WT_FSR_MVLOG") != NULL;
    if (g_mvlog.on) { g_mvlog.draws++; if (fresh) { g_mvlog.blocks++; if (last) { g_mvlog.paired++; g_mvlog.moved += memcmp(last, P(u->addr), u->size) != 0; } else if (why == 1) g_mvlog.unnamed++; else g_mvlog.gone++; } }
    if (!last || tess || !fsr_mv_on() || !u3->addr || !u3->size || u3->size > sizeof g_mvcam[0].b) return 0;
    MvCam *cam = &g_mvcam[F.frame & 1], *pcam = &g_mvcam[~F.frame & 1];
    if (cam->frame != F.frame || !cam->n) { cam->frame = F.frame; cam->addr = u3->addr; cam->n = (uint32_t)u3->size; memcpy(cam->b, P(u3->addr), u3->size); }      /* the pre-pass's first camera is the scene's */
    if (cam->addr != u3->addr || pcam->frame != F.frame - 1 || pcam->n < u3->size || !R.depth.tex->vk) return 0;
    {   static int all = -1;                       /* TEST ONLY: WT_FSR_MV_ALL=1 draws the ones that did not move too: their vectors must equal the camera's */
        if (all < 0) all = getenv("WT_FSR_MV_ALL") != NULL;
        if (!all && !memcmp(last, P(u->addr), u->size)) return 0; }      /* it did not move: the camera's vector is exact */
    MvPipe *mp = fsr_mv_pipe(vs, ((VTex *)R.depth.tex->vk)->fmt);
    if (!mp) return 0;
    o->pipe = mp->p; o->prev = mp->prev;
    o->p14 = ring_raw(last, u->size); o->n14 = (uint32_t)u->size;
    if (pcam->ring_frame != F.frame || pcam->ring_gen != R.gen) { pcam->ring_off = ring_raw(pcam->b, pcam->n); pcam->ring_gen = R.gen; pcam->ring_frame = F.frame; }
    o->p3 = pcam->ring_off; o->n3 = pcam->n;
    return 1;
}
/* draw_prepare, after the game's vertex buffers are bound: the draw's state as the command buffer has it now */
static void fsr_mv_record(const MvPrep *q, VStage *vs, const BufRef *bufs, uint32_t vnb, const VkVertexInputBindingDescription2EXT *vbd, uint32_t vna, const VkVertexInputAttributeDescription2EXT *vad,
                          const VkBuffer *vb, const VkDeviceSize *vo, VRes **vkept, int from_cb) {
    if (from_cb || !g_dsh_ok || vnb > 17 || vna > 16) return;
    if (g_mvd.frame != F.frame) { g_mvd.frame = F.frame; g_mvd.n = 0; g_mvd.depth = R.depth.tex; g_mvd.dview = target_view(&R.depth, R.depth.tex->vk); }
    if (g_mvd.depth != R.depth.tex) return;        /* another depth image of the scene's size: not the one FSR reads (shortcut: the first one of the frame is taken) */
    if (g_mvd.n == g_mvd.cap) { g_mvd.cap = g_mvd.cap ? g_mvd.cap * 2 : 256; g_mvd.d = realloc(g_mvd.d, (size_t)g_mvd.cap * sizeof *g_mvd.d); }
    MvDraw *e = &g_mvd.d[g_mvd.n];
    e->gen = R.gen; e->p = (VkPipeline)q->pipe; e->nb = 0;
    for (unsigned n = 1, ib = 0; n < 18; n++) {    /* the vertex stage's buffers lead draw_prepare's list, in this order */
        if (!(vs->cbufs >> n & 1)) continue;
        const BufRef *b = &bufs[ib++];
        if (!b->zero && b->buf != R.ring) return;
        e->b[e->nb].binding = n; e->b[e->nb].buf = b->zero ? R.zero : b->buf; e->b[e->nb].off = b->zero ? 0 : b->off; e->b[e->nb].range = b->zero ? 65536 : b->size; e->nb++;
    }
    e->b[e->nb].binding = 54; e->b[e->nb].buf = R.ring; e->b[e->nb].off = q->p14; e->b[e->nb].range = q->n14; e->nb++;
    if (q->prev >> 3 & 1) { e->b[e->nb].binding = 43; e->b[e->nb].buf = R.ring; e->b[e->nb].off = q->p3; e->b[e->nb].range = q->n3; e->nb++; }
    e->vnb = vnb; e->vna = vna;
    memcpy(e->vbd, vbd, vnb * sizeof *vbd); memcpy(e->vad, vad, vna * sizeof *vad); memcpy(e->vb, vb, vnb * sizeof *vb); memcpy(e->vo, vo, vnb * sizeof *vo);
    for (int i = 0; i < 16; i++) if (vkept[i] && vkept[i]->busy < R.serial + 256) vkept[i]->busy = R.serial + 256;      /* a kept buffer dropped before the pass stays until it has run (vrc_collect) */
    e->vp = g_dsh.vp; e->sc = g_dsh.sc; e->cull = g_dsh.cull; e->ff = g_dsh.ff; e->dbe = g_dsh.dbe; memcpy(e->bias, g_dsh.bias, sizeof e->bias);
    e->topo = (uint32_t)g_dsh_topo; e->rst = (uint32_t)g_dsh_rst;
    e->pc[0] = (uint32_t)R.alpha_func; memcpy(&e->pc[1], &R.alpha_ref, 4); memcpy(&e->pc[2], &R.point_size, 4);
    g_mvd.open = 1;
}
/* the replay, after the game's own draw call: the draw's numbers */
static void fsr_mv_commit(int indexed, VkBuffer ib, VkDeviceSize iboff, VkIndexType it, uint32_t count, uint32_t inst, uint32_t first, int32_t base, uint32_t first_inst) {
    if (!g_mvd.open) return;
    g_mvd.open = 0;
    if (indexed && ib != R.ring) return;
    MvDraw *e = &g_mvd.d[g_mvd.n++];
    e->indexed = indexed; e->ib = ib; e->iboff = iboff; e->it = it; e->count = count; e->inst = inst; e->first = first; e->base = base; e->first_inst = first_inst;
}
static void mv_jitter(const uint8_t *c3, int w, int h, float *j) {      /* as fsr_run reads it from the projection */
    float m[32]; memcpy(m, c3, sizeof m);
    double jx = m[30] != 0.0f ? (double)m[18] / m[30] : 0.0, jy = m[30] != 0.0f ? (double)m[22] / m[30] : 0.0;
    if (fabs(jx) * w > 1.15 || fabs(jy) * h > 1.15) jx = jy = 0.0;
    j[0] = (float)jx; j[1] = (float)jy;
}
/* fsr_run, after the camera's vectors: the noted draws over them. Returns the number drawn. */
static int fsr_mv_pass(Tex *depth, VTex *vd, int w, int h) {
    const MvCam *cam = &g_mvcam[F.frame & 1], *pcam = &g_mvcam[~F.frame & 1];
    int n = g_mvd.frame == F.frame && g_mvd.depth == depth && cam->frame == F.frame && pcam->frame == F.frame - 1 ? g_mvd.n : 0, drawn = 0;
    g_mvd.n = 0;
    if (!n || !(F.img[FI_MV].vt->storage & 2)) return 0;
    float pc[6] = {(float)w, (float)h, 0, 0, 0, 0};
    mv_jitter(cam->b, w, h, &pc[2]); mv_jitter(pcam->b, w, h, &pc[4]);
    fsr_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT);
    vt_layout(vd, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}, da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = F.img[FI_MV].lvl[0]; ca.imageLayout = VK_IMAGE_LAYOUT_GENERAL; ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    da.imageView = g_mvd.dview; da.imageLayout = vd->layout; da.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea.extent.width = (uint32_t)w; ri.renderArea.extent.height = (uint32_t)h; ri.layerCount = 1; ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
    vkCmdBeginRendering(R.cmd, &ri);
    VkPipeline bound = NULL;
    for (int i = 0; i < n; i++) {
        const MvDraw *e = &g_mvd.d[i];
        if (e->gen != R.gen) continue;             /* the ring wrapped since: its bytes may be gone */
        VkWriteDescriptorSet wr[20]; VkDescriptorBufferInfo bi[20];
        for (int k = 0; k < e->nb; k++) {
            bi[k].buffer = e->b[k].buf; bi[k].offset = e->b[k].off; bi[k].range = e->b[k].range;
            wr[k] = (VkWriteDescriptorSet){VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, NULL, e->b[k].binding, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &bi[k], NULL};
        }
        if (e->p != bound) { vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, e->p); bound = e->p; }
        vkCmdSetViewport(R.cmd, 0, 1, &e->vp); vkCmdSetScissor(R.cmd, 0, 1, &e->sc);
        vkCmdSetCullMode(R.cmd, e->cull); vkCmdSetFrontFace(R.cmd, (VkFrontFace)e->ff);
        vkCmdSetPrimitiveTopology(R.cmd, (VkPrimitiveTopology)e->topo); vkCmdSetPrimitiveRestartEnable(R.cmd, e->rst);
        vkCmdSetDepthBiasEnable(R.cmd, e->dbe); vkCmdSetDepthBias(R.cmd, e->bias[0], e->bias[1], e->bias[2]);
        vkCmdSetVertexInputEXT(R.cmd, e->vnb, e->vbd, e->vna, e->vad);
        if (e->vnb) vkCmdBindVertexBuffers(R.cmd, 0, e->vnb, e->vb, e->vo);
        vkCmdPushDescriptorSetKHR(R.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_mv_pl, 0, (uint32_t)e->nb, wr);
        vkCmdPushConstants(R.cmd, g_mv_pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 12, e->pc);
        vkCmdPushConstants(R.cmd, g_mv_pl, VK_SHADER_STAGE_FRAGMENT_BIT, 16, 24, pc);
        if (e->indexed) { vkCmdBindIndexBuffer(R.cmd, e->ib, e->iboff, e->it); vkCmdDrawIndexed(R.cmd, e->count, e->inst, e->first, e->base, e->first_inst); }
        else vkCmdDraw(R.cmd, e->count, e->inst, e->first, e->first_inst);
        drawn++;
    }
    vkCmdEndRendering(R.cmd);
    fsr_barrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT);
    vt_layout(vd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    F.depth_layout = vd->layout;
    R.pipe_bound = NULL; dsh_forget(); R.dyn_dirty = R.vi_dirty = R.dyn3_dirty = 1;      /* a pipeline and dynamic state of our own */
    g_mvlog.replayed += drawn;
    return drawn;
}
/* TEST ONLY (WT_FSR_MVDUMP=<frame>): FI_MV of that frame before and after the velocity pass, raw RG16F rows, as
 * mv_<frame>_<0|1>_<width>x<height>.bin in WT_SHOT_DIR (or here). One GPU wait. */
static void fsr_mv_dump(Tex *depth, VTex *vd, int w, int h) {
    static LONG64 at = -2;
    if (at == -2) at = getenv("WT_FSR_MVDUMP") ? atoll(getenv("WT_FSR_MVDUMP")) : -1;
    if (F.frame != at) { fsr_mv_pass(depth, vd, w, h); return; }
    VMem m[2]; VkBuffer b[2];
    VkBufferImageCopy c; memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent.width = (uint32_t)w; c.imageExtent.height = (uint32_t)h; c.imageExtent.depth = 1;
    int drawn = 0;
    for (int k = 0; k < 2; k++) {
        b[k] = vbuf_new((uint64_t)w * h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &m[k]);
        if (k) drawn = fsr_mv_pass(depth, vd, w, h);
        fsr_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
        vkCmdCopyImageToBuffer(R.cmd, F.img[FI_MV].vt->img, VK_IMAGE_LAYOUT_GENERAL, b[k], 1, &c);
    }
    fsr_barrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    cmd_wait_all();
    for (int k = 0; k < 2; k++) {
        char path[MAX_PATH]; snprintf(path, sizeof path, "%s/mv_%lld_%d_%dx%d.bin", getenv("WT_SHOT_DIR") ? getenv("WT_SHOT_DIR") : ".", (long long)F.frame, k, w, h);
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(m[k].map, 4, (size_t)w * h, f); fclose(f); }
        vkDestroyBuffer(V.device, b[k], NULL); vmem_free(m[k]);
    }
    printf("nvn_vk_fsr: TEST: motion vectors of frame %lld written (%dx%d, %d velocity draws)\n", (long long)F.frame, w, h, drawn);
    cmd_begin();
}
/* TEST ONLY: nvn_vk_draw.c draw_prepare, after the vertex streams */
static void fsr_mv_streams(int from_cb, VStage *vs) { if (g_mvlog.on > 0 && !R.ncol && fsr_scene_depth()) { g_mvlog.cb_draws += from_cb != 0; g_mvlog.ssbo_draws += vs->ssbos != 0; } }
/* nvn_vk_draw.c, at shutdown (the GPU is idle; the images went with the others) */
static void fsr_shutdown(void) {
    dlss_shutdown();
    for (int i = 0; i < 2; i++) { free(g_mv.s[i].b); free(g_mv.s[i].d); }
    memset(&g_mv, 0, sizeof g_mv);
    if (g_mvj_thread) {                            /* the pipeline thread first: it uses the device */
        EnterCriticalSection(&g_mvj_lock); g_mvj_quit = 1; LeaveCriticalSection(&g_mvj_lock); WakeConditionVariable(&g_mvj_cv);
        WaitForSingleObject(g_mvj_thread, INFINITE); CloseHandle(g_mvj_thread); g_mvj_thread = NULL; g_mvj_quit = 0;
        while (g_mvj_head) { MvJob *j = g_mvj_head; g_mvj_head = j->next; free(j); }
        g_mvj_tail = &g_mvj_head; DeleteCriticalSection(&g_mvj_lock);
    }
    for (int i = 0; i < 4096; i++) if (g_mvp[i].p) vkDestroyPipeline(V.device, g_mvp[i].p, NULL);
    for (int i = 0; i < 32; i++) if (g_mv_fs[i]) vkDestroyShaderModule(V.device, g_mv_fs[i], NULL);
    if (g_mv_pl) { vkDestroyPipelineLayout(V.device, g_mv_pl, NULL); vkDestroyDescriptorSetLayout(V.device, g_mv_dl, NULL); }
    free(g_mvd.d); memset(&g_mvd, 0, sizeof g_mvd); memset(g_mvp, 0, sizeof g_mvp); memset(g_mv_fs, 0, sizeof g_mv_fs); g_mv_pl = NULL;
    for (int p = 0; p < P_N; p++) {
        if (F.pipe[p]) vkDestroyPipeline(V.device, F.pipe[p], NULL);
        if (F.pl[p]) vkDestroyPipelineLayout(V.device, F.pl[p], NULL);
        if (F.dl[p]) vkDestroyDescriptorSetLayout(V.device, F.dl[p], NULL);
    }
    for (int i = 0; i < 2; i++) if (F.smp[i]) vkDestroySampler(V.device, F.smp[i], NULL);
    if (F.ubo) { vkDestroyBuffer(V.device, F.ubo, NULL); vmem_free(F.ubo_mem); }
    memset(&F, 0, sizeof F);
}
/* nvn_vk_draw.c vk_present2: a frame ends */
static void fsr_present(void) {
    if (F.ran != F.frame) game_fsr_w = game_fsr_h = 0;      /* a frame without FSR: the game stops jittering (a later run starts it again, with a reset) */
    if (!fsr_want() && F.img[FI_OUT].vt) {                  /* turned off: its images go (the frames in flight keep them alive, vt_garbage) */
        for (int i = 0; i < FI_N; i++) if (F.img[i].vt) vt_garbage(F.img[i].vt);
        memset(F.img, 0, sizeof F.img); F.w = F.h = 0;
    }
    if (g_mvlog.on > 0) {
        if (F.frame % 300 == 0) printf("nvn_vk_fsr: motion blocks, frame %lld: %d pre-pass draws, %d blocks: %d with last frame's bytes (%d of them changed), %d the game named no slot for, %d whose slot was not drawn last frame; %d draws with a vertex stream from a compute buffer, %d whose vertex stage reads a storage buffer; velocity pass: %d draws, %d pipelines so far (%.1f ms in all, %d stages without)\n",
                                       (long long)F.frame, g_mvlog.draws, g_mvlog.blocks, g_mvlog.paired, g_mvlog.moved, g_mvlog.unnamed, g_mvlog.gone, g_mvlog.cb_draws, g_mvlog.ssbo_draws, g_mvlog.replayed, g_mvd.pipes, g_mvd.pipe_ms, g_mvd.bad);
        g_mvlog.draws = g_mvlog.blocks = g_mvlog.paired = g_mvlog.unnamed = g_mvlog.gone = g_mvlog.moved = g_mvlog.cb_draws = g_mvlog.ssbo_draws = g_mvlog.replayed = 0;
    }
    dlss_present();
    mv_flip(&g_mv);
    F.frame++;
}
