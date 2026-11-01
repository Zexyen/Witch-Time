/* [witchtime] The PC settings menu on the screen (Vulkan; included by nvn_vk.c after nvn_vk_draw.c). pcmenu.c draws the panel into
 * BGRA pixels on the CPU when its content changes; here they are copied over the presented picture (after the game's frame, the
 * bars and the first-run text: never into the game's textures), at the size of the window.
 *   - nvn_vk_pcmenu_job: a job of the render thread that pcmenu.c posts about 60 times a second while the menu holds the game:
 *     the work the menu asks for (a sampler flush for anisotropic filtering / texture detail, the present mode for vsync, the images and stages of a new resolution scale), the panel's pixels
 *     into a staging buffer when they changed, then the game's last picture presented again with the panel (vk_present2's
 *     extra present, as the held loading screens do).
 *   - pcm_vk_draw: in every present while the menu is open (also the game's own presents before it is held); one flag read
 *     while it is closed.
 *   - SSBU_PCMENU_SHOT=<folder> (test aid): every panel change saves the next presented picture, panel included, as
 *     pcmenu_<n>.bmp (needs swapchain images with TRANSFER_SRC; the log says when they have none). */
int pcmenu_is_open(void), pcmenu_visible(void);
int pcmenu_panel_size(int W, int H, int r[4], unsigned *gen);      /* pcmenu.c: 0 = closed; r = x, y, w, h in the window's pixels */
void pcmenu_panel_read(void *dst, int w, int h);
int pcmenu_take_work(void);                                         /* 1 flush the samplers, 2 present mode again, 4 resolution scale */
static struct { VkBuffer buf; VMem mem; uint64_t size; unsigned gen; int r[4], ready, panel; } g_pcm;      /* panel: the pixels are the menu's, not the hint's */
static struct { VkBuffer buf; VMem mem; uint64_t size; int w, h, pending, n, said; } g_pcm_shot;

