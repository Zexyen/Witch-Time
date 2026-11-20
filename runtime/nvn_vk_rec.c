/* [witchtime] issue 257 (WT_VK_REC=1; off by default: a test until it has run on other drivers): a second thread records
 * the per-draw Vulkan commands. A quarter of the render thread's time in a ride is the driver's, most of it in the draw
 * calls and the binds before them. With this on, those calls (the "hot" ones below) are written into a queue and a
 * recording thread makes them, in order, while the render thread prepares the next draw. Every other command-buffer call
 * first waits until the queue is empty (rc_flush) and is then made by the calling thread itself: Vulkan asks only that one
 * thread at a time records into the buffers of a pool. Code that hands the command buffer to someone else (DLSS, FSR 4,
 * frame generation) calls rc_flush too. Measured in docs/STATUS.md. Included by nvn_vk_draw.c after its function pointers. */
enum { RC_WRAP, RC_G1, RC_G2, RC_PIPE, RC_VIN, RC_VB, RC_PUSH, RC_SETS, RC_PC, RC_IB, RC_DRAW, RC_DRAWX, RC_IND, RC_INDX, RC_INDC, RC_INDXC, RC_VP, RC_SC };
#define RC_BYTES (8u << 20)
typedef struct { uint32_t op, len; } RcHdr;
typedef void (VKAPI_PTR *RcG1)(VkCommandBuffer, uint32_t);
typedef void (VKAPI_PTR *RcG2)(VkCommandBuffer, uint32_t, uint32_t);
static uint8_t *g_rc_buf;
/* Byte counts that only grow. Each side's words are on a cache line of their own: the writer hands entries over once per
 * draw (rc_pub), and the recording thread reads `head` again only when it has caught up, so the lines seldom change hands. */
