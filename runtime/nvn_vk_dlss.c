/* [witchtime] issue 190: NVIDIA DLSS / DLAA in place of FSR's own passes (nvn_vk_fsr.c fsr_run), with the PLAYER'S copy of
 * nvngx_dlss.dll. Nothing of NVIDIA's is in this repository or in a release: no SDK header, no SDK library, no DLL. The
 * calls below go to the NGX runtime the NVIDIA driver installs (_nvngx.dll / nvngx.dll), which loads the player's
 * nvngx_dlss.dll from the folder it is told. The declarations are our own, written from the interface's public facts.
 *
 * upscaler = dlaa | dlss_quality | dlss_balanced | dlss_performance | dlss_ultra_performance: the same scales, jitter,
 * motion vectors and output image as the FSR mode of the same rank (fsr_ratio), so when DLSS cannot run here (no NVIDIA
 * GPU, no DLL, an old driver, a failed call) fsr_run goes on with FSR's passes and nothing else changes.
 *
 * The player puts nvngx_dlss.dll beside witch-time(.exe) (the folder above this program's) or beside this program.
 * Windows only (and Proton, which provides nvngx.dll): the Linux NGX runtime takes another library, not looked at.
 *
 * NOT RUN ON AN NVIDIA GPU YET (2026-11-12): written on an AMD machine. WT_DLSS_JSIGN (bit 0: x, bit 1: y) flips the
 * jitter's sign and WT_DLSS_MVSIGN=1 the motion vectors' for the first test builds. */
#ifdef _WIN32
#include <wchar.h>
typedef struct NgxParam { void **vt; } NgxParam;      /* NVIDIA's C++ parameter interface as MSVC lays it out: each overload set in reverse order of declaration */
typedef struct NgxHandle NgxHandle;
typedef unsigned int NgxResult;                    /* 1 = success, 0xBAD000xx = failure */
static void ngx_set_p(NgxParam *p, const char *n, void *v) { ((void (*)(NgxParam *, const char *, void *))p->vt[0])(p, n, v); }
static void ngx_set_i(NgxParam *p, const char *n, int v) { ((void (*)(NgxParam *, const char *, int))p->vt[3])(p, n, v); }
static void ngx_set_u(NgxParam *p, const char *n, unsigned v) { ((void (*)(NgxParam *, const char *, unsigned))p->vt[4])(p, n, v); }
static void ngx_set_f(NgxParam *p, const char *n, float v) { ((void (*)(NgxParam *, const char *, float))p->vt[6])(p, n, v); }
static NgxResult ngx_get_i(NgxParam *p, const char *n, int *v) { return ((NgxResult (*)(NgxParam *, const char *, int *))p->vt[11])(p, n, v); }
typedef struct { VkImageView view; VkImage image; VkImageSubresourceRange range; VkFormat format; unsigned int width, height; uint32_t type; unsigned char rw; } NgxRes;      /* an image view resource (type 0); rw: it has storage usage */
typedef struct { const wchar_t *const *path; unsigned int n; void *internal; void *log_cb; int log_level; unsigned char log_only; } NgxCommon;      /* the folders searched for the feature's DLL; logging off */
_Static_assert(sizeof(NgxRes) == 56 && sizeof(NgxCommon) == 40, "NGX structures");

