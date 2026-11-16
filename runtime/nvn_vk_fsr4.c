/* [witchtime] AMD FSR 4 in place of FSR 3.1's own passes (nvn_vk_fsr.c fsr_run), with the PLAYER'S copy of AMD's Vulkan
 * DLLs: amd_fidelityfx_loader_vk.dll and amd_fidelityfx_upscaler_vk.dll (FSR SDK 2.4.0 or newer; in the SDK's
 * Kits/FidelityFX/signedbin, or in a Vulkan game that has FSR 4). No DLL of AMD's is in this repository or in a release:
 * their licence is binary-only and this program is GPL. The interface is AMD's own MIT headers (third_party/ffx_api).
 * NOT amdxcffx64.dll: that one is the driver's DirectX 12 library, which a Vulkan renderer cannot call.
 *
 * upscaler = fsr4 | fsr4_quality | fsr4_balanced | fsr4_performance | fsr4_ultra_performance (11 .. 15): the same scales,
 * jitter, motion vectors and output image as the FSR 3.1 mode of the same rank (fsr_ratio), so when FSR 4 cannot run here
 * (not an AMD GPU, no DLLs, an old driver, a failed call) fsr_run goes on with FSR 3.1's passes and nothing else changes.
 *
 * The player puts both DLLs beside witch-time(.exe) (the folder above this program's) or beside this program.
 * Windows only. AMD: Radeon RX 6000 or newer and Adrenalin 26.10.1 or newer for FSR 4 on Vulkan.
 *
 * NOT RUN YET (2026-11-16): written on Linux, where AMD ships no FSR 4. */
#ifdef _WIN32
#include <wchar.h>
#include "third_party/ffx_api/api/include/vk/ffx_api_vk.h"
#include "third_party/ffx_api/upscalers/include/ffx_upscale.h"

typedef struct Fsr4Ctx {                           /* a context and what it was created from (AMD: the descriptions stay alive until it is destroyed) */
    ffxContext c;
    struct ffxCreateContextDescUpscale up; struct ffxCreateContextDescUpscaleVersion ver; struct ffxCreateBackendVKDesc vk; struct ffxOverrideVersion ov;
    LONG64 frame;                                  /* when it was replaced */
} Fsr4Ctx;
static struct {
    int ok, ext;                                   /* ok: 0 not tried, 1 ready, -1 not available (logged once); ext: the device was made for FSR 4 */
    wchar_t dir[MAX_PATH];                         /* the folder of the player's DLLs */
    HMODULE lib;
    PfnFfxCreateContext create; PfnFfxDestroyContext destroy; PfnFfxQuery query; PfnFfxDispatch dispatch;
    uint64_t ver; char name[32];                   /* the FSR 4 version the DLL offers for this device */
    Fsr4Ctx *cur, *old[8];                         /* old: replaced contexts, destroyed a few frames later (commands not yet run may use them) */
    int w, h, uw, uh, hdr;                         /* the largest sizes the context in use was created for */
    int msgs; LONG64 runs, made;
} A4;
/* the features AMD's sample turns on for the FSR 4 models beyond the 1.1 - 1.3 ones this device already has whole:
 * VkPhysicalDevice{CooperativeMatrixFeaturesKHR, ShaderFloat8FeaturesEXT, ComputeShaderDerivativesFeaturesKHR}, two
 * VkBool32 each (declared here: the Vulkan headers of a build may be older than the second) */
static struct { VkStructureType sType; void *pNext; VkBool32 f[2]; } g_fsr4_feat[3];