static struct {
    _Alignas(64) uint64_t head;                             /* written and handed over */
    _Alignas(64) uint64_t tail;                             /* made by the recording thread */
    _Alignas(64) int asleep;                                /* the recording thread waits for g_rc_wake */
    _Alignas(64) uint64_t wr, tail_seen;                    /* the writer's own: written so far; the last tail it read */
    LONG64 n, flushes, waits;                               /* entries; flushes; flushes that found the queue not empty */
} g_rq;
#define g_rc_n g_rq.n
#define g_rc_flushes g_rq.flushes
#define g_rc_waits g_rq.waits
static int g_rc_on = -1;
static HANDLE g_rc_wake;
static void rc_run(const RcHdr *h) {
    const uint8_t *p = (const uint8_t *)(h + 1);
    VkCommandBuffer c; memcpy(&c, p, sizeof c); p += 8;
    const uint32_t *u = (const uint32_t *)p;
    switch (h->op) {
    case RC_G1: { RcG1 f; memcpy(&f, p, 8); f(c, u[2]); break; }
    case RC_G2: { RcG2 f; memcpy(&f, p, 8); f(c, u[2], u[3]); break; }
    case RC_PIPE: { VkPipeline pl; memcpy(&pl, p + 8, 8); vkCmdBindPipeline(c, (VkPipelineBindPoint)u[0], pl); break; }
    case RC_VIN: vkCmdSetVertexInputEXT(c, u[0], (const VkVertexInputBindingDescription2EXT *)(p + 8), u[1], (const VkVertexInputAttributeDescription2EXT *)(p + 8 + u[0] * sizeof(VkVertexInputBindingDescription2EXT))); break;
    case RC_VB: vkCmdBindVertexBuffers(c, u[0], u[1], (const VkBuffer *)(p + 8), (const VkDeviceSize *)(p + 8 + 8 * u[1])); break;
    case RC_PUSH: {                                         /* per write 16 bytes (binding, element, count, type and kind), then its infos */
        VkPipelineLayout lay; memcpy(&lay, p + 16, 8);
        VkWriteDescriptorSet w[64];
        const uint8_t *q = p + 24;
        for (uint32_t i = 0; i < u[2]; i++) {
            const uint32_t *e = (const uint32_t *)q; q += 16;
            memset(&w[i], 0, sizeof w[i]);
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstBinding = e[0]; w[i].dstArrayElement = e[1]; w[i].descriptorCount = e[2]; w[i].descriptorType = (VkDescriptorType)(e[3] & 0x7fffffff);
            if (e[3] >> 31) { w[i].pImageInfo = (const VkDescriptorImageInfo *)q; q += e[2] * sizeof(VkDescriptorImageInfo); }
            else { w[i].pBufferInfo = (const VkDescriptorBufferInfo *)q; q += e[2] * sizeof(VkDescriptorBufferInfo); }
        }
        vkCmdPushDescriptorSetKHR(c, (VkPipelineBindPoint)u[0], lay, u[1], u[2], w);
        break; }
    case RC_SETS: { VkPipelineLayout lay; memcpy(&lay, p + 16, 8); vkCmdBindDescriptorSets(c, (VkPipelineBindPoint)u[0], lay, u[1], u[2], (const VkDescriptorSet *)(p + 24), u[3], (const uint32_t *)(p + 24 + 8 * u[2])); break; }
    case RC_PC: { VkPipelineLayout lay; memcpy(&lay, p + 16, 8); vkCmdPushConstants(c, lay, u[0], u[1], u[2], p + 24); break; }
    case RC_IB: { VkBuffer b; VkDeviceSize o; memcpy(&b, p + 8, 8); memcpy(&o, p + 16, 8); vkCmdBindIndexBuffer(c, b, o, (VkIndexType)u[0]); break; }
    case RC_DRAW: vkCmdDraw(c, u[0], u[1], u[2], u[3]); break;
    case RC_DRAWX: vkCmdDrawIndexed(c, u[0], u[1], u[2], (int32_t)u[3], u[4]); break;
    case RC_IND: case RC_INDX: { VkBuffer b; VkDeviceSize o; memcpy(&b, p + 8, 8); memcpy(&o, p + 16, 8); if (h->op == RC_IND) vkCmdDrawIndirect(c, b, o, u[0], u[1]); else vkCmdDrawIndexedIndirect(c, b, o, u[0], u[1]); break; }
    case RC_INDC: case RC_INDXC: {
        VkBuffer b, cb; VkDeviceSize o, co; memcpy(&b, p + 8, 8); memcpy(&o, p + 16, 8); memcpy(&cb, p + 24, 8); memcpy(&co, p + 32, 8);
        if (h->op == RC_INDC) vkCmdDrawIndirectCount_(c, b, o, cb, co, u[0], u[1]); else vkCmdDrawIndexedIndirectCount_(c, b, o, cb, co, u[0], u[1]);
        break; }
    case RC_VP: vkCmdSetViewport(c, u[0], u[1], (const VkViewport *)(p + 8)); break;
    case RC_SC: vkCmdSetScissor(c, u[0], u[1], (const VkRect2D *)(p + 8)); break;
    }
}
static DWORD WINAPI rc_thread(LPVOID arg) {
    (void)arg;
    uint64_t t = 0, head = 0;
    for (int idle = 0;;) {
        if (t == head) {
            head = __atomic_load_n(&g_rq.head, __ATOMIC_SEQ_CST);
            if (t != head) { idle = 0; continue; }
            if (++idle < 4000) { YieldProcessor(); continue; }
            __atomic_store_n(&g_rq.asleep, 1, __ATOMIC_SEQ_CST);      /* the writer looks at this after it has published (rc_pub): one of the two sees the other */
            if (t == __atomic_load_n(&g_rq.head, __ATOMIC_SEQ_CST)) WaitForSingleObject(g_rc_wake, 100);
            __atomic_store_n(&g_rq.asleep, 0, __ATOMIC_SEQ_CST);
            idle = 0;
            continue;
        }
        const RcHdr *h = (const RcHdr *)(g_rc_buf + (t & (RC_BYTES - 1)));
        if (h->op != RC_WRAP) rc_run(h);
        t += h->len;
        __atomic_store_n(&g_rq.tail, t, __ATOMIC_RELEASE);
    }
    return 0;
}
static int rc_on(void) {
    if (g_rc_on < 0) {
        const char *e = getenv("WT_VK_REC");
        int on = e && atoi(e);
        if (on) { g_rc_buf = malloc(RC_BYTES); g_rc_wake = CreateEventA(NULL, FALSE, FALSE, NULL); CloseHandle(CreateThread(NULL, 0, rc_thread, NULL, 0, NULL)); printf("nvn_vk_rec: WT_VK_REC=1 (test): the per-draw Vulkan commands are recorded by a second thread\n"); }
        g_rc_on = on;
    }
    return g_rc_on;
}
static void rc_pub(void) {                              /* what was written is handed to the recording thread */
    if (g_rq.head == g_rq.wr) return;
    __atomic_store_n(&g_rq.head, g_rq.wr, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_rq.asleep, __ATOMIC_SEQ_CST)) SetEvent(g_rc_wake);
}
/* The queue is empty and its last command made: the caller may record, end or hand over a command buffer. */
static void rc_flush(void) {
    if (g_rc_on <= 0) return;
    rc_pub();
    g_rc_flushes++;
    if (__atomic_load_n(&g_rq.tail, __ATOMIC_ACQUIRE) == g_rq.wr) return;
    g_rc_waits++;
    while (__atomic_load_n(&g_rq.tail, __ATOMIC_ACQUIRE) != g_rq.wr) YieldProcessor();
}
static void rc_room(uint64_t end) {
    while (end - g_rq.tail_seen > RC_BYTES) { rc_pub(); g_rq.tail_seen = __atomic_load_n(&g_rq.tail, __ATOMIC_ACQUIRE); if (end - g_rq.tail_seen > RC_BYTES) YieldProcessor(); }
}
/* Room for an entry of n payload bytes after the command buffer's handle. One writer: the render thread. A draw hands the
 * entries written so far to the recording thread (rc_pub), and so does rc_flush. */
