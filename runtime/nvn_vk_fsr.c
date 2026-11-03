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

__declspec(dllimport) extern volatile int game_fsr_w, game_fsr_h;      /* game_shim_eh.c h_cam_upload: the jitter's pixel size, 0 = none */

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
static const struct { VkFormat f; uint8_t size; } k_fi[FI_N] = {      /* size: 0 the scene's, 1 half of it, 2 one texel, 3 the 128 x 1 LUT, 4 the output */
    {VK_FORMAT_R8_UNORM, 0}, {VK_FORMAT_R8_UNORM, 0}, {VK_FORMAT_R16_SFLOAT, 0}, {VK_FORMAT_R16_SFLOAT, 0}, {VK_FORMAT_R16_SFLOAT, 0},
    {VK_FORMAT_R8_UNORM, 1}, {VK_FORMAT_R8_UNORM, 0}, {VK_FORMAT_R16G16B16A16_SFLOAT, 0}, {VK_FORMAT_R16G16B16A16_SFLOAT, 0},
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
    int w, h, blit; FImg img[FI_N];                /* blit: the output is RGBA16F and blitted into the scene's format (no storage for it) */
    FsrConst k; uint32_t res_index; int fresh;    /* fresh: resources just made: clear them, reset */
    double prev_vp[16]; float prev_fwd[3], prev_pos[3]; int have_prev;
    LONG64 frame, ran, depth_frame, cam_frame; LONG64 t_last;
    Tex *depth; float c3[64]; int cam_jit;           /* cam_jit: the c3 kept this frame carries our jitter ([1][2] != 0: Halton(k, 3) - 0.5 never is) */
    VkImageView col_view, depth_view; VkImageLayout col_layout, depth_layout;
    LONG64 runs, resets;
} F;

static int fsr_want(void) { return gfx_options()->upscaler == 1 && F.ok >= 0; }
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
    vt->magic = 0x56544558; vt->fmt = f; vt->aspect = VK_IMAGE_ASPECT_COLOR_BIT; vt->w = w; vt->h = h; vt->d = 1; vt->levels = levels; vt->layers = 1; vt->samples = 1; vt->storage = storage;
    ci.imageType = VK_IMAGE_TYPE_2D; ci.format = f; ci.extent.width = (uint32_t)w; ci.extent.height = (uint32_t)h; ci.extent.depth = 1; ci.mipLevels = (uint32_t)levels; ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL; ci.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
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