static struct {
    int ok, ext;                                   /* ok: 0 not tried, 1 ready, -1 not available (logged once); ext: the device was made with what NGX needs */
    wchar_t dir[MAX_PATH], exe[MAX_PATH];          /* the folder of the player's nvngx_dlss.dll; this program's */
    HMODULE core;
    NgxResult (*init)(const char *, int, const char *, const wchar_t *, VkInstance, VkPhysicalDevice, VkDevice, void *, void *, int, const NgxCommon *);      /* Init_ProjectID_Ext */
    NgxResult (*init9)(const char *, int, const char *, const wchar_t *, VkInstance, VkPhysicalDevice, VkDevice, int, const NgxCommon *);      /* Init_ProjectID: no loader functions */
    NgxResult (*shutdown1)(VkDevice, unsigned int *);
    NgxResult (*caps)(NgxParam **); NgxResult (*alloc)(NgxParam **); NgxResult (*destroy)(NgxParam *);
    NgxResult (*create1)(VkDevice, VkCommandBuffer, int, NgxParam *, NgxHandle **);
    NgxResult (*release)(NgxHandle *);
    NgxResult (*evaluate)(VkCommandBuffer, const NgxHandle *, const NgxParam *, void *);
    NgxParam *par; NgxHandle *h;
    int w, h_, uw, uh, mode; VkFormat fmt;         /* what the feature was created for */
    struct { NgxHandle *h; LONG64 frame; } old[8]; /* replaced features: released a few frames later (commands not yet run may name them) */
    LONG64 runs, made;
} DL;

static void dlss_off(const char *why, NgxResult r) {
    if (DL.ok >= 0) { if (r) printf("nvn_vk_dlss: DLSS is off, FSR 3.1 takes its place: %s (NGX result 0x%08x)\n", why, r); else printf("nvn_vk_dlss: DLSS is off, FSR 3.1 takes its place: %s\n", why); }
    DL.ok = -1;
}
static int dlss_file(const wchar_t *dir) { wchar_t p[MAX_PATH + 20]; _snwprintf(p, MAX_PATH + 20, L"%ls\\nvngx_dlss.dll", dir); p[MAX_PATH + 19] = 0; return GetFileAttributesW(p) != INVALID_FILE_ATTRIBUTES; }
static int dlss_find(void) {                       /* the player's DLL: beside the launcher (the folder above), else beside this program */
    if (DL.dir[0]) return 1;
    DWORD n = GetModuleFileNameW(NULL, DL.exe, MAX_PATH);
    if (!n || n >= MAX_PATH) return 0;
    wchar_t *s = wcsrchr(DL.exe, L'\\');
    if (!s) return 0;
    *s = 0;
    wchar_t up[MAX_PATH];
    wcscpy(up, DL.exe);
    s = wcsrchr(up, L'\\');
    if (s) { *s = 0; if (dlss_file(up)) { wcscpy(DL.dir, up); return 1; } }
    if (dlss_file(DL.exe)) { wcscpy(DL.dir, DL.exe); return 1; }
    return 0;
}
/* nvn_vk_draw.c vkd_device_info: the device extensions NGX needs, only on an NVIDIA GPU with the player's DLL in place
 * (nobody else's device changes) */
