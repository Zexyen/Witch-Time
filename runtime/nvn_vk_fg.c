/* [witchtime] issue 119: frame generation. A generated picture is shown between every two frames of the game:
 *   frame N is finished -> the picture between N-1 and N goes to the window at N's tick -> N itself half a frame later.
 * So the window gets two presents a game frame (nvn_vk_draw.c vk_present2 runs its swapchain part twice, the generated
 * picture first; the real frame is kept in g_pend[1] and a timer thread here posts its present to the render thread).
 * The game's own clock and tick do not change; the real frame is on screen half a frame later than without this.
 *
 * frame_generation = on with frame_generator = fsr | dlss (graphics.ini; WT_FRAMEGEN = off | on | fsr | dlss wins, and has
 * test: the real frame twice, to see the pacing).
 * The generators are the PLAYER'S DLLs, as with DLSS and FSR 4 (nothing of AMD's or NVIDIA's is here or in a release):
 *   fsr:  amd_fidelityfx_loader_vk.dll + amd_fidelityfx_framegeneration_vk.dll (AMD FSR SDK 2.4.0), through AMD's MIT
 *         headers (third_party/ffx_api). Only AMD's frame generation context is used, with our own images: NOT its
 *         frame-generation swapchain (four queues and a present thread of its own, against display.c's pacing).
 *   dlss: nvngx_dlss.dll + nvngx_dlssg.dll, through the driver's NGX runtime as nvn_vk_dlss.c does (feature 11). NVIDIA
 *         publishes no interface for this feature outside Streamline: the parameter names are those the open project
 *         dlssg-to-fsr3 (GPL-3.0, Nukem9) reads in NVIDIA's place. A GUESS until an RTX 40 owner runs it.
 * Both need what an upscaler mode makes each frame (motion vectors, the jitter, the camera): frame generation runs only
 * in frames where fsr_run ran (upscaler not off, a 3D scene). Menus and movies are presented as before.
 *
 * NOT RUN with either DLL (2026-11-16, written on Linux). The pacing was run with WT_FRAMEGEN=test.
 *
 * THE PICTURE WITHOUT THE HUD (fg_draw): in a frame with a scene the game's first draw into the window texture is its last
 * post pass (FXAA from the tone map's target); the HUD's draws follow into the same texture. Before the second draw the
 * window texture is copied to G.hud. Both generators take it: they work out the in-between picture from it and put the
 * HUD of the real frame on top, so the HUD does not move or smear. A scene frame where no second draw came (no HUD on
 * screen) gives the finished picture as its own HUD-less one. frame_generation_hud = off (WT_FG_HUD=0): not handed over.
 * Not done: HDR output takes the 8-bit picture. */
