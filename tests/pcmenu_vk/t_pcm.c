/* pcmenu.c with the stubs of its surroundings, the clock frozen, and the hooks vktest.c calls (see vktest.c) */
#include <windows.h>
#define GetTickCount64() ((uint64_t)1234567)
#include "../../runtime/pcmenu.c"                       /* (clang 19 or later: #embed) */
#include <stdio.h>
#include <time.h>
volatile LONG g_guest_frozen, g_display_close;
volatile int game_cutscene_fps = 30;
static GfxOptions g_o = {.width = 1920, .height = 1080, .shader_prepare = 0};
const GfxOptions *gfx_options(void) { return &g_o; }
int gfx_ini_get(const char *k, char *v, size_t n) { (void)k; (void)v; (void)n; return 0; }
int gfx_option_set(const char *k, const char *v) { (void)k; (void)v; return 1; }
void nnhid_menu_read(uint64_t *b, int *lx, int *ly, int s) { *b = 0; *lx = *ly = 0; (void)s; }
int nnhid_menu_key(int vk) { (void)vk; return 0; }
uint32_t nnhid_menu_pad(void) { return 0; }
int nvn_front_post_pcmenu(void) { return 0; }
void display_vsync_apply(void) {}
int display_window_apply(void) { return 1; }
void display_mouse_want(int on) { (void)on; }
int nnpad_mouse_cfg(float *s, int *i) { (void)s; (void)i; return 0; }
const char *nnpad_prompts_setting(void) { return "auto"; }
int nnpad_prompts_set(const char *v) { (void)v; return 1; }
const char *nnpad_menu_swap_setting(void) { return "off"; }
int nnpad_menu_swap_set(const char *v) { (void)v; return 1; }
int nnpad_menu_swapped(void) { return 0; }
const char *nnpad_layout_setting(void) { return "auto"; }
int nnpad_layout_set(const char *v) { (void)v; return 1; }
int nnpad_button_prompts(void) { return 1; }
int nnpad_prompts_by_letter(void) { return 0; }
int nnpad_bind_get(int ch, unsigned char *o) { (void)ch; (void)o; return 0; }
int nnpad_bind_set(int ch, const unsigned char *b) { (void)ch; (void)b; return 1; }
int options_control_type(int ch) { (void)ch; return 0; }
static void save(const char *path) {
    FILE *f = fopen(path, "wb");
    int w = g_pw, h = g_ph; unsigned off = 54, size = off + (unsigned)w * h * 4;
    unsigned char hd[54] = {'B','M', size, size >> 8, size >> 16, size >> 24, 0,0,0,0, off,0,0,0, 40,0,0,0, w, w >> 8, w >> 16, w >> 24, h, h >> 8, h >> 16, h >> 24, 1,0, 32,0, 0,0,0,0};
    fwrite(hd, 1, 54, f);
    for (int y = h - 1; y >= 0; y--) { for (int x = 0; x < w; x++) { unsigned char *p = g_px + ((size_t)y * w + x) * 4, q[4] = {p[0], p[1], p[2], 255}; fwrite(q, 1, 4, f); } }
    fclose(f);
}

/* hooks for the Vulkan test (vktest.c): the menu open, or the first-start screen */

void t_setup(int which) {
    if (!g_inited) { if (!fonts_init()) { printf("fonts\n"); exit(1); } menu_init(); g_inited = 1; }
    if (which == 1) { g_open = 1; g_prep.on = 0; g_sel = 4; g_cur[4] = 2; g_start[4] = 0; }
    else { g_open = 0; g_prep.on = 1; g_prep.pct = 62; g_prep.done = 2311; g_prep.total = 3720; g_prep.eta = 48; snprintf(g_prep.info, sizeof g_prep.info, "Vulkan  \xC2\xB7  llvmpipe"); g_ver++; }
}
short GetAsyncKeyState(int vk) { (void)vk; return 0; }
int display_mouse_pixels(int *x, int *y) { (void)x; (void)y; return 0; }