static void pcm_shot_write(void) {                 /* the picture copied by the last present (the GPU has finished: cmd_wait_all) */
    if (!g_pcm_shot.pending) return;
    g_pcm_shot.pending = 0;
    char path[300];
    snprintf(path, sizeof path, "%s/pcmenu_%03d.bmp", getenv("SSBU_PCMENU_SHOT"), g_pcm_shot.n++);
    BmpJob *j = malloc(sizeof *j);
    uint8_t *px = malloc((size_t)g_pcm_shot.w * g_pcm_shot.h * 4);
    if (!j || !px) { free(j); free(px); return; }
    memcpy(px, g_pcm_shot.mem.map, (size_t)g_pcm_shot.w * g_pcm_shot.h * 4);
    snprintf(j->path, sizeof j->path, "%s", path); j->px = px; j->w = g_pcm_shot.w; j->h = g_pcm_shot.h; j->bgr = 1;
    InterlockedIncrement(&g_bmp_pending);
    bmp_write_thread(j);
}
void nvn_vk_pcmenu_job(void *arg) {
    (void)arg;
    if (!V.alive) return;
    int work = pcmenu_take_work(), r[4];
    unsigned gen;
    if (work || g_pcm_shot.pending) cmd_wait_all();
    pcm_shot_write();
    if (work & 1) {      /* anisotropic filtering, texture_lod_bias: new samplers from now on; the old ones may still be named by cached descriptor sets */
        if (g_nsmp_old + g_nsmp <= (int)(sizeof g_smp_old / sizeof g_smp_old[0])) {
            for (int i = 0; i < g_nsmp; i++) g_smp_old[g_nsmp_old++] = g_smp[i].s;
            printf("pcmenu: %d samplers made again from now on (anisotropic_filtering %d, texture_lod_bias %.2f)\n", g_nsmp, gfx_options()->aniso, gfx_options()->texture_lod_bias);
            g_nsmp = 0;
        } else printf("pcmenu: too many samplers kept already: anisotropic_filtering applies to new samplers only (all at the next start)\n");
    }
    if (work & 2) { present_mode_pick(); if (V.swapchain) swapchain_destroy(); InterlockedIncrement64(&g_vk_recreated); }
    if (work & 4) rs_apply();
    if (pcmenu_panel_size((int)V.extent.width, (int)V.extent.height, r, &gen) && (gen != g_pcm.gen || !g_pcm.ready)) {
        uint64_t need = (uint64_t)r[2] * (uint64_t)r[3] * 4;
        if (!work && !g_pcm_shot.pending) cmd_wait_all();      /* the staging buffer may be read by a present in flight */
        if (need > g_pcm.size) {
            if (g_pcm.buf) { vkDestroyBuffer(V.device, g_pcm.buf, NULL); vmem_free(g_pcm.mem); }
            g_pcm.buf = vbuf_new(need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g_pcm.mem); g_pcm.size = need;
        }
        pcmenu_panel_read(g_pcm.mem.map, r[2], r[3]);
        memcpy(g_pcm.r, r, sizeof r); g_pcm.gen = gen; g_pcm.ready = 1; g_pcm.panel = pcmenu_is_open();
        if (getenv("SSBU_PCMENU_SHOT")) g_pcm_shot.pending = -1;       /* the next present */
    }
    if (g_pcm_last.win && pcmenu_is_open()) vk_present2(&g_pcm_last, 1);      /* (the hint alone rides on the game's presents) */
}
static void pcm_vk_draw(VkImage tgt) {
    if (!pcmenu_visible() || !g_pcm.ready || g_pcm.panel != pcmenu_is_open()) return;      /* (not the other one's pixels before the next job)*/
    int W = (int)V.extent.width, H = (int)V.extent.height, *r = g_pcm.r;
    if (r[0] < 0 || r[1] < 0 || r[0] + r[2] > W || r[1] + r[3] > H) return;      /* made for another window size: the next job makes it again */
    VkBufferImageCopy c;
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1;
    c.imageOffset.x = r[0]; c.imageOffset.y = r[1]; c.imageExtent.width = (uint32_t)r[2]; c.imageExtent.height = (uint32_t)r[3]; c.imageExtent.depth = 1;
    xfer_wait();                                   /* over the blit of the game's picture */
    vkCmdCopyBufferToImage(R.cmd, g_pcm.buf, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    if (g_pcm_shot.pending == -1) {
        g_pcm_shot.pending = 0;
        if (V.hdr || !g_sc_src) { if (!g_pcm_shot.said++) printf("pcmenu: SSBU_PCMENU_SHOT: the swapchain images cannot be read (no TRANSFER_SRC, or HDR): no picture\n"); return; }
        uint64_t need = (uint64_t)W * H * 4;
        if (need > g_pcm_shot.size) {
            if (g_pcm_shot.buf) { vkDestroyBuffer(V.device, g_pcm_shot.buf, NULL); vmem_free(g_pcm_shot.mem); }
            g_pcm_shot.buf = vbuf_new(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g_pcm_shot.mem); g_pcm_shot.size = need;
        }
        image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        c.imageOffset.x = c.imageOffset.y = 0; c.imageExtent.width = (uint32_t)W; c.imageExtent.height = (uint32_t)H;
        vkCmdCopyImageToBuffer(R.cmd, tgt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_pcm_shot.buf, 1, &c);
        image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        g_pcm_shot.w = W; g_pcm_shot.h = H; g_pcm_shot.pending = 1;      /* written by the next job, after its wait */
    }
}
static void pcm_vk_free(void) {
    if (g_pcm.buf) { vkDestroyBuffer(V.device, g_pcm.buf, NULL); vmem_free(g_pcm.mem); g_pcm.buf = VK_NULL_HANDLE; }
    if (g_pcm_shot.buf) { vkDestroyBuffer(V.device, g_pcm_shot.buf, NULL); vmem_free(g_pcm_shot.mem); g_pcm_shot.buf = VK_NULL_HANDLE; }
}