static void dlss_device_exts(const VkExtensionProperties *dext, uint32_t ndext) {
    if (V.props.vendorID != 0x10de || !dlss_find()) return;
    if (!has_name(dext, ndext, "VK_NVX_binary_import") || !has_name(dext, ndext, "VK_NVX_image_view_handle")) { printf("nvn_vk_dlss: nvngx_dlss.dll found, but the driver has no VK_NVX_binary_import / VK_NVX_image_view_handle\n"); return; }
    D.exts[D.next++] = "VK_NVX_binary_import"; D.exts[D.next++] = "VK_NVX_image_view_handle";
    DL.ext = 1;
    printf("nvn_vk_dlss: nvngx_dlss.dll found in %ls: the device is made with VK_NVX_binary_import and VK_NVX_image_view_handle\n", DL.dir);
}
static HMODULE dlss_core(void) {                   /* the driver's NGX runtime: System32 (Proton puts it there too), else the folder the driver names in the registry */
    static const wchar_t *const k[2] = {L"_nvngx.dll", L"nvngx.dll"};
    for (int i = 0; i < 2; i++) { HMODULE m = LoadLibraryExW(k[i], NULL, LOAD_LIBRARY_SEARCH_SYSTEM32); if (m) return m; }
    wchar_t d[MAX_PATH], p[MAX_PATH + 16]; DWORD n = sizeof d;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"System\\CurrentControlSet\\Services\\nvlddmkm\\NGXCore", L"NGXPath", RRF_RT_REG_SZ, NULL, d, &n) != ERROR_SUCCESS) return NULL;
    for (int i = 0; i < 2; i++) { _snwprintf(p, MAX_PATH + 16, L"%ls\\%ls", d, k[i]); p[MAX_PATH + 15] = 0; HMODULE m = LoadLibraryExW(p, NULL, LOAD_WITH_ALTERED_SEARCH_PATH); if (m) return m; }
    return NULL;
}
static int dlss_setup(void) {                      /* render thread, once */
    if (V.props.vendorID != 0x10de) { dlss_off("this is not an NVIDIA GPU", 0); return 0; }
    if (!dlss_find()) { dlss_off("no nvngx_dlss.dll beside witch-time.exe (copy it there from a game that has DLSS, then start again)", 0); return 0; }
    if (!DL.ext || !D.has_push || !D.f12.bufferDeviceAddress) { dlss_off("the device lacks an extension NGX needs", 0); return 0; }
    DL.core = dlss_core();
    if (!DL.core) { dlss_off("the NVIDIA driver's NGX runtime (_nvngx.dll) was not found", 0); return 0; }
#define N_(f, name) DL.f = (void *)GetProcAddress(DL.core, "NVSDK_NGX_VULKAN_" name); if (!DL.f) { dlss_off("the driver's NGX runtime is too old (no NVSDK_NGX_VULKAN_" name ")", 0); return 0; }
    /* the core's Init_ProjectID takes NO loader functions (read in the driver's _nvngx.dll 620.70: 9 arguments, the version
     * 8th); the form with them is Init_ProjectID_Ext. Calling the first with 11 crashed inside NGX (issue 190). */
    DL.init = (void *)GetProcAddress(DL.core, "NVSDK_NGX_VULKAN_Init_ProjectID_Ext");
    N_(init9, "Init_ProjectID") N_(shutdown1, "Shutdown1") N_(caps, "GetCapabilityParameters") N_(alloc, "AllocateParameters") N_(destroy, "DestroyParameters")
    N_(create1, "CreateFeature1") N_(release, "ReleaseFeature") N_(evaluate, "EvaluateFeature")
#undef N_
    const wchar_t *paths[1] = {DL.dir};
    NgxCommon ci; memset(&ci, 0, sizeof ci);
    ci.path = paths; ci.n = 1;
    /* a project id of our own (any GUID), engine "custom"; this program's folder for NGX's own files; interface version 0x15 */
    static const char k_id[] = "b3f1a0c4-7d2e-4c59-9a61-3e8f5d2c7b10";
    NgxResult r = DL.init ? DL.init(k_id, 0, "1.0", DL.exe, V.instance, V.gpu, V.device, (void *)vkGetInstanceProcAddr, (void *)vkGetDeviceProcAddr, 0x15, &ci)
                          : DL.init9(k_id, 0, "1.0", DL.exe, V.instance, V.gpu, V.device, 0x15, &ci);
    printf("nvn_vk_dlss: NGX started with %s: result 0x%08x\n", DL.init ? "Init_ProjectID_Ext" : "Init_ProjectID", r);
    if (r != 1) { dlss_off("NGX did not start", r); return 0; }
    NgxParam *cap = NULL; int have = 0, need_drv = 0, why = 0;
    r = DL.caps(&cap);
    if (r != 1 || !cap) { dlss_off("NGX gave no capabilities", r); return 0; }
    ngx_get_i(cap, "SuperSampling.Available", &have); ngx_get_i(cap, "SuperSampling.NeedsUpdatedDriver", &need_drv); ngx_get_i(cap, "SuperSampling.FeatureInitResult", &why);
    DL.destroy(cap);
    if (!have) { dlss_off(need_drv ? "the NVIDIA driver is too old for this nvngx_dlss.dll" : "NGX says DLSS is not available on this GPU (an RTX card is needed)", (NgxResult)why); return 0; }
    r = DL.alloc(&DL.par);
    if (r != 1 || !DL.par) { dlss_off("NGX gave no parameters", r); return 0; }
    DL.ok = 1;
    printf("nvn_vk_dlss: NGX ready, DLSS available (the player's nvngx_dlss.dll in %ls)\n", DL.dir);
    return 1;
}
static void dlss_retire(void) {                    /* the feature in use goes to the list of the replaced */
    if (!DL.h) return;
    int k = 0;
    for (int i = 0; i < 8; i++) { if (!DL.old[i].h) { k = i; break; } if (DL.old[i].frame < DL.old[k].frame) k = i; }
    if (DL.old[k].h) { vkDeviceWaitIdle(V.device); DL.release(DL.old[k].h); }      /* full (8 sizes within 4 frames): the oldest goes now */
    DL.old[k].h = DL.h; DL.old[k].frame = F.frame; DL.h = NULL;
}
/* fsr_run, after the motion vectors (FI_MV) and with the scene's colour and depth as F.col_view / F.depth_view: DLSS writes
 * FI_OUT. Returns 0 when it did not (fsr_run then runs FSR's passes). */