static int fsr_setup(void) {                       /* render thread, once: samplers, the uniform ring, the 8 pipelines */
    if (!D.has_push) { fsr_off("the device has no VK_KHR_push_descriptor"); return 0; }
    if (!D.f2.features.shaderStorageImageWriteWithoutFormat) { fsr_off("the device has no shaderStorageImageWriteWithoutFormat"); return 0; }
    for (int i = 0; i < FI_N; i++)
        if (k_fi[i].f && i != FI_LUT && !fmt_supported(k_fi[i].f, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { char m[96]; snprintf(m, sizeof m, "no storage images of format %d", (int)k_fi[i].f); fsr_off(m); return 0; }
    if (!fmt_supported(VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT)) { fsr_off("no image atomics on R32_UINT"); return 0; }
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
        if (p != P_MV) for (int i = 0; i < 2; i++) { VkDescriptorSetLayoutBinding b = {1000u + (uint32_t)i, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &F.smp[i]}; lb[n++] = b; }
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

static void fsr_resize(int w, int h, VkFormat col) {      /* the scene's size: every resource made again, history reset */
    for (int i = 0; i < FI_N; i++) if (F.img[i].vt) { vt_garbage(F.img[i].vt); F.img[i].vt = NULL; }
    F.blit = !fmt_supported(col, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    for (int i = 0; i < FI_N; i++) {
        int s = k_fi[i].size, iw = s == 0 || s == 4 ? w : s == 1 ? w / 2 : s == 3 ? 128 : 1, ih = s == 0 || s == 4 ? h : s == 1 ? h / 2 : 1, lv = 1;
        if (i == FI_SPD) { int m = iw > ih ? iw : ih; while (m > 1) { m >>= 1; lv++; } }      /* AMD: the whole chain */
        fsr_img(&F.img[i], s == 4 ? (F.blit ? VK_FORMAT_R16G16B16A16_SFLOAT : col) : k_fi[i].f, iw, ih, lv, i != FI_LUT);
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
    F.w = w; F.h = h; F.fresh = 1;
    printf("nvn_vk_fsr: resources for %dx%d (output %s)\n", w, h, F.blit ? "RGBA16F, blitted" : "in the scene's format, copied");
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
    vkCmdBindPipeline(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.pipe[p]);
    vkCmdPushDescriptorSetKHR(R.cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.pl[p], 0, ps->n, w);
    vkCmdDispatch(R.cmd, gx ? gx : 1, gy ? gy : 1, 1);
    fsr_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
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
    int w = vc->w, h = vc->h;
    if (vd->w != w || vd->h != h) return;
    if (F.ok == 0 && !fsr_setup()) return;
    pass_end();
    cmd_begin();
    pt_mark(w, h, 0, 0x118, 0, 0, 1);              /* WT_VK_PASSTIME: FSR (and its copy) as a kind of its own, "format 0x118" */
    if (w != F.w || h != F.h || !F.img[FI_OUT].vt || (!F.blit && F.img[FI_OUT].vt->fmt != vc->fmt)) fsr_resize(w, h, vc->fmt);
    LONG64 now = tk();
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    double dt = F.t_last ? (double)(now - F.t_last) / (double)fq.QuadPart : 1.0 / 60.0;
    F.t_last = now;

    /* the camera of the scene's draws: c3 rows (h_cam_upload): [0..15] view, [16..31] projection, [32..47] VP, [48..63] inverse view */
    const float *m = F.c3;
    double jx = m[30] != 0.0f ? (double)m[18] / m[30] : 0.0, jy = m[30] != 0.0f ? (double)m[22] / m[30] : 0.0;      /* the projection's [0][2], [1][2]: 0 but for our jitter */
    if (fabs(jx) * w > 1.01 || fabs(jy) * h > 1.01) jx = jy = 0.0;      /* an off-centre projection of the game's own, not our jitter */
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
        k->renderSize[0] = k->upscaleSize[0] = k->maxRenderSize[0] = k->maxUpscaleSize[0] = w;
        k->renderSize[1] = k->upscaleSize[1] = k->maxRenderSize[1] = k->maxUpscaleSize[1] = h;
        k->deviceToViewDepth[0] = (float)(-q); k->deviceToViewDepth[1] = (float)(q * zn); k->deviceToViewDepth[2] = (float)(t * asp); k->deviceToViewDepth[3] = (float)t;
        k->previousFrameJitterOffset[0] = k->jitterOffset[0]; k->previousFrameJitterOffset[1] = k->jitterOffset[1];
        k->jitterOffset[0] = (float)(jx * w * 0.5); k->jitterOffset[1] = (float)(-jy * h * 0.5);      /* pixels, y down (AMD: projection += (2x/w, -2y/h)) */
        k->motionVectorScale[0] = k->motionVectorScale[1] = 1.0f;      /* ours are in UV already */
        k->downscaleFactor[0] = k->downscaleFactor[1] = 1.0f;
        k->motionVectorJitterCancellation[0] = k->motionVectorJitterCancellation[1] = 0.0f;
        k->tanHalfFOV = (float)(t * asp);
        k->jitterPhaseCount = 8.0f;                 /* ffxFsr3UpscalerGetJitterPhaseCount at 1:1; h_cam_upload's sequence */
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
    fsr_dispatch(P_PREP, src_x, src_y);
    fsr_dispatch(P_LUMA, spd_x, spd_y);
    fsr_dispatch(P_SCPYR, spd_x, spd_y);
    fsr_dispatch(P_SC, sc_x, sc_y);
    fsr_dispatch(P_REACT, src_x, src_y);
    fsr_dispatch(P_INST, src_x, src_y);
    fsr_dispatch(P_ACC, src_x, src_y);              /* upscale size = render size */
    F.res_index = (F.res_index + 1) % 16;

    /* the result into the scene's colour */
    vt_layout(vc, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageSubresourceLayers sl = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    if (F.blit) {
        VkImageBlit b; memset(&b, 0, sizeof b);
        b.srcSubresource = b.dstSubresource = sl; b.srcOffsets[1].x = b.dstOffsets[1].x = w; b.srcOffsets[1].y = b.dstOffsets[1].y = h; b.srcOffsets[1].z = b.dstOffsets[1].z = 1;
        vkCmdBlitImage(R.cmd, F.img[FI_OUT].vt->img, VK_IMAGE_LAYOUT_GENERAL, vc->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
    } else {
        VkImageCopy c; memset(&c, 0, sizeof c);
        c.srcSubresource = c.dstSubresource = sl; c.extent.width = (uint32_t)w; c.extent.height = (uint32_t)h; c.extent.depth = 1;
        vkCmdCopyImage(R.cmd, F.img[FI_OUT].vt->img, VK_IMAGE_LAYOUT_GENERAL, vc->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    game_fsr_w = w; game_fsr_h = h;
    if (F.runs++ == 0) printf("nvn_vk_fsr: first run at %dx%d (scene depth %dx%d) before a %s draw, jitter %.3f, %.3f px\n", w, h, vd->w, vd->h, fx == FX_DOWN ? "bloom downsample" : "tonemap",
                              (double)F.k.jitterOffset[0], (double)F.k.jitterOffset[1]);
    else if (F.runs % 1800 == 0) printf("nvn_vk_fsr: %lld runs, %lld history resets, jitter %.3f, %.3f px\n", (long long)F.runs, (long long)F.resets, (double)F.k.jitterOffset[0], (double)F.k.jitterOffset[1]);
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
    if (F.ran == F.frame || !F.depth || F.depth_frame != F.frame || F.cam_frame != F.frame || !fs || !R.prog[1] || F.depth->dead) return;
    int fx = stage_fx(&R.prog[1]->st[1]);
    if (fx != FX_GRAIN && fx != FX_DOWN) return;
    for (int i = 0; i < fs->nsmp; i++) {
        unsigned un = fs->smp[i].unit;
        Tex *t = NULL; View view; SamplerParams sp;
        if (un >= 32 || !(nvn_front_handle(R.tpool, R.spool, R.nvn_tex[1][un], &t, &view, &sp) & 1) || !t || t->dead || !t->vk) continue;
        if (t != c && t->b.width == F.depth->b.width && t->b.height == F.depth->b.height && fsr_hdr(t->vk) && ((VTex *)t->vk)->samples == 1) { fsr_run(t, F.depth, fx); return; }
    }
}
/* nvn_vk_draw.c, at shutdown (the GPU is idle; the images went with the others) */
static void fsr_shutdown(void) {
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
    F.frame++;
}