static void fsr4_off(const char *why, unsigned r) {
    if (A4.ok >= 0) { if (r) printf("nvn_vk_fsr4: FSR 4 is off, FSR 3.1 takes its place: %s (AMD's result %u)\n", why, r); else printf("nvn_vk_fsr4: FSR 4 is off, FSR 3.1 takes its place: %s\n", why); }
    A4.ok = -1;
}
static int fsr4_file(const wchar_t *dir, const wchar_t *name) { wchar_t p[MAX_PATH + 48]; _snwprintf(p, MAX_PATH + 48, L"%ls\\%ls", dir, name); p[MAX_PATH + 47] = 0; return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int fsr4_find(void) {                       /* the player's loader DLL: beside the launcher (the folder above), else beside this program */
    wchar_t exe[MAX_PATH], up[MAX_PATH];
    if (A4.dir[0]) return 1;
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wchar_t *s = wcsrchr(exe, L'\\');
    if (!s) return 0;
    *s = 0;
    wcscpy(up, exe);
    s = wcsrchr(up, L'\\');
    if (s) { *s = 0; if (fsr4_file(up, L"amd_fidelityfx_loader_vk.dll")) { wcscpy(A4.dir, up); return 1; } }
    if (fsr4_file(exe, L"amd_fidelityfx_loader_vk.dll")) { wcscpy(A4.dir, exe); return 1; }
    return 0;
}
/* nvn_vk_draw.c vkd_device_info, before the features are read: the extensions of the FSR 4 models, only on an AMD GPU
 * with the player's DLLs in place (nobody else's device changes). Returns the end of the feature chain. */
static void **fsr4_device_exts(const VkExtensionProperties *dext, uint32_t ndext, void **tail) {
    static const char *const k_name[3] = {"VK_KHR_cooperative_matrix", "VK_EXT_shader_float8", "VK_KHR_compute_shader_derivatives"};
    static const uint32_t k_stype[3] = {1000506000u, 1000567000u, 1000201000u};
    int n = 0;
    if (V.props.vendorID != 0x1002 || !fsr4_find()) return tail;
    for (int i = 0; i < 3; i++) if (has_name(dext, ndext, k_name[i])) {
        D.exts[D.next++] = k_name[i]; g_fsr4_feat[i].sType = (VkStructureType)k_stype[i]; *tail = &g_fsr4_feat[i]; tail = &g_fsr4_feat[i].pNext; n++;
    }
    A4.ext = 1;
    printf("nvn_vk_fsr4: amd_fidelityfx_loader_vk.dll found in %ls: the device is made with %d of the 3 extensions of the FSR 4 models (cooperative matrix, 8-bit floats, compute derivatives)\n", A4.dir, n);
    return tail;
}
static void fsr4_msg(uint32_t type, const wchar_t *m) { if (A4.msgs++ < 32) printf("nvn_vk_fsr4: AMD's %s: %ls\n", type ? "warning" : "error", m); }
static int fsr4_setup(void) {                      /* render thread, once */
    wchar_t p[MAX_PATH + 48];
    if (V.props.vendorID != 0x1002) { fsr4_off("this is not an AMD GPU", 0); return 0; }
    if (!fsr4_find()) { fsr4_off("no amd_fidelityfx_loader_vk.dll beside witch-time.exe (copy it and amd_fidelityfx_upscaler_vk.dll there from AMD's FSR SDK 2.4.0 or a Vulkan game that has FSR 4, then start again)", 0); return 0; }
    if (!fsr4_file(A4.dir, L"amd_fidelityfx_upscaler_vk.dll")) { fsr4_off("amd_fidelityfx_upscaler_vk.dll is not beside amd_fidelityfx_loader_vk.dll (FSR 4 is in that one)", 0); return 0; }
    if (!A4.ext) { fsr4_off("the device was made before the DLLs were found", 0); return 0; }
    _snwprintf(p, MAX_PATH + 48, L"%ls\\amd_fidelityfx_loader_vk.dll", A4.dir); p[MAX_PATH + 47] = 0;
    A4.lib = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);      /* (its folder first: the loader takes the upscaler DLL from beside itself) */
    if (!A4.lib) { fsr4_off("amd_fidelityfx_loader_vk.dll did not load", (unsigned)GetLastError()); return 0; }
    A4.create = (PfnFfxCreateContext)(void *)GetProcAddress(A4.lib, "ffxCreateContext"); A4.destroy = (PfnFfxDestroyContext)(void *)GetProcAddress(A4.lib, "ffxDestroyContext");
    A4.query = (PfnFfxQuery)(void *)GetProcAddress(A4.lib, "ffxQuery"); A4.dispatch = (PfnFfxDispatch)(void *)GetProcAddress(A4.lib, "ffxDispatch");
    if (!A4.create || !A4.destroy || !A4.query || !A4.dispatch) { fsr4_off("amd_fidelityfx_loader_vk.dll has no ffxCreateContext / ffxDestroyContext / ffxQuery / ffxDispatch", 0); return 0; }
    /* the versions the DLL (or the driver through it) offers for THIS device: FSR 4 is in the list only where it can run */
    VkDeviceContext dc = {V.device, V.gpu, vkGetDeviceProcAddr};
    uint64_t n = 0, ids[16]; const char *names[16]; char all[256]; size_t at = 0;
    struct ffxQueryDescGetVersions q; memset(&q, 0, sizeof q);
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS; q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; q.device = &dc; q.outputCount = &n;
    ffxReturnCode_t r = A4.query(NULL, &q.header);
    if (r != FFX_API_RETURN_OK || !n) { fsr4_off("AMD's library lists no upscaler for this device", r); return 0; }
    if (n > 16) n = 16;
    q.versionIds = ids; q.versionNames = names;
    r = A4.query(NULL, &q.header);
    if (r != FFX_API_RETURN_OK) { fsr4_off("AMD's library lists no upscaler for this device", r); return 0; }
    all[0] = 0;
    for (uint64_t i = 0; i < n; i++) {
        if (!names[i]) continue;
        if (at < sizeof all) at += (size_t)snprintf(all + at, sizeof all - at, "%s%s", at ? ", " : "", names[i]);
        if (!A4.ver && names[i][0] == '4') { A4.ver = ids[i]; snprintf(A4.name, sizeof A4.name, "%s", names[i]); }      /* the first 4.x: the newest (the name is copied: AMD may reuse its memory) */
    }
    if (!A4.ver) { printf("nvn_vk_fsr4: versions offered here: %s\n", all); fsr4_off("AMD's library offers no FSR 4 for this GPU and driver (Radeon RX 6000 or newer, Adrenalin 26.10.1 or newer)", 0); return 0; }
    A4.ok = 1;
    printf("nvn_vk_fsr4: FSR %s ready (the player's DLLs in %ls; versions offered: %s)\n", A4.name, A4.dir, all);
    return 1;
}
static void fsr4_retire(void) {                    /* the context in use goes to the list of the replaced */
    if (!A4.cur) return;
    int k = 0;
    for (int i = 0; i < 8; i++) { if (!A4.old[i]) { k = i; break; } if (A4.old[i]->frame < A4.old[k]->frame) k = i; }
    if (A4.old[k]) { vkDeviceWaitIdle(V.device); A4.destroy(&A4.old[k]->c, NULL); free(A4.old[k]); }      /* full: the oldest goes now */
    A4.old[k] = A4.cur; A4.cur->frame = F.frame; A4.cur = NULL;
}
/* an image as AMD's Vulkan backend takes it (ffx_api_vk.h ffxApiGetImageResourceDescriptionVK, which is C++ only) */
static struct FfxApiResource fsr4_res(const VTex *vt, uint32_t state, int *bad) {
    struct FfxApiResource r; memset(&r, 0, sizeof r);
    uint32_t f = FFX_API_SURFACE_FORMAT_UNKNOWN, usage = FFX_API_RESOURCE_USAGE_READ_ONLY;
    switch (vt->fmt) {
    case VK_FORMAT_B10G11R11_UFLOAT_PACK32: f = FFX_API_SURFACE_FORMAT_R11G11B10_FLOAT; break;
    case VK_FORMAT_R16G16B16A16_SFLOAT: f = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT; break;
    case VK_FORMAT_R8G8B8A8_UNORM: f = FFX_API_SURFACE_FORMAT_R8G8B8A8_UNORM; break;
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: f = FFX_API_SURFACE_FORMAT_R10G10B10A2_UNORM; break;
    case VK_FORMAT_R16G16_SFLOAT: f = FFX_API_SURFACE_FORMAT_R16G16_FLOAT; break;
    case VK_FORMAT_D24_UNORM_S8_UINT: usage |= FFX_API_RESOURCE_USAGE_STENCILTARGET;      /* fall through */
    case VK_FORMAT_X8_D24_UNORM_PACK32: f = FFX_API_SURFACE_FORMAT_R32_UINT; usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET; break;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: usage |= FFX_API_RESOURCE_USAGE_STENCILTARGET;      /* fall through */
    case VK_FORMAT_D32_SFLOAT: f = FFX_API_SURFACE_FORMAT_R32_FLOAT; usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET; break;
    case VK_FORMAT_D16_UNORM: f = FFX_API_SURFACE_FORMAT_R16_UNORM; usage |= FFX_API_RESOURCE_USAGE_DEPTHTARGET; break;
    default: *bad = (int)vt->fmt; break;
    }
    if (vt->storage) usage |= FFX_API_RESOURCE_USAGE_UAV;
    r.resource = (void *)vt->img; r.state = state;
    r.description.type = FFX_API_RESOURCE_TYPE_TEXTURE2D; r.description.format = f; r.description.width = (uint32_t)vt->w; r.description.height = (uint32_t)vt->h;
    r.description.depth = 1; r.description.mipCount = 1; r.description.flags = FFX_API_RESOURCE_FLAGS_NONE; r.description.usage = usage;
    return r;
}
/* fsr_run, after the motion vectors (FI_MV) and with the scene's colour and depth read-only: FSR 4 writes FI_OUT.
 * Returns 0 when it did not (fsr_run then runs FSR 3.1's passes). */
