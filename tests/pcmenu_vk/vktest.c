/* A Linux test of nvn_vk_pcmenu.c's GPU path: the panel (screen 1) or the first-start screen (3) composed by the pipeline
 * against the CPU composition at the same clock; stubs stand in for nvn_vk_draw.c. Needs libvulkan, glslangValidator and a
 * Vulkan 1.3 device (lavapipe does: mesa-vulkan-drivers). Writes cpu<n>.bmp, gpu<n>.bmp, diff<n>.bmp.
 *   RT=../../runtime; FL="-O1 -w -I $RT/posix -I $RT -I $RT/gen -D_GNU_SOURCE -D__declspec(x)= -D__stdcall= -D__cdecl="
 *   clang $FL -c t_pcm.c && clang $FL -I . -c vktest.c && clang $FL -c $RT/plat_posix.c && clang t_pcm.o vktest.o plat_posix.o -lvulkan -lm -lpthread -o vktest
 *   VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./vktest 1920 1080 1
 * (the clock is frozen in t_pcm.c; the hands follow the wall clock, so a second's change between the two renders shows as a
 * hand's difference) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include <windows.h>
#include <vulkan/vulkan.h>
#include "gfx_options.h"
typedef struct { int block; uint64_t off, size; void *map; VkDeviceMemory mem; } VMem;
static struct { VkInstance inst; VkPhysicalDevice pd; VkPhysicalDeviceMemoryProperties mp; VkDevice device; VkQueue queue; uint32_t qf; VkExtent2D extent; int hdr, alive; VkSwapchainKHR swapchain; } V;
static struct { VkCommandPool pool; VkCommandBuffer cmd; void *pipe_bound; int dyn_dirty, dyn3_dirty, in_pass; } R;
static void vk_fatal(const char *what, VkResult r) { printf("FATAL %s: %d\n", what, (int)r); exit(2); }
#define VKC(call) do { VkResult r_ = (call); if (r_ != VK_SUCCESS) vk_fatal(#call, r_); } while (0)
#define DBG_ASSERT(c, m) do { if (!(c)) { printf("assert: %s\n", m); exit(3); } } while (0)
static const char *vk_result_name(VkResult r) { static char b[32]; snprintf(b, sizeof b, "%d", (int)r); return b; }
static uint32_t mem_type(uint32_t bits, VkFlags want) { for (uint32_t i = 0; i < V.mp.memoryTypeCount; i++) if ((bits >> i & 1) && (V.mp.memoryTypes[i].propertyFlags & want) == want) return i; printf("no memory type\n"); exit(4); }
static VMem vmem_alloc(VkMemoryRequirements rq, VkFlags want, int cls) {
    VMem m = {0}; (void)cls;
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = rq.size; ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, want);
    VKC(vkAllocateMemory(V.device, &ai, NULL, &m.mem)); m.size = rq.size;
    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) VKC(vkMapMemory(V.device, m.mem, 0, rq.size, 0, &m.map));
    return m;
}
static void vmem_free(VMem m) { if (m.mem) vkFreeMemory(V.device, m.mem, NULL); }
static VkBuffer vbuf_new(uint64_t size, VkFlags usage, VMem *mem) {
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; VkBuffer b; VkMemoryRequirements rq;
    bi.size = size; bi.usage = usage; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VKC(vkCreateBuffer(V.device, &bi, NULL, &b)); vkGetBufferMemoryRequirements(V.device, b, &rq);
    *mem = vmem_alloc(rq, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, 0);
    VKC(vkBindBufferMemory(V.device, b, mem->mem, 0));
    return b;
}
static VkFlags layout_access(VkImageLayout l) {
    switch (l) {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return VK_ACCESS_TRANSFER_WRITE_BIT; case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return VK_ACCESS_TRANSFER_READ_BIT;
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return VK_ACCESS_SHADER_READ_BIT; case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL: return VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    default: return 0;
    }
}
static void image_barrier(VkImage img, VkFlags aspect, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    DBG_ASSERT(!R.in_pass, "an image layout change inside a render pass");
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img;
    b.subresourceRange.aspectMask = aspect; b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS; b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    b.srcAccessMask = layout_access(from); b.dstAccessMask = layout_access(to); b.oldLayout = from; b.newLayout = to;
    vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}
static void xfer_wait(void) { VkMemoryBarrier m = {VK_STRUCTURE_TYPE_MEMORY_BARRIER}; m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; m.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT; vkCmdPipelineBarrier(R.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &m, 0, NULL, 0, NULL); }
static void cmd_wait_all(void) { vkQueueWaitIdle(V.queue); }
static uint64_t g_spv_unused;
static VkShaderModule module_from_glsl(int stage, const char *glsl, const char *what, uint64_t *h) {
    (void)h; FILE *f = fopen(stage ? "s.frag" : "s.vert", "wb"); fwrite(glsl, 1, strlen(glsl), f); fclose(f);
    if (system(stage ? "glslangValidator -V s.frag -o s.spv >glslang.log 2>&1" : "glslangValidator -V s.vert -o s.spv >glslang.log 2>&1")) { printf("glslang rejected %s\n", what); system("cat glslang.log"); return NULL; }
    f = fopen("s.spv", "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); uint32_t *w = malloc(n); fread(w, 1, n, f); fclose(f);
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO}; ci.codeSize = n; ci.pCode = w; VkShaderModule m; VKC(vkCreateShaderModule(V.device, &ci, NULL, &m)); free(w); return m;
}
static void dsh_forget(void) {}
static int g_nsmp_old, g_nsmp; static VkSampler g_smp_old[4]; static struct { VkSampler s; } g_smp[4];
const GfxOptions *gfx_options(void);
static void present_mode_pick(void) {} static void swapchain_destroy(void) {} static LONG64 g_vk_recreated; static void rs_apply(void) {}
typedef struct { void *win; } PresentJob; static PresentJob g_pcm_last; static void vk_present2(void *a, int p) { (void)a; (void)p; }
typedef struct { char path[300]; uint8_t *px; int w, h, bgr; } BmpJob; static LONG g_bmp_pending; static void bmp_write_thread(BmpJob *j) { free(j->px); free(j); }
static int g_sc_src;
static void pcm_upload(int wait); static void pcm_vk_draw(VkImage tgt); static void pcm_vk_free(void);
#include "../../runtime/nvn_vk_pcmenu.c"
void t_setup(int which);
static void save_bmp(const char *path, const uint8_t *px, int w, int h) {
    FILE *f = fopen(path, "wb"); unsigned off = 54, size = off + (unsigned)w * h * 4;
    unsigned char hd[54] = {'B','M', size, size >> 8, size >> 16, size >> 24, 0,0,0,0, off,0,0,0, 40,0,0,0, w, w >> 8, w >> 16, w >> 24, h, h >> 8, h >> 16, h >> 24, 1,0, 32,0, 0,0,0,0};
    fwrite(hd, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) for (int x = 0; x < w; x++) { const uint8_t *p = px + ((size_t)y * w + x) * 4; unsigned char q[4] = {p[0], p[1], p[2], 255}; fwrite(q, 1, 4, f); }
    fclose(f);
}
static void cmd_begin(void) { VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT; VKC(vkResetCommandBuffer(R.cmd, 0)); VKC(vkBeginCommandBuffer(R.cmd, &bi)); }
static void cmd_run(void) { VKC(vkEndCommandBuffer(R.cmd)); VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &R.cmd; VKC(vkQueueSubmit(V.queue, 1, &si, NULL)); VKC(vkQueueWaitIdle(V.queue)); }
int main(int argc, char **argv) {
    int W = argc > 1 ? atoi(argv[1]) : 1280, H = argc > 2 ? atoi(argv[2]) : 720, which = argc > 3 ? atoi(argv[3]) : 1;
    VkApplicationInfo ap = {VK_STRUCTURE_TYPE_APPLICATION_INFO}; ap.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ii = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ii.pApplicationInfo = &ap;
    VKC(vkCreateInstance(&ii, NULL, &V.inst));
    uint32_t n = 1; VKC(vkEnumeratePhysicalDevices(V.inst, &n, &V.pd)); if (!n) { printf("no device\n"); return 1; }
    vkGetPhysicalDeviceMemoryProperties(V.pd, &V.mp);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(V.pd, &pp); printf("device: %s\n", pp.deviceName);
    float prio = 1; VkDeviceQueueCreateInfo qi = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES}; f13.dynamicRendering = VK_TRUE;
    VkDeviceCreateInfo di = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; di.pNext = &f13; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    VKC(vkCreateDevice(V.pd, &di, NULL, &V.device)); vkGetDeviceQueue(V.device, 0, 0, &V.queue); V.alive = 1;
    VkCommandPoolCreateInfo pi = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; VKC(vkCreateCommandPool(V.device, &pi, NULL, &R.pool));
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; ai.commandPool = R.pool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1; VKC(vkAllocateCommandBuffers(V.device, &ai, &R.cmd));
    V.extent.width = W; V.extent.height = H;
    t_setup(which);
    /* the CPU composition, as a reference */
    uint8_t *cpu = malloc((size_t)W * H * 4), *gpu = malloc((size_t)W * H * 4);
    { int r[4]; unsigned gen; pcmenu_gpu(0); if (!pcmenu_panel_size(W, H, r, &gen)) { printf("no panel\n"); return 1; } pcmenu_panel_read(cpu, r[2], r[3]); }
    /* the GPU path: the target image in TRANSFER_DST as the swapchain image is, the panel drawn over it, read back */
    VkImage tgt; VMem tmem; VkImageView tview;
    cmd_begin();
    pcm_upload(1);
    if (!g_gpu.on) { printf("GPU path not taken\n"); return 1; }
    pcm_image_new(W, H, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &tgt, &tmem, &tview);
    image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkClearColorValue z = {{1, 0, 1, 1}}; VkImageSubresourceRange sr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(R.cmd, tgt, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &sr);
    for (int pass = 0; pass < 3; pass++) {         /* three presents: the uploads in the first, then the steady state */
        pcm_vk_draw(tgt);
        if (pass < 2) { cmd_run(); cmd_begin(); pcm_upload(1); }
    }
    VMem rb; VkBuffer rbuf = vbuf_new((uint64_t)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &rb);
    image_barrier(tgt, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c; memset(&c, 0, sizeof c); c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; c.imageSubresource.layerCount = 1; c.imageExtent.width = W; c.imageExtent.height = H; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(R.cmd, tgt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &c);
    cmd_run();
    memcpy(gpu, rb.map, (size_t)W * H * 4);
    /* compare */
    long bad = 0, worst = 0; double sum = 0; uint8_t *diff = calloc((size_t)W * H, 4);
    for (long i = 0; i < (long)W * H; i++) { int d = 0; for (int k = 0; k < 3; k++) { int e = abs(cpu[i * 4 + k] - gpu[i * 4 + k]); if (e > d) d = e; } sum += d; if (d > worst) worst = d; if (d > 16) bad++; diff[i * 4] = diff[i * 4 + 1] = diff[i * 4 + 2] = d > 255 ? 255 : d * 4 > 255 ? 255 : d * 4; }
    printf("%dx%d screen %d: %d sprites; worst channel difference %ld, mean %.3f, pixels over 16: %ld of %ld (%.3f%%)\n", W, H, which, pcmenu_gpu_scene(W, H, (PcmSprite[32]){0}, 32), worst, sum / ((double)W * H), bad, (long)W * H, 100.0 * bad / ((double)W * H));
    char nm[64];
    snprintf(nm, sizeof nm, "cpu%d.bmp", which); save_bmp(nm, cpu, W, H); snprintf(nm, sizeof nm, "gpu%d.bmp", which); save_bmp(nm, gpu, W, H); snprintf(nm, sizeof nm, "diff%d.bmp", which); save_bmp(nm, diff, W, H);
    pcm_vk_free();
    return 0;
}