enum { FG_OFF, FG_FSR, FG_DLSS, FG_TEST };
void nvn_front_post_fg_flush(void);                /* nvn_gl.c */
double display_refresh_hz(void);                   /* display.c */
static struct {
    FImg out, hud; int w, h; VkFormat fmt;         /* the generated picture, and the frame without its HUD (fg_draw): the window texture's size and format */
    Tex *win; LONG64 win_frame, hud_frame; int win_draws;      /* the window texture drawn into in F.frame = win_frame, the draws into it so far; hud_frame: G.hud is of that frame */
    int said;                                      /* bit per reason: said once */
    LONG64 scene_frame; int scene_w, scene_h, scene_reset;      /* fsr_run of F.frame = scene_frame left its depth and vectors (fg_scene) */
    uint64_t id; LONG64 last, runs, shown;         /* id: AMD's frame number; last: F.frame of the last generated picture */
    LONG64 t_prev, half;                           /* pacing: the last present job, half a game frame (QPC ticks) */
    CRITICAL_SECTION lock; CONDITION_VARIABLE cv; HANDLE thread; LONG64 due; int armed;
} G;
static int fg_mode(void) { return gfx_options()->frame_gen; }
static int fg_hud_on(void) { static int e = -2; if (e == -2) { const char *s = getenv("WT_FG_HUD"); e = s ? atoi(s) != 0 : -1; } return e >= 0 ? e : !gfx_options()->frame_gen_no_hud; }
static void fg_copy(VTex *from, VTex *to) {        /* the whole image; both are left in their transfer layouts */
    VkImageCopy c; memset(&c, 0, sizeof c);
    c.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.srcSubresource.layerCount = 1; c.dstSubresource = c.srcSubresource;
    c.extent.width = (uint32_t)from->w; c.extent.height = (uint32_t)from->h; c.extent.depth = 1;
    pass_end();
    vt_layout(from, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL); vt_layout(to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    cmd_begin();
    vkCmdCopyImage(R.cmd, from->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}
/* nvn_vk_draw.c, before every draw (render thread): the window texture without the HUD, see the top */
static void fg_draw(VStage *fs) {
    static int log = -1;
    Tex *c = R.ncol ? R.col[0].tex : NULL;
    if (!gfx_options()->frame_gen || !c || F.ran != F.frame || !wintex_is(c) || c->dead || !c->vk) return;      /* (F.ran: fsr_run ran in this frame, so it has a scene) */
    if (G.win_frame != F.frame || G.win != c) { G.win_frame = F.frame; G.win = c; G.win_draws = 0; }
    if (log < 0) { const char *e = getenv("WT_FG_HUDLOG"); log = e ? atoi(e) : 0; }      /* TEST: the draws into the window texture of that many scene frames */
    if (log > 0 && G.win_draws < 6) { printf("nvn_vk_fg: hudlog: frame %lld, draw %d into the window texture: fragment stage %u (%d samplers)\n", (long long)F.frame, G.win_draws, fs ? fs->id : 0u, fs ? fs->nsmp : 0); if (G.win_draws == 5) log--; }
    if (G.win_draws++ != 1) return;
    VTex *vt = c->vk;
    if (!G.hud.vt || G.w != vt->w || G.h != vt->h || G.fmt != vt->fmt || vt->samples != 1) return;      /* (made by fg_make, for the window texture's size) */
    fg_copy(vt, G.hud.vt);
    G.hud_frame = F.frame;
}
static void fg_say(int bit, const char *why) { if (!(G.said >> bit & 1)) { G.said |= 1 << bit; printf("nvn_vk_fg: no generated picture: %s\n", why); } }
/* the camera of this frame's scene, back from fsr_run's constants: deviceToViewDepth = {-q, q * near, .., tan(fovy / 2)}, q = far / (near - far) */
static void fg_camera(float *zn, float *zf, float *fov) {
    float q = -F.k.deviceToViewDepth[0];
    *zn = q != 0 ? F.k.deviceToViewDepth[1] / q : 0.1f; *zf = 1.0f + q != 0 ? q * *zn / (1.0f + q) : 5000.0f;
    if (!(*zn > 0) || !(*zf > *zn)) { *zn = 0.1f; *zf = 5000.0f; }
    *fov = 2.0f * atanf(F.k.deviceToViewDepth[3]);
}

#ifdef _WIN32
#include "third_party/ffx_api/framegeneration/include/ffx_framegeneration.h"
/* ---- fsr: AMD's frame generation context (the loader and fsr4_res are nvn_vk_fsr4.c's) */
static struct {
    int ok;                                        /* 0 not tried, 1 ready, -1 not available (logged once) */
    ffxContext c; struct ffxCreateContextDescFrameGeneration cd; struct ffxCreateContextDescFrameGenerationVersion ver; struct ffxCreateBackendVKDesc vk;
    int dw, dh, rw, rh; VkFormat fmt;              /* what the context was created for */
    LONG64 prep;                                   /* F.frame of the last prepare */
} GA;
static void fga_off(const char *why, unsigned r) {
    if (GA.ok >= 0) { if (r) printf("nvn_vk_fg: FSR frame generation is off: %s (AMD's result %u)\n", why, r); else printf("nvn_vk_fg: FSR frame generation is off: %s\n", why); }
    GA.ok = -1;
}
static void fga_msg(uint32_t type, const wchar_t *m) { static int n; if (n++ < 32) printf("nvn_vk_fg: AMD's %s: %ls\n", type ? "warning" : "error", m); }
/* the context for the window texture's size (G.w, G.h, G.fmt: known after the first present) and this scene size.
 * Called from fga_prepare, before anything of this frame uses a context: an old one goes once the GPU is idle. */
static int fga_context(int rw, int rh) {
    if (GA.ok < 0 || !G.w) return 0;
    if (GA.c && GA.dw == G.w && GA.dh == G.h && GA.fmt == G.fmt && rw <= GA.rw && rh <= GA.rh) return 1;
    if (!GA.ok) {
        const char *why = fsr4_load();
        if (why) { fga_off(why, 0); return 0; }
        if (!fsr4_file(A4.dir, L"amd_fidelityfx_framegeneration_vk.dll")) { fga_off("amd_fidelityfx_framegeneration_vk.dll is not beside amd_fidelityfx_loader_vk.dll", 0); return 0; }
    }
    if (GA.c) { vkDeviceWaitIdle(V.device); A4.destroy(&GA.c, NULL); GA.c = NULL; }
    struct FfxApiResource probe; int bad = 0; VTex pv; memset(&pv, 0, sizeof pv); pv.fmt = G.fmt; probe = fsr4_res(&pv, 0, &bad);
    if (bad) { fga_off("the window picture's format has no name in AMD's interface", (unsigned)bad); return 0; }
    memset(&GA.cd, 0, sizeof GA.cd); memset(&GA.ver, 0, sizeof GA.ver); memset(&GA.vk, 0, sizeof GA.vk);
    GA.cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION; GA.cd.header.pNext = &GA.ver.header;
    GA.cd.flags = 0;                               /* an 8-bit picture, depth 0 near with a far plane, vectors at the scene's size without jitter */
    GA.cd.displaySize.width = (uint32_t)G.w; GA.cd.displaySize.height = (uint32_t)G.h;
    if (rw < GA.rw) rw = GA.rw;
    if (rh < GA.rh) rh = GA.rh;
    GA.cd.maxRenderSize.width = (uint32_t)rw; GA.cd.maxRenderSize.height = (uint32_t)rh; GA.cd.backBufferFormat = probe.description.format;
    GA.ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION; GA.ver.header.pNext = &GA.vk.header; GA.ver.version = FFX_FRAMEGENERATION_VERSION;
    GA.vk.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    GA.vk.deviceContext.vkDevice = V.device; GA.vk.deviceContext.vkPhysicalDevice = V.gpu; GA.vk.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    ffxReturnCode_t r = A4.create(&GA.c, &GA.cd.header, NULL);
    if (r != FFX_API_RETURN_OK || !GA.c) { GA.c = NULL; fga_off("AMD's frame generation context was not created (the GPU or driver may not have it)", r); return 0; }
    struct ffxConfigureDescGlobalDebug dbg; memset(&dbg, 0, sizeof dbg);
    dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG; dbg.effectId = FFX_API_EFFECT_ID_FRAMEGENERATION; dbg.fpMessage = fga_msg; dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    A4.configure(NULL, &dbg.header);
    struct ffxQueryGetProviderVersion pq; memset(&pq, 0, sizeof pq); pq.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    A4.query(&GA.c, &pq.header);
    GA.ok = 1; GA.dw = G.w; GA.dh = G.h; GA.fmt = G.fmt; GA.rw = rw; GA.rh = rh; GA.prep = 0;
    printf("nvn_vk_fg: AMD frame generation %s ready: pictures of %dx%d (Vulkan format %d), scenes up to %dx%d\n", pq.versionName ? pq.versionName : "(version not told)", G.w, G.h, (int)G.fmt, rw, rh);
    return 1;
}
/* fg_scene (inside fsr_run, the scene's depth read-only, the vectors written): AMD's per-frame setup and its prepare pass */
static void fga_prepare(VTex *vd, int w, int h, int reset) {
    if (!fga_context(w, h)) return;
    VTex *mv = F.img[FI_MV].vt;
    int bad = 0;
    G.id++;
    struct ffxConfigureDescFrameGeneration cf; memset(&cf, 0, sizeof cf);
    cf.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION; cf.frameGenerationEnabled = true;      /* no swapchain, no callbacks: the dispatch below is ours */
    cf.generationRect.width = GA.dw; cf.generationRect.height = GA.dh; cf.frameID = G.id;
    if (fg_hud_on() && G.hud.vt && G.hud.vt->fmt == GA.fmt) { cf.HUDLessColor = fsr4_res(G.hud.vt, FFX_API_RESOURCE_STATE_COMPUTE_READ, &bad); cf.HUDLessColor.description.usage &= ~(uint32_t)FFX_API_RESOURCE_USAGE_UAV; }      /* the image is ours for good; its picture of this frame is copied before AMD's pass reads it (fg_draw, fg_make) */
    ffxReturnCode_t r = A4.configure(&GA.c, &cf.header);
    if (r != FFX_API_RETURN_OK) { fga_off("AMD's frame generation took no setup without its swapchain", r); return; }
    struct ffxDispatchDescFrameGenerationPrepareV2 p; memset(&p, 0, sizeof p);
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2; p.frameID = G.id; p.commandList = R.cmd;
    p.renderSize.width = (uint32_t)w; p.renderSize.height = (uint32_t)h;
    p.jitterOffset.x = F.k.jitterOffset[0]; p.jitterOffset.y = F.k.jitterOffset[1]; p.motionVectorScale.x = (float)w; p.motionVectorScale.y = (float)h;      /* as FSR 4 takes them (nvn_vk_fsr4.c) */
    p.frameTimeDelta = F.k.deltaTime * 1000.0f; p.reset = reset != 0; p.viewSpaceToMetersFactor = 1.0f;
    fg_camera(&p.cameraNear, &p.cameraFar, &p.cameraFovAngleVertical);
    vt_layout(mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    p.depth = fsr4_res(vd, FFX_API_RESOURCE_STATE_COMPUTE_READ, &bad); p.motionVectors = fsr4_res(mv, FFX_API_RESOURCE_STATE_COMPUTE_READ, &bad);
    p.motionVectors.description.usage &= ~(uint32_t)FFX_API_RESOURCE_USAGE_UAV;
    {   const float *m = F.c3;                     /* c3: the view matrix in rows [0..15], its inverse in [48..63] (fsr_run) */
        p.cameraRight[0] = m[0]; p.cameraRight[1] = m[1]; p.cameraRight[2] = m[2]; p.cameraUp[0] = m[4]; p.cameraUp[1] = m[5]; p.cameraUp[2] = m[6];
        p.cameraForward[0] = -m[8]; p.cameraForward[1] = -m[9]; p.cameraForward[2] = -m[10]; p.cameraPosition[0] = m[51]; p.cameraPosition[1] = m[55]; p.cameraPosition[2] = m[59]; }
    r = bad ? FFX_API_RETURN_ERROR_PARAMETER : A4.dispatch(&GA.c, &p.header);
    R.pipe_bound = NULL; dsh_forget(); R.dyn_dirty = R.vi_dirty = R.dyn3_dirty = 1;      /* AMD leaves bindings of its own in the command buffer */
    vt_layout(mv, VK_IMAGE_LAYOUT_GENERAL);
    if (r != FFX_API_RETURN_OK) { fga_off("AMD's prepare pass failed", r); return; }
    GA.prep = F.frame;
}
/* fg_make: the picture between the last frame and `vt` into `out`. 0 = none. */
static int fga_run(VTex *vt, VTex *out, int reset) {
    int bad = 0;
    if (GA.ok != 1 || !GA.c || GA.prep != G.scene_frame || GA.dw != vt->w || GA.dh != vt->h || GA.fmt != vt->fmt) return 0;      /* (a new window size: the context follows at the next scene) */
    struct ffxDispatchDescFrameGeneration d; memset(&d, 0, sizeof d);
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION; d.commandList = R.cmd;
    vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); vt_layout(out, VK_IMAGE_LAYOUT_GENERAL); vt_layout(G.hud.vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    d.presentColor = fsr4_res(vt, FFX_API_RESOURCE_STATE_COMPUTE_READ, &bad); d.presentColor.description.usage &= ~(uint32_t)FFX_API_RESOURCE_USAGE_UAV;
    d.outputs[0] = fsr4_res(out, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS, &bad); d.outputs[0].description.usage |= FFX_API_RESOURCE_USAGE_RENDERTARGET;
    d.numGeneratedFrames = 1; d.reset = reset != 0; d.backbufferTransferFunction = FFX_API_BACKBUFFER_TRANSFER_FUNCTION_SRGB; d.minMaxLuminance[1] = 200.0f;
    d.generationRect.width = vt->w; d.generationRect.height = vt->h; d.frameID = G.id;
    ffxReturnCode_t r = bad ? FFX_API_RETURN_ERROR_PARAMETER : A4.dispatch(&GA.c, &d.header);
    R.pipe_bound = NULL; dsh_forget(); R.dyn_dirty = R.vi_dirty = R.dyn3_dirty = 1;
    if (r != FFX_API_RETURN_OK) { fga_off("AMD's frame generation pass failed", r); return 0; }
    return 1;
}
static void fga_shutdown(void) { if (GA.c) A4.destroy(&GA.c, NULL); memset(&GA, 0, sizeof GA); }

/* ---- dlss: NVIDIA's frame generation feature through NGX (nvn_vk_dlss.c has the runtime: DL) */
static struct {
    int ok;                                        /* 0 not tried, 1 ready, -1 not available (logged once) */
    NgxParam *par; NgxHandle *h; int w, h_; VkFormat fmt;
    float proj[16];                                /* "DLSSG.CameraViewToClip" points here */
} GD;
static void fgd_off(const char *why, NgxResult r) {
    if (GD.ok >= 0) { if (r) printf("nvn_vk_fg: DLSS frame generation is off: %s (NGX result 0x%08x)\n", why, r); else printf("nvn_vk_fg: DLSS frame generation is off: %s\n", why); }
    GD.ok = -1;
}
static int fgd_run(Tex *t, VTex *vt, VTex *out, int reset) {
    if (GD.ok < 0) return 0;
    if (!GD.ok) {
        wchar_t p[MAX_PATH + 24];
        if (DL.ok < 0 || (DL.ok == 0 && !dlss_setup())) { fgd_off("NGX is not running here (it needs an NVIDIA RTX GPU and nvngx_dlss.dll beside witch-time.exe: see the nvn_vk_dlss lines)", 0); return 0; }
        _snwprintf(p, MAX_PATH + 24, L"%ls\\nvngx_dlssg.dll", DL.dir); p[MAX_PATH + 23] = 0;
        if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) { fgd_off("no nvngx_dlssg.dll beside nvngx_dlss.dll (copy it from a game that has DLSS frame generation)", 0); return 0; }
        NgxParam *cap = NULL; int have = -1;
        if (DL.caps(&cap) == 1 && cap) { ngx_get_i(cap, "FrameGeneration.Available", &have); DL.destroy(cap); }
        printf("nvn_vk_fg: NGX says of frame generation: available = %d (-1: it did not say)\n", have);
        NgxResult r = DL.alloc(&GD.par);
        if (r != 1 || !GD.par) { fgd_off("NGX gave no parameters", r); return 0; }
        GD.ok = 1;
    }
    VTex *vd = F.depth ? F.depth->vk : NULL, *mv = F.img[FI_MV].vt;
    if (!vd || !mv) return 0;
    if (!GD.h || GD.w != vt->w || GD.h_ != vt->h || GD.fmt != vt->fmt) {
        if (GD.h) { vkDeviceWaitIdle(V.device); DL.release(GD.h); GD.h = NULL; }      /* (a new window size: rare) */
        ngx_set_u(GD.par, "CreationNodeMask", 1); ngx_set_u(GD.par, "VisibilityNodeMask", 1);
        ngx_set_u(GD.par, "Width", (unsigned)vt->w); ngx_set_u(GD.par, "Height", (unsigned)vt->h);
        ngx_set_u(GD.par, "DLSSG.BackbufferFormat", (unsigned)vt->fmt);
        NgxResult r = DL.create1(V.device, R.cmd, 11, GD.par, &GD.h);      /* feature 11: frame generation */
        if (r != 1 || !GD.h) { GD.h = NULL; fgd_off("the frame generation feature was not created (RTX 40 or newer, and a driver that has it)", r); return 0; }
        GD.w = vt->w; GD.h_ = vt->h; GD.fmt = vt->fmt; reset = 1;
        printf("nvn_vk_fg: DLSS frame generation feature made for %dx%d (Vulkan format %d)\n", vt->w, vt->h, (int)vt->fmt);
    }
    EView e;
    eview_of(t, vt, NULL, &e); e.lvl0 = 0; e.nlvl = 1; e.dsmode = 0; e.swz[0] = 2; e.swz[1] = 3; e.swz[2] = 4; e.swz[3] = 5;
    VkImageView bview = vt_view(vt, &e, DIM_2D, 0);
    vt_layout(vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); vt_layout(vd, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    vt_layout(mv, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL); vt_layout(out, VK_IMAGE_LAYOUT_GENERAL);
    NgxRes rb, rd, rm, ro, rh;
    memset(&rb, 0, sizeof rb);
    rb.range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; rb.range.levelCount = 1; rb.range.layerCount = 1;
    rd = rm = ro = rh = rb;
    vt_layout(G.hud.vt, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rh.view = G.hud.lvl[0]; rh.image = G.hud.vt->img; rh.format = G.hud.vt->fmt; rh.width = (unsigned)G.hud.vt->w; rh.height = (unsigned)G.hud.vt->h;
    rb.view = bview; rb.image = vt->img; rb.format = vt->fmt; rb.width = (unsigned)vt->w; rb.height = (unsigned)vt->h;
    rd.view = F.depth_view; rd.image = vd->img; rd.format = vd->fmt; rd.width = (unsigned)vd->w; rd.height = (unsigned)vd->h; rd.range.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    rm.view = F.img[FI_MV].lvl[0]; rm.image = mv->img; rm.format = mv->fmt; rm.width = (unsigned)mv->w; rm.height = (unsigned)mv->h;
    ro.view = G.out.lvl[0]; ro.image = out->img; ro.format = out->fmt; ro.width = (unsigned)out->w; ro.height = (unsigned)out->h; ro.rw = 1;
    float zn, zf, fov;
    fg_camera(&zn, &zf, &fov);
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) GD.proj[r * 4 + c] = F.c3[16 + c * 4 + r];      /* ours transposed: NVIDIA's rows are for row vectors */
    GD.proj[8] = GD.proj[9] = 0.0f;                /* without our jitter (the projection's [0][2], [1][2]) */
    ngx_set_p(GD.par, "DLSSG.Backbuffer", &rb); ngx_set_p(GD.par, "DLSSG.Depth", &rd); ngx_set_p(GD.par, "DLSSG.MVecs", &rm); ngx_set_p(GD.par, "DLSSG.OutputInterpolated", &ro);
    ngx_set_p(GD.par, "DLSSG.HUDLess", fg_hud_on() ? &rh : NULL); ngx_set_p(GD.par, "DLSSG.OutputReal", NULL);
    ngx_set_u(GD.par, "DLSSG.HUDLessSubrectWidth", (unsigned)G.hud.vt->w); ngx_set_u(GD.par, "DLSSG.HUDLessSubrectHeight", (unsigned)G.hud.vt->h);
    ngx_set_p(GD.par, "DLSSG.CameraViewToClip", GD.proj);
    ngx_set_u(GD.par, "DLSSG.EnableInterp", 1); ngx_set_u(GD.par, "DLSSG.IsRecording", 1); ngx_set_u(GD.par, "DLSSG.MultiFrameCount", 1); ngx_set_u(GD.par, "DLSSG.MultiFrameIndex", 1);
    ngx_set_u(GD.par, "DLSSG.DepthSubrectWidth", (unsigned)G.scene_w); ngx_set_u(GD.par, "DLSSG.DepthSubrectHeight", (unsigned)G.scene_h);
    ngx_set_u(GD.par, "DLSSG.MVecsSubrectWidth", (unsigned)G.scene_w); ngx_set_u(GD.par, "DLSSG.MVecsSubrectHeight", (unsigned)G.scene_h);
    ngx_set_u(GD.par, "DLSSG.MvecJittered", 0); ngx_set_u(GD.par, "DLSSG.MvecDilated", 0); ngx_set_u(GD.par, "DLSSG.ColorBuffersHDR", 0); ngx_set_u(GD.par, "DLSSG.DepthInverted", 0);
    ngx_set_u(GD.par, "DLSSG.OrthoProjection", 0); ngx_set_u(GD.par, "DLSSG.Reset", reset != 0);
    ngx_set_f(GD.par, "DLSSG.MvecScaleX", (float)G.scene_w); ngx_set_f(GD.par, "DLSSG.MvecScaleY", (float)G.scene_h);      /* ours are in UV, to the previous position */
    ngx_set_f(GD.par, "DLSSG.JitterOffsetX", F.k.jitterOffset[0]); ngx_set_f(GD.par, "DLSSG.JitterOffsetY", F.k.jitterOffset[1]);
    ngx_set_f(GD.par, "DLSSG.CameraFOV", fov); ngx_set_f(GD.par, "DLSSG.CameraNear", zn); ngx_set_f(GD.par, "DLSSG.CameraFar", zf);
    NgxResult r = DL.evaluate(R.cmd, GD.h, GD.par, NULL);
    R.pipe_bound = NULL; dsh_forget(); R.dyn_dirty = R.vi_dirty = R.dyn3_dirty = 1;      /* NGX leaves bindings of its own in the command buffer */
    vt_layout(mv, VK_IMAGE_LAYOUT_GENERAL);
    if (r != 1) { fgd_off("a frame generation call failed", r); return 0; }
    return 1;
}
static void fgd_shutdown(void) { if (GD.ok == 1) { if (GD.h) DL.release(GD.h); if (GD.par) DL.destroy(GD.par); } memset(&GD, 0, sizeof GD); }      /* before dlss_shutdown ends NGX */
#else
static void fga_prepare(VTex *vd, int w, int h, int reset) { (void)vd; (void)w; (void)h; (void)reset; }
static int fga_run(VTex *vt, VTex *out, int reset) { (void)vt; (void)out; (void)reset; fg_say(8, "AMD ships its frame generation DLLs for Windows only"); return 0; }
static int fgd_run(Tex *t, VTex *vt, VTex *out, int reset) { (void)t; (void)vt; (void)out; (void)reset; fg_say(9, "this build has DLSS frame generation on Windows only"); return 0; }
static void fga_shutdown(void) {}
static void fgd_shutdown(void) {}
#endif

/* ---- the host */
/* fsr_run, once the frame's motion vectors are written and with the scene's depth read-only */
static void fg_scene(VTex *vd, int w, int h, int reset) {
    if (!fg_mode()) return;
    G.scene_frame = F.frame; G.scene_w = w; G.scene_h = h; G.scene_reset = reset;
    if (fg_mode() == FG_FSR) fga_prepare(vd, w, h, reset);
}
/* the timer thread: at G.due it posts the present of the kept real frame to the render thread */
static DWORD WINAPI fg_timer(LPVOID arg) {
    (void)arg;
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    for (;;) {
        EnterCriticalSection(&G.lock);
        while (!G.armed) SleepConditionVariableCS(&G.cv, &G.lock, INFINITE);
        LONG64 due = G.due; G.armed = 0;
        LeaveCriticalSection(&G.lock);
        for (;;) {
            LONG64 left = due - tk();
            if (left <= 0) break;
            DWORD ms = (DWORD)(left * 1000 / fq.QuadPart);
            Sleep(ms > 1 ? ms - 1 : 0);                /* the last millisecond in short yields: Sleep(1) can take two */
        }
        nvn_front_post_fg_flush();
    }
    return 0;
}
/* render thread: the generated picture went to the window now; the real frame follows half a game frame later */
static void fg_arm(void) {
    if (!G.thread) { InitializeCriticalSection(&G.lock); InitializeConditionVariable(&G.cv); G.thread = CreateThread(NULL, 0, fg_timer, NULL, 0, NULL); }
    EnterCriticalSection(&G.lock); G.due = tk() + G.half; G.armed = 1; LeaveCriticalSection(&G.lock);
    WakeConditionVariable(&G.cv);
    G.shown++;
}
/* vk_present2, render thread, before the swapchain part: the picture to show BEFORE the frame `vt` (the game's window
 * texture `t`), or NULL: frame generation off, no scene in this frame, or the generator could not. */
static VTex *fg_make(Tex *t, VTex *vt, LONG64 now) {
    int mode = fg_mode();
    LARGE_INTEGER fq; QueryPerformanceFrequency(&fq);
    {   /* half a game frame, from the time between the last two present jobs (60 fps: 8.3 ms), within 240 .. 20 fps */
        LONG64 d = G.t_prev ? now - G.t_prev : fq.QuadPart / 60;
        if (d < fq.QuadPart / 240) d = fq.QuadPart / 240;
        if (d > fq.QuadPart / 20) d = fq.QuadPart / 20;
        G.half = d / 2; G.t_prev = now;
    }
    if (!mode) { if (G.out.vt) { vt_garbage(G.out.vt); vt_garbage(G.hud.vt); memset(&G.out, 0, sizeof G.out); memset(&G.hud, 0, sizeof G.hud); G.w = 0; } return NULL; }
    if (G.w != vt->w || G.h != vt->h || G.fmt != vt->fmt || !G.out.vt) {
        if (G.out.vt) { vt_garbage(G.out.vt); vt_garbage(G.hud.vt); }
        memset(&G.out, 0, sizeof G.out); memset(&G.hud, 0, sizeof G.hud);
        cmd_begin();
        fsr_img(&G.hud, vt->fmt, vt->w, vt->h, 1, 0); G.hud.vt->layout = VK_IMAGE_LAYOUT_GENERAL; G.hud_frame = 0;
        fsr_img(&G.out, vt->fmt, vt->w, vt->h, 1, 2 | (fmt_supported(vt->fmt, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) ? 1 : 0));
        G.out.vt->storage &= 1;                     /* (VTex.storage as fsr4_res reads it: made with storage usage) */
        G.out.vt->layout = VK_IMAGE_LAYOUT_GENERAL;
        G.w = vt->w; G.h = vt->h; G.fmt = vt->fmt; G.last = 0;
        printf("nvn_vk_fg: frame generation (%s): pictures of %dx%d, Vulkan format %d, present mode %s\n", mode == FG_FSR ? "fsr" : mode == FG_DLSS ? "dlss" : "test", vt->w, vt->h, (int)vt->fmt, V.mode >= 0 && V.mode < 4 ? k_mode[V.mode] : "?");
    }
    if (mode != FG_TEST && G.scene_frame != F.frame - 1) return NULL;      /* fsr_run did not run in this frame: a menu, a movie, upscaler = off (F.frame was counted on by fsr_present) */
    if (mode != FG_TEST && !gfx_options()->upscaler) { fg_say(0, "it needs an upscaler mode (upscaler = fsr3 or any other but off): the motion vectors come from there"); return NULL; }
    {   /* two presents a game frame through a present mode that waits for the display: the display must show twice the game's rate */
        double hz = display_refresh_hz();
        if ((V.mode == VK_PRESENT_MODE_FIFO_KHR || V.mode == VK_PRESENT_MODE_FIFO_RELAXED_KHR) && hz > 0 && hz < 1.9 * (double)fq.QuadPart / (double)(2 * G.half)) {
            fg_say(1, "the display shows fewer than twice the game's frames a second and the present mode waits for it (the game would be slowed down): a 120 Hz display for 60 fps, or vsync = off");
            return NULL;
        }
    }
    VTex *out = G.out.vt;
    int reset = G.last != F.frame - 1 || G.scene_reset, ok;
    cmd_begin();
    if (G.hud_frame != F.frame - 1) fg_copy(vt, G.hud.vt);      /* no second draw into the window texture in this frame: nothing lay over the scene */
    {   /* TEST ONLY: WT_FG_HUDSHOT=<frame>: that frame and its picture without the HUD as two BMP files in WT_SHOT_DIR. The
         * screenshot reads the game's own textures only, so the second goes through the window texture: that one frame is
         * shown without its HUD. */
        static int shot = -2;
        if (shot == -2) { const char *e = getenv("WT_FG_HUDSHOT"); shot = e ? atoi(e) : -1; }
        if (shot > 0 && F.frame >= shot) {
            char path[300]; const char *dir = getenv("WT_SHOT_DIR");
            snprintf(path, sizeof path, "%s%sfg_frame_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)F.frame); save_bmp_vk(path, vt);
            fg_copy(G.hud.vt, vt);
            snprintf(path, sizeof path, "%s%sfg_hudless_%lld.bmp", dir ? dir : "", dir ? "/" : "", (long long)F.frame); save_bmp_vk(path, vt);
            printf("nvn_vk_fg: TEST: the frame and its picture without the HUD saved at F.frame %lld (HUD-less copy of this frame: %s)\n", (long long)F.frame, G.hud_frame == F.frame - 1 ? "yes" : "no, the whole frame");
            shot = -1;
        }
    }
    if (mode == FG_TEST) {                         /* the real frame once more: only the pacing is new */
        fg_copy(vt, out);
        ok = 1;
    } else ok = mode == FG_FSR ? fga_run(vt, out, reset) : fgd_run(t, vt, out, reset);
    if (!ok) return NULL;
    out->rs = vt->rs; out->sx = vt->sx; out->sy = vt->sy;      /* shown through the same crop and reduction as the frame */
    G.last = F.frame;
    if (G.runs++ == 0) printf("nvn_vk_fg: first generated picture (%s), the real frame follows %.2f ms later\n", mode == FG_FSR ? "fsr" : mode == FG_DLSS ? "dlss" : "test", (double)G.half * 1000.0 / (double)fq.QuadPart);
    else if (G.runs % 3600 == 0) printf("nvn_vk_fg: %lld generated pictures, %lld shown ahead of their frame\n", (long long)G.runs, (long long)G.shown);
    return out;
}
static void fg_shutdown(void) { fga_shutdown(); fgd_shutdown(); }      /* fsr_shutdown, before DLSS and FSR 4 go: the GPU is idle (the images go with the others) */