static int dlss_run(VTex *vc, VTex *vd, int w, int h, int uw, int uh, int reset) {
    static const int k_quality[5] = {5, 2, 1, 0, 3};      /* DLAA, quality, balanced, performance, ultra performance */
    static int jsign = -1, mvsign;
    if (DL.ok < 0 || (DL.ok == 0 && !dlss_setup())) return 0;
    VTex *mv = F.img[FI_MV].vt, *out = F.img[FI_OUT].vt;
    if (jsign < 0) { const char *e = getenv("WT_DLSS_JSIGN"); jsign = e ? atoi(e) & 3 : 0; e = getenv("WT_DLSS_MVSIGN"); mvsign = e && atoi(e); }
    if (!DL.h || DL.w != w || DL.h_ != h || DL.uw != uw || DL.uh != uh || DL.mode != g_rs_mode || DL.fmt != out->fmt) {
        dlss_retire();
        ngx_set_u(DL.par, "CreationNodeMask", 1); ngx_set_u(DL.par, "VisibilityNodeMask", 1);
        ngx_set_u(DL.par, "Width", (unsigned)w); ngx_set_u(DL.par, "Height", (unsigned)h); ngx_set_u(DL.par, "OutWidth", (unsigned)uw); ngx_set_u(DL.par, "OutHeight", (unsigned)uh);
        ngx_set_i(DL.par, "PerfQualityValue", k_quality[g_rs_mode - 6]);
        ngx_set_i(DL.par, "DLSS.Feature.Create.Flags", 1 | 2 | 64);      /* HDR colour, motion vectors at the scene's size, exposure worked out by DLSS (depth 0 near: not inverted; vectors without jitter) */
        ngx_set_i(DL.par, "DLSS.Enable.Output.Subrects", 0);
        NgxResult r = DL.create1(V.device, R.cmd, 1, DL.par, &DL.h);
        if (r != 1 || !DL.h) { DL.h = NULL; dlss_off("the DLSS feature was not created", r); return 0; }
        DL.w = w; DL.h_ = h; DL.uw = uw; DL.uh = uh; DL.mode = g_rs_mode; DL.fmt = out->fmt; reset = 1;
        if (DL.made++ < 8 || DL.made % 64 == 0) printf("nvn_vk_dlss: feature %lld made: %dx%d to %dx%d, quality value %d, output format %d\n", (long long)DL.made, w, h, uw, uh, k_quality[g_rs_mode - 6], (int)out->fmt);
    }
    vt_layout(mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);      /* NGX: inputs read-only, the output general (as FI_OUT is) */
    NgxRes rc, rd, rm, ro;
    memset(&rc, 0, sizeof rc);
    rc.range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rc.range.levelCount = 1; rc.range.layerCount = 1;
    rd = rm = ro = rc;
    rc.view = F.col_view; rc.image = vc->img; rc.format = vc->fmt; rc.width = (unsigned)w; rc.height = (unsigned)h;
    rd.view = F.depth_view; rd.image = vd->img; rd.format = vd->fmt; rd.width = (unsigned)w; rd.height = (unsigned)h; rd.range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    rm.view = F.img[FI_MV].lvl[0]; rm.image = mv->img; rm.format = mv->fmt; rm.width = (unsigned)w; rm.height = (unsigned)h;
    ro.view = F.img[FI_OUT].lvl[0]; ro.image = out->img; ro.format = out->fmt; ro.width = (unsigned)uw; ro.height = (unsigned)uh; ro.rw = 1;
    ngx_set_p(DL.par, "Color", &rc); ngx_set_p(DL.par, "Depth", &rd); ngx_set_p(DL.par, "MotionVectors", &rm); ngx_set_p(DL.par, "Output", &ro);
    ngx_set_f(DL.par, "Jitter.Offset.X", jsign & 1 ? -F.k.jitterOffset[0] : F.k.jitterOffset[0]);      /* pixels, as FSR takes them */
    ngx_set_f(DL.par, "Jitter.Offset.Y", jsign & 2 ? -F.k.jitterOffset[1] : F.k.jitterOffset[1]);
    ngx_set_f(DL.par, "MV.Scale.X", mvsign ? -(float)w : (float)w); ngx_set_f(DL.par, "MV.Scale.Y", mvsign ? -(float)h : (float)h);      /* ours are in UV, to the previous position */
    ngx_set_u(DL.par, "DLSS.Render.Subrect.Dimensions.Width", (unsigned)w); ngx_set_u(DL.par, "DLSS.Render.Subrect.Dimensions.Height", (unsigned)h);
    ngx_set_f(DL.par, "Sharpness", 0.0f); ngx_set_f(DL.par, "DLSS.Pre.Exposure", 1.0f); ngx_set_f(DL.par, "DLSS.Exposure.Scale", 1.0f);
    ngx_set_i(DL.par, "Reset", reset);
    NgxResult r = DL.evaluate(R.cmd, DL.h, DL.par, NULL);
    R.pipe_bound = NULL; dsh_forget(); R.dyn_dirty = R.vi_dirty = R.dyn3_dirty = 1;      /* NGX leaves bindings of its own in the command buffer */
    vt_layout(mv, VK_IMAGE_LAYOUT_GENERAL);
    if (r != 1) { dlss_off("a DLSS frame failed", r); return 0; }
    fsr_barrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
    if (DL.runs++ == 0) printf("nvn_vk_dlss: first DLSS frame, %dx%d to %dx%d, jitter %.3f, %.3f px\n", w, h, uw, uh, (double)F.k.jitterOffset[0], (double)F.k.jitterOffset[1]);
    return 1;
}
static void dlss_present(void) {                   /* fsr_present: replaced features whose frames are done; the one in use when DLSS was turned off */
    if (DL.ok != 1) return;
    if (DL.h && g_rs_mode < 6) dlss_retire();
    for (int i = 0; i < 8; i++) if (DL.old[i].h && F.frame - DL.old[i].frame > 4) { DL.release(DL.old[i].h); DL.old[i].h = NULL; }
}
static void dlss_shutdown(void) {                  /* fsr_shutdown: the GPU is idle */
    if (DL.ok != 1) return;
    unsigned int left = 0;
    if (DL.h) DL.release(DL.h);
    for (int i = 0; i < 8; i++) if (DL.old[i].h) DL.release(DL.old[i].h);
    DL.destroy(DL.par); DL.shutdown1(V.device, &left);
    memset(&DL, 0, sizeof DL);
}
#else
static void dlss_device_exts(const VkExtensionProperties *dext, uint32_t ndext) { (void)dext; (void)ndext; }
static int dlss_run(VTex *vc, VTex *vd, int w, int h, int uw, int uh, int reset) {
    static int said; (void)vc; (void)vd; (void)w; (void)h; (void)uw; (void)uh; (void)reset;
    if (!said++) printf("nvn_vk_dlss: DLSS is off, FSR 3.1 takes its place: this build has DLSS on Windows only\n");
    return 0;
}
static void dlss_present(void) {}
static void dlss_shutdown(void) {}
#endif