static int fsr4_run(VTex *vc, VTex *vd, int w, int h, int uw, int uh, int reset) {
    if (A4.ok < 0 || (A4.ok == 0 && !fsr4_setup())) return 0;
    VTex *mv = F.img[FI_MV].vt, *out = F.img[FI_OUT].vt;
    int hdr = fsr_hdr(vc), bad = 0;
    /* one context for every size up to the largest seen (AMD's dynamic resolution): the game's own resolution levels and
     * a change of mode do not make a new one, which would stop the game while the models are set up */
    if (!A4.cur || w > A4.w || h > A4.h || uw > A4.uw || uh > A4.uh || hdr != A4.hdr) {
        int mw = w, mh = h, muw = uw, muh = uh;     /* the new context's limits: never below the last one's */
        if (A4.cur) { if (mw < A4.w) mw = A4.w; if (mh < A4.h) mh = A4.h; if (muw < A4.uw) muw = A4.uw; if (muh < A4.uh) muh = A4.uh; }
        fsr4_retire();
        Fsr4Ctx *x = calloc(1, sizeof *x);
        if (!x) { fsr4_off("no memory", 0); return 0; }
        x->up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE; x->up.header.pNext = &x->ver.header;
        x->up.flags = FFX_UPSCALE_ENABLE_AUTO_EXPOSURE | FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION | (hdr ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0);      /* depth 0 near, a far plane, vectors at the scene's size without jitter: no flag */
        x->up.maxRenderSize.width = (uint32_t)mw; x->up.maxRenderSize.height = (uint32_t)mh; x->up.maxUpscaleSize.width = (uint32_t)muw; x->up.maxUpscaleSize.height = (uint32_t)muh;
        x->up.fpMessage = fsr4_msg;
        x->ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION; x->ver.header.pNext = &x->vk.header; x->ver.version = FFX_UPSCALER_VERSION;
        x->vk.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK; x->vk.header.pNext = &x->ov.header;
        x->vk.deviceContext.vkDevice = V.device; x->vk.deviceContext.vkPhysicalDevice = V.gpu; x->vk.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
        x->ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION; x->ov.versionId = A4.ver;
        ffxReturnCode_t r = A4.create(&x->c, &x->up.header, NULL);
        if (r != FFX_API_RETURN_OK || !x->c) { free(x); fsr4_off("the FSR 4 context was not created", r); return 0; }
        A4.cur = x; A4.w = mw; A4.h = mh; A4.uw = muw; A4.uh = muh; A4.hdr = hdr; reset = 1;
        printf("nvn_vk_fsr4: context %lld made: FSR %s, up to %dx%d to %dx%d, %s colour\n", (long long)++A4.made, A4.name, mw, mh, muw, muh, hdr ? "HDR" : "SDR");
    }
    vt_layout(mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);      /* AMD: inputs read-only, the output general (as FI_OUT is) */
    struct ffxDispatchDescUpscale d; memset(&d, 0, sizeof d);
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE; d.commandList = R.cmd;
    d.color = fsr4_res(vc, FFX_API_RESOURCE_STATE_COMPUTE_READ, &bad); d.depth = fsr4_res(vd, FFX_API_RESOURCE_STATE_COMPUTE_READ, &bad);
    d.motionVectors = fsr4_res(mv, FFX_API_RESOURCE_STATE_COMPUTE_READ, &bad); d.output = fsr4_res(out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, &bad);
    d.motionVectors.description.usage &= ~(uint32_t)FFX_API_RESOURCE_USAGE_UAV;      /* read only here, whatever else we write it with */
    if (bad) { vt_layout(mv, VK_IMAGE_LAYOUT_GENERAL); printf("nvn_vk_fsr4: an image has Vulkan format %d, which AMD's interface has no name for\n", bad); fsr4_off("an image format AMD's interface cannot take", 0); return 0; }
    d.jitterOffset.x = F.k.jitterOffset[0]; d.jitterOffset.y = F.k.jitterOffset[1];      /* pixels, as FSR 3.1 takes them */
    d.motionVectorScale.x = (float)w; d.motionVectorScale.y = (float)h;      /* ours are in UV, to the previous position */
    d.renderSize.width = (uint32_t)w; d.renderSize.height = (uint32_t)h; d.upscaleSize.width = (uint32_t)uw; d.upscaleSize.height = (uint32_t)uh;
    d.frameTimeDelta = F.k.deltaTime * 1000.0f; d.preExposure = 1.0f; d.reset = reset != 0; d.viewSpaceToMetersFactor = 1.0f;
    {   /* the camera, back from fsr_run's constants: deviceToViewDepth = {-q, q * near, .., tan(fovy / 2)}, q = far / (near - far) */
        float q = -F.k.deviceToViewDepth[0], zn = q != 0 ? F.k.deviceToViewDepth[1] / q : 0.1f, zf = 1.0f + q != 0 ? q * zn / (1.0f + q) : 5000.0f;
        if (!(zn > 0) || !(zf > zn)) { zn = 0.1f; zf = 5000.0f; }
        d.cameraNear = zn; d.cameraFar = zf; d.cameraFovAngleVertical = 2.0f * atanf(F.k.deviceToViewDepth[3]);
    }
    ffxReturnCode_t r = A4.dispatch(&A4.cur->c, &d.header);
    R.pipe_bound = NULL; dsh_forget(); R.dyn_dirty = R.vi_dirty = R.dyn3_dirty = 1;      /* AMD leaves bindings of its own in the command buffer */
    vt_layout(mv, VK_IMAGE_LAYOUT_GENERAL);
    if (r != FFX_API_RETURN_OK) { fsr4_off("an FSR 4 frame failed", r); return 0; }
    fsr_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    if (A4.runs++ == 0) printf("nvn_vk_fsr4: first FSR 4 frame, %dx%d to %dx%d, jitter %.3f, %.3f px, near %.3f, far %.1f\n", w, h, uw, uh, (double)d.jitterOffset.x, (double)d.jitterOffset.y, (double)d.cameraNear, (double)d.cameraFar);
    return 1;
}
static void fsr4_present(void) {                   /* fsr_present: replaced contexts whose frames are done; the one in use when FSR 4 was turned off */
    if (A4.ok != 1) return;
    if (A4.cur && g_rs_mode < 11) fsr4_retire();
    for (int i = 0; i < 8; i++) if (A4.old[i] && F.frame - A4.old[i]->frame > 4) { A4.destroy(&A4.old[i]->c, NULL); free(A4.old[i]); A4.old[i] = NULL; }
}
static void fsr4_shutdown(void) {                  /* fsr_shutdown: the GPU is idle */
    if (A4.ok != 1) return;
    if (A4.cur) { A4.destroy(&A4.cur->c, NULL); free(A4.cur); }
    for (int i = 0; i < 8; i++) if (A4.old[i]) { A4.destroy(&A4.old[i]->c, NULL); free(A4.old[i]); }
    memset(&A4, 0, sizeof A4);                     /* (the DLL stays loaded) */
}
#else
static void **fsr4_device_exts(const VkExtensionProperties *dext, uint32_t ndext, void **tail) { (void)dext; (void)ndext; return tail; }
static int fsr4_run(VTex *vc, VTex *vd, int w, int h, int uw, int uh, int reset) {
    static int said; (void)vc; (void)vd; (void)w; (void)h; (void)uw; (void)uh; (void)reset;
    if (!said++) printf("nvn_vk_fsr4: FSR 4 is off, FSR 3.1 takes its place: AMD ships FSR 4 for Windows only\n");
    return 0;
}
static void fsr4_present(void) {}
static void fsr4_shutdown(void) {}
#endif