static uint8_t *rc_put(uint32_t op, VkCommandBuffer c, uint32_t n) {
    uint32_t len = (uint32_t)(sizeof(RcHdr) + 8 + n + 7) & ~7u, at = (uint32_t)(g_rq.wr & (RC_BYTES - 1));
    if (at + len > RC_BYTES) {                         /* an entry is never split: the rest of the buffer is skipped */
        rc_room(g_rq.wr + (RC_BYTES - at));
        RcHdr *w = (RcHdr *)(g_rc_buf + at); w->op = RC_WRAP; w->len = RC_BYTES - at;
        g_rq.wr += RC_BYTES - at; at = 0;
    }
    rc_room(g_rq.wr + len);
    RcHdr *h = (RcHdr *)(g_rc_buf + at); h->op = op; h->len = len;
    memcpy(h + 1, &c, sizeof c);
    g_rq.wr += len; g_rc_n++;
    return (uint8_t *)(h + 1) + 8;
}
static void rc_g1(RcG1 f, VkCommandBuffer c, uint32_t v) { if (!rc_on()) { f(c, v); return; } uint8_t *p = rc_put(RC_G1, c, 12); memcpy(p, &f, 8); memcpy(p + 8, &v, 4); }
static void rc_g2(RcG2 f, VkCommandBuffer c, uint32_t a, uint32_t b) { if (!rc_on()) { f(c, a, b); return; } uint8_t *p = rc_put(RC_G2, c, 16); memcpy(p, &f, 8); memcpy(p + 8, &a, 4); memcpy(p + 12, &b, 4); }
static void rc_BindPipeline(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipeline pl) {
    if (!rc_on()) { vkCmdBindPipeline(c, bp, pl); return; }
    uint8_t *p = rc_put(RC_PIPE, c, 16); uint32_t b = (uint32_t)bp; memcpy(p, &b, 4); memcpy(p + 8, &pl, 8);
}
static void rc_SetVertexInput(VkCommandBuffer c, uint32_t nb, const VkVertexInputBindingDescription2EXT *b, uint32_t na, const VkVertexInputAttributeDescription2EXT *a) {
    if (!rc_on()) { vkCmdSetVertexInputEXT(c, nb, b, na, a); return; }
    uint8_t *p = rc_put(RC_VIN, c, 8 + nb * (uint32_t)sizeof *b + na * (uint32_t)sizeof *a);
    memcpy(p, &nb, 4); memcpy(p + 4, &na, 4); if (nb) memcpy(p + 8, b, nb * sizeof *b); if (na) memcpy(p + 8 + nb * sizeof *b, a, na * sizeof *a);
}
static void rc_BindVertexBuffers(VkCommandBuffer c, uint32_t first, uint32_t n, const VkBuffer *b, const VkDeviceSize *o) {
    if (!rc_on()) { vkCmdBindVertexBuffers(c, first, n, b, o); return; }
    uint8_t *p = rc_put(RC_VB, c, 8 + 16 * n);
    memcpy(p, &first, 4); memcpy(p + 4, &n, 4); memcpy(p + 8, b, 8 * n); memcpy(p + 8 + 8 * n, o, 8 * n);
}
static void rc_PushDescriptorSet(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout lay, uint32_t set, uint32_t n, const VkWriteDescriptorSet *w) {
    uint32_t bytes = 0, plain = n <= 64;
    for (uint32_t i = 0; i < n; i++) {                    /* buffers and images only, no extension structures: anything else is made by the caller */
        if (w[i].pNext || w[i].pTexelBufferView || (!w[i].pBufferInfo && !w[i].pImageInfo) || (uint32_t)w[i].descriptorType >> 31) plain = 0;
        bytes += 16 + w[i].descriptorCount * (uint32_t)(w[i].pBufferInfo ? sizeof(VkDescriptorBufferInfo) : sizeof(VkDescriptorImageInfo));
    }
    if (!rc_on() || !plain) { rc_flush(); vkCmdPushDescriptorSetKHR(c, bp, lay, set, n, w); return; }
    uint8_t *p = rc_put(RC_PUSH, c, 24 + bytes), *q = p + 24;
    uint32_t b = (uint32_t)bp;
    memcpy(p, &b, 4); memcpy(p + 4, &set, 4); memcpy(p + 8, &n, 4); memcpy(p + 16, &lay, 8);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t e[4] = {w[i].dstBinding, w[i].dstArrayElement, w[i].descriptorCount, (uint32_t)w[i].descriptorType | (w[i].pBufferInfo ? 0 : 1u << 31)};
        size_t k = w[i].descriptorCount * (w[i].pBufferInfo ? sizeof(VkDescriptorBufferInfo) : sizeof(VkDescriptorImageInfo));
        memcpy(q, e, 16); memcpy(q + 16, w[i].pBufferInfo ? (const void *)w[i].pBufferInfo : (const void *)w[i].pImageInfo, k); q += 16 + k;
    }
}
static void rc_BindDescriptorSets(VkCommandBuffer c, VkPipelineBindPoint bp, VkPipelineLayout lay, uint32_t first, uint32_t n, const VkDescriptorSet *s, uint32_t nd, const uint32_t *d) {
    if (!rc_on()) { vkCmdBindDescriptorSets(c, bp, lay, first, n, s, nd, d); return; }
    uint8_t *p = rc_put(RC_SETS, c, 24 + 8 * n + 4 * nd); uint32_t b = (uint32_t)bp;
    memcpy(p, &b, 4); memcpy(p + 4, &first, 4); memcpy(p + 8, &n, 4); memcpy(p + 12, &nd, 4); memcpy(p + 16, &lay, 8); memcpy(p + 24, s, 8 * n); if (nd) memcpy(p + 24 + 8 * n, d, 4 * nd);
}
static void rc_PushConstants(VkCommandBuffer c, VkPipelineLayout lay, VkShaderStageFlags st, uint32_t off, uint32_t size, const void *data) {
    if (!rc_on()) { vkCmdPushConstants(c, lay, st, off, size, data); return; }
    uint8_t *p = rc_put(RC_PC, c, 24 + size); uint32_t s = (uint32_t)st;
    memcpy(p, &s, 4); memcpy(p + 4, &off, 4); memcpy(p + 8, &size, 4); memcpy(p + 16, &lay, 8); memcpy(p + 24, data, size);
}
static void rc_BindIndexBuffer(VkCommandBuffer c, VkBuffer buf, VkDeviceSize off, VkIndexType t) {
    if (!rc_on()) { vkCmdBindIndexBuffer(c, buf, off, t); return; }
    uint8_t *p = rc_put(RC_IB, c, 24); uint32_t v = (uint32_t)t; memcpy(p, &v, 4); memcpy(p + 8, &buf, 8); memcpy(p + 16, &off, 8);
}
static void rc_Draw(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, uint32_t e) {
    if (!rc_on()) { vkCmdDraw(c, a, b, d, e); return; }
    uint32_t v[4] = {a, b, d, e}; memcpy(rc_put(RC_DRAW, c, 16), v, 16); rc_pub();
}
static void rc_DrawIndexed(VkCommandBuffer c, uint32_t a, uint32_t b, uint32_t d, int32_t e, uint32_t f) {
    if (!rc_on()) { vkCmdDrawIndexed(c, a, b, d, e, f); return; }
    uint32_t v[5] = {a, b, d, (uint32_t)e, f}; memcpy(rc_put(RC_DRAWX, c, 20), v, 20); rc_pub();
}
static void rc_ind(uint32_t op, VkCommandBuffer c, VkBuffer buf, VkDeviceSize off, uint32_t n, uint32_t stride) {
    uint8_t *p = rc_put(op, c, 24); memcpy(p, &n, 4); memcpy(p + 4, &stride, 4); memcpy(p + 8, &buf, 8); memcpy(p + 16, &off, 8); rc_pub();
}
static void rc_DrawIndirect(VkCommandBuffer c, VkBuffer buf, VkDeviceSize off, uint32_t n, uint32_t stride) { if (!rc_on()) vkCmdDrawIndirect(c, buf, off, n, stride); else rc_ind(RC_IND, c, buf, off, n, stride); }
static void rc_DrawIndexedIndirect(VkCommandBuffer c, VkBuffer buf, VkDeviceSize off, uint32_t n, uint32_t stride) { if (!rc_on()) vkCmdDrawIndexedIndirect(c, buf, off, n, stride); else rc_ind(RC_INDX, c, buf, off, n, stride); }
static void rc_indc(uint32_t op, VkCommandBuffer c, VkBuffer buf, VkDeviceSize off, VkBuffer cb, VkDeviceSize co, uint32_t n, uint32_t stride) {
    uint8_t *p = rc_put(op, c, 40); memcpy(p, &n, 4); memcpy(p + 4, &stride, 4); memcpy(p + 8, &buf, 8); memcpy(p + 16, &off, 8); memcpy(p + 24, &cb, 8); memcpy(p + 32, &co, 8); rc_pub();
}
static void rc_DrawIndirectCount(VkCommandBuffer c, VkBuffer buf, VkDeviceSize off, VkBuffer cb, VkDeviceSize co, uint32_t n, uint32_t stride) { if (!rc_on()) vkCmdDrawIndirectCount_(c, buf, off, cb, co, n, stride); else rc_indc(RC_INDC, c, buf, off, cb, co, n, stride); }
static void rc_DrawIndexedIndirectCount(VkCommandBuffer c, VkBuffer buf, VkDeviceSize off, VkBuffer cb, VkDeviceSize co, uint32_t n, uint32_t stride) { if (!rc_on()) vkCmdDrawIndexedIndirectCount_(c, buf, off, cb, co, n, stride); else rc_indc(RC_INDXC, c, buf, off, cb, co, n, stride); }
static void rc_SetViewport(VkCommandBuffer c, uint32_t first, uint32_t n, const VkViewport *v) {
    if (!rc_on()) { vkCmdSetViewport(c, first, n, v); return; }
    uint8_t *p = rc_put(RC_VP, c, 8 + n * (uint32_t)sizeof *v); memcpy(p, &first, 4); memcpy(p + 4, &n, 4); memcpy(p + 8, v, n * sizeof *v);
}
static void rc_SetScissor(VkCommandBuffer c, uint32_t first, uint32_t n, const VkRect2D *v) {
    if (!rc_on()) { vkCmdSetScissor(c, first, n, v); return; }
    uint8_t *p = rc_put(RC_SC, c, 8 + n * (uint32_t)sizeof *v); memcpy(p, &first, 4); memcpy(p + 4, &n, 4); memcpy(p + 8, v, n * sizeof *v);
}
/* the hot calls go to the queue */
#define vkCmdBindPipeline(...) rc_BindPipeline(__VA_ARGS__)
#define vkCmdSetVertexInputEXT(...) rc_SetVertexInput(__VA_ARGS__)
#define vkCmdBindVertexBuffers(...) rc_BindVertexBuffers(__VA_ARGS__)
#define vkCmdPushDescriptorSetKHR(...) rc_PushDescriptorSet(__VA_ARGS__)
#define vkCmdBindDescriptorSets(...) rc_BindDescriptorSets(__VA_ARGS__)
#define vkCmdPushConstants(...) rc_PushConstants(__VA_ARGS__)
#define vkCmdBindIndexBuffer(...) rc_BindIndexBuffer(__VA_ARGS__)
#define vkCmdDraw(...) rc_Draw(__VA_ARGS__)
#define vkCmdDrawIndexed(...) rc_DrawIndexed(__VA_ARGS__)
#define vkCmdDrawIndirect(...) rc_DrawIndirect(__VA_ARGS__)
#define vkCmdDrawIndexedIndirect(...) rc_DrawIndexedIndirect(__VA_ARGS__)
#define vkCmdDrawIndirectCount_(...) rc_DrawIndirectCount(__VA_ARGS__)
#define vkCmdDrawIndexedIndirectCount_(...) rc_DrawIndexedIndirectCount(__VA_ARGS__)
#define vkCmdSetViewport(...) rc_SetViewport(__VA_ARGS__)
#define vkCmdSetScissor(...) rc_SetScissor(__VA_ARGS__)
#define vkCmdSetPrimitiveTopology(c, v) rc_g1((RcG1)vkCmdSetPrimitiveTopology, c, (uint32_t)(v))
#define vkCmdSetPrimitiveRestartEnable(c, v) rc_g1((RcG1)vkCmdSetPrimitiveRestartEnable, c, (uint32_t)(v))
#define vkCmdSetCullMode(c, v) rc_g1((RcG1)vkCmdSetCullMode, c, (uint32_t)(v))
#define vkCmdSetFrontFace(c, v) rc_g1((RcG1)vkCmdSetFrontFace, c, (uint32_t)(v))
#define vkCmdSetDepthTestEnable(c, v) rc_g1((RcG1)vkCmdSetDepthTestEnable, c, (uint32_t)(v))
#define vkCmdSetDepthWriteEnable(c, v) rc_g1((RcG1)vkCmdSetDepthWriteEnable, c, (uint32_t)(v))
#define vkCmdSetDepthCompareOp(c, v) rc_g1((RcG1)vkCmdSetDepthCompareOp, c, (uint32_t)(v))
#define vkCmdSetDepthBiasEnable(c, v) rc_g1((RcG1)vkCmdSetDepthBiasEnable, c, (uint32_t)(v))
#define vkCmdSetStencilTestEnable(c, v) rc_g1((RcG1)vkCmdSetStencilTestEnable, c, (uint32_t)(v))
#define vkCmdSetRasterizerDiscardEnable(c, v) rc_g1((RcG1)vkCmdSetRasterizerDiscardEnable, c, (uint32_t)(v))
#define vkCmdSetDepthBoundsTestEnable(c, v) rc_g1((RcG1)vkCmdSetDepthBoundsTestEnable, c, (uint32_t)(v))
#define vkCmdSetStencilCompareMask(c, a, b) rc_g2((RcG2)vkCmdSetStencilCompareMask, c, (uint32_t)(a), (uint32_t)(b))
#define vkCmdSetStencilWriteMask(c, a, b) rc_g2((RcG2)vkCmdSetStencilWriteMask, c, (uint32_t)(a), (uint32_t)(b))
#define vkCmdSetStencilReference(c, a, b) rc_g2((RcG2)vkCmdSetStencilReference, c, (uint32_t)(a), (uint32_t)(b))
/* every other call that records into, begins, ends or resets a command buffer waits for the queue first */
#define vkCmdBeginQuery(...) (rc_flush(), vkCmdBeginQuery(__VA_ARGS__))
#define vkCmdBeginRendering(...) (rc_flush(), vkCmdBeginRendering(__VA_ARGS__))
#define vkCmdBlitImage(...) (rc_flush(), vkCmdBlitImage(__VA_ARGS__))
#define vkCmdClearAttachments(...) (rc_flush(), vkCmdClearAttachments(__VA_ARGS__))
#define vkCmdClearColorImage(...) (rc_flush(), vkCmdClearColorImage(__VA_ARGS__))
#define vkCmdClearDepthStencilImage(...) (rc_flush(), vkCmdClearDepthStencilImage(__VA_ARGS__))
#define vkCmdCopyBuffer(...) (rc_flush(), vkCmdCopyBuffer(__VA_ARGS__))
#define vkCmdCopyBufferToImage(...) (rc_flush(), vkCmdCopyBufferToImage(__VA_ARGS__))
#define vkCmdCopyImage(...) (rc_flush(), vkCmdCopyImage(__VA_ARGS__))
#define vkCmdCopyImageToBuffer(...) (rc_flush(), vkCmdCopyImageToBuffer(__VA_ARGS__))
#define vkCmdDispatch(...) (rc_flush(), vkCmdDispatch(__VA_ARGS__))
#define vkCmdEndQuery(...) (rc_flush(), vkCmdEndQuery(__VA_ARGS__))
#define vkCmdEndRendering(...) (rc_flush(), vkCmdEndRendering(__VA_ARGS__))
#define vkCmdPipelineBarrier(...) (rc_flush(), vkCmdPipelineBarrier(__VA_ARGS__))
#define vkCmdResetQueryPool(...) (rc_flush(), vkCmdResetQueryPool(__VA_ARGS__))
#define vkCmdResolveImage(...) (rc_flush(), vkCmdResolveImage(__VA_ARGS__))
#define vkCmdSetAlphaToCoverageEnableEXT(...) (rc_flush(), vkCmdSetAlphaToCoverageEnableEXT(__VA_ARGS__))
#define vkCmdSetBlendConstants(...) (rc_flush(), vkCmdSetBlendConstants(__VA_ARGS__))
#define vkCmdSetCheckpointNV_(...) (rc_flush(), vkCmdSetCheckpointNV_(__VA_ARGS__))
#define vkCmdSetColorBlendEnableEXT(...) (rc_flush(), vkCmdSetColorBlendEnableEXT(__VA_ARGS__))
#define vkCmdSetColorBlendEquationEXT(...) (rc_flush(), vkCmdSetColorBlendEquationEXT(__VA_ARGS__))
#define vkCmdSetColorWriteMaskEXT(...) (rc_flush(), vkCmdSetColorWriteMaskEXT(__VA_ARGS__))
#define vkCmdSetDepthBias(...) (rc_flush(), vkCmdSetDepthBias(__VA_ARGS__))
#define vkCmdSetDepthBounds(...) (rc_flush(), vkCmdSetDepthBounds(__VA_ARGS__))
#define vkCmdSetDepthClampEnableEXT(...) (rc_flush(), vkCmdSetDepthClampEnableEXT(__VA_ARGS__))
#define vkCmdSetLineWidth(...) (rc_flush(), vkCmdSetLineWidth(__VA_ARGS__))
#define vkCmdSetPolygonModeEXT(...) (rc_flush(), vkCmdSetPolygonModeEXT(__VA_ARGS__))
#define vkCmdSetSampleMaskEXT(...) (rc_flush(), vkCmdSetSampleMaskEXT(__VA_ARGS__))
#define vkCmdSetStencilOp(...) (rc_flush(), vkCmdSetStencilOp(__VA_ARGS__))
#define vkCmdWriteTimestamp(...) (rc_flush(), vkCmdWriteTimestamp(__VA_ARGS__))
#define vkBeginCommandBuffer(...) (rc_flush(), vkBeginCommandBuffer(__VA_ARGS__))
#define vkEndCommandBuffer(...) (rc_flush(), vkEndCommandBuffer(__VA_ARGS__))
#define vkResetCommandBuffer(...) (rc_flush(), vkResetCommandBuffer(__VA_ARGS__))
