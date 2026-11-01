/* [witchtime] PC settings menu: a panel drawn by the runtime over the game, for the keys of graphics.ini.
 *
 * Open: F1 on the keyboard, or Start + Select (Plus + Minus) on player 1's controller; seen at the game's own pad reads
 * (pcmenu_gate, nnhid.c), so nothing runs while it is closed but one key and one mask test per read. While it is open:
 *   - the game is held: every guest thread stops at its next SDK call (hostrt.c g_guest_frozen, the console's suspension);
 *     its sound runs dry; the game's pad reads (none while held) get an idle pad, and after the menu
 *     closes too, until the buttons that closed it are let go;
 *   - a thread of its own reads player 1 (nnhid_menu_read: test scripts, the keyboard player's keys, the first controller),
 *     the arrow keys / Enter / Escape / F1 and the mouse, and posts a job to the render thread about 60 times a second that
 *     presents the game's last picture again with the panel over it (nvn_vk_pcmenu.c).
 * Controls: up / down choose a row, left / right (or A / Enter, B / Escape closes) change it; the mouse: point at a row,
 * click on the value (on '‹' for the one before). A change is written to graphics.ini at once (gfx_option_set: comments and
 * other keys kept) and applied while the game runs where the runtime can: cutscene_fps (the game library's switch), vsync
 * (present mode and pacing), the window (Linux), anisotropic filtering and texture detail (new samplers from then on), aspect,
 * resolution_scale (when the panel closes, so that going past 3.0 does not make its images; from the game's Options rows at
 * once: nvn_vk_draw.c rs_apply). The rest (frame_rate) is marked "applies at the next start". A key set by an environment
 * variable is shown and locked.
 * Rows with `optional` appear only when graphics.ini has their key (for keys of later builds; a frame-rate cap: one more row,
 * and its live field in gfx_option_set). texture_lod_bias applies live through the same sampler flush as anisotropic filtering.
 * The panel: drawn on the CPU (BGRA) when something changes, text with stb_truetype (third_party/, public domain / MIT) in
 * a subset of Noto Serif (SIL OFL 1.1, third_party/OFL-NotoSerif.txt), embedded with #embed; opaque, dark, gold lines.
 * One row is not graphics.ini's: button_prompts, of controllers.ini, shown by the game when it runs on (prompts.c).
 * Test aids: SSBU_PCMENU_SHOT=<folder> (nvn_vk_pcmenu.c), SSBU_PCMENU_OPEN=<seconds> opens it that long after the first pad
 * read (for scripted runs without keys), SSBU_PCMENU_LOG=1 logs each input step. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include "gfx_options.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

static const unsigned char k_font[] = {
#embed "third_party/NotoSerif-pcmenu.ttf"
};

extern volatile LONG g_guest_frozen;                       /* hostrt.c */
extern volatile LONG g_display_close;                      /* display.c: the window is being closed (the game must run to exit) */
__declspec(dllimport) extern volatile int game_cutscene_fps;      /* game_shim_eh.c (the game library) */
void nnhid_menu_read(uint64_t *buttons, int *lx, int *ly, int swap_ab);       /* nnhid.c */
int nnhid_menu_key(int vk);
int nvn_front_post_pcmenu(void);                           /* nvn_gl.c: 0 = not the Vulkan renderer */
void display_vsync_apply(void);                            /* display.c */
int display_window_apply(void);
void display_mouse_want(int on);
int nnpad_mouse_cfg(float *sens, int *invert);             /* nnpad_sdl.c */
const char *nnpad_prompts_setting(void);
int nnpad_prompts_set(const char *val);
const char *nnpad_menu_swap_setting(void);
int nnpad_menu_swap_set(const char *val);
int nnpad_menu_swapped(void);

enum { A_LIVE, A_CUT, A_VSYNC, A_WINDOW, A_SIZE, A_SAMPLER, A_SCALE, A_RESTART, A_PROMPTS, A_MENU_SWAP };   /* A_PROMPTS, A_MENU_SWAP: keys of controllers.ini */
#define ROW_VALUES 12
typedef struct {
    const char *key, *label, *help, *env, *dflt;           /* dflt: the reader's value when the key is not in the file */
    int apply, optional;
    const char *val[ROW_VALUES], *name[ROW_VALUES];
} Row;
static const Row k_rows[] = {
    {"window", "Display mode", "Windowed: a window of the size below. Borderless and fullscreen cover the monitor the window is on.", NULL, "windowed", A_WINDOW, 0,
     {"windowed", "borderless", "fullscreen"}, {"Windowed", "Borderless", "Fullscreen"}},
    {"size", "Window size", "The window's size in windowed mode. The game is drawn at the internal resolution and shown at this size.", "SSBU_WINDOW", NULL, A_SIZE, 0,
     {"1280x720", "1600x900", "1920x1080", "2560x1440", "3840x2160"}, {"1280 \xC3\x97 720", "1600 \xC3\x97 900", "1920 \xC3\x97 1080", "2560 \xC3\x97 1440", "3840 \xC3\x97 2160"}},
    {"vsync", "Vertical sync", "Auto: locked to a 60 / 120 / 240 Hz display, else the program's own 60 Hz clock. Off: no waiting, tearing allowed. On: the swapchain's FIFO.", "SSBU_VK_PRESENT", "auto", A_VSYNC, 0,
     {"auto", "display", "clock", "off", "on", "mailbox"}, {"Auto", "Display", "Clock", "Off", "On", "Mailbox"}},
    {"cutscene_fps", "Cutscene frame rate", "The title, menus and cutscenes at 30 fps as on the console, or at 60 like gameplay. Movies keep their own rate.", "SSBU_CUTSCENE_FPS", "30", A_CUT, 0,
     {"30", "60"}, {"30 fps", "60 fps"}},
    {"frame_rate", "Frame rate (experimental)", "EXPERIMENTAL above 60: the game keeps real time, but some effects, input windows and countdowns may still run faster than at 60. Applies at the next start.", NULL, "60", A_RESTART, 0,
     {"60", "90", "120"}, {"60 fps", "90 fps", "120 fps"}},
    {"resolution_scale", "Internal resolution", "The size the game renders at (900p is its own). Applies when this panel closes. The first time away from 900p the game may stop for a while as its shaders are prepared.", "SSBU_VK_SCALE", "1.0", A_SCALE, 0,
     {"0.4", "0.5", "0.6", "0.75", "0.8", "1.0", "1.2", "1.6", "2.0", "2.4", "3.0"},
     {"360p", "450p", "540p", "675p", "720p", "900p", "1080p", "1440p", "1800p", "2160p", "2700p"}},
    {"anisotropic_filtering", "Anisotropic filtering", "Sharper textures at flat angles. Game: as the game set each texture.", NULL, "game", A_SAMPLER, 0,
     {"game", "2", "4", "8", "16"}, {"Game", "2\xC3\x97", "4\xC3\x97", "8\xC3\x97", "16\xC3\x97"}},
    {"texture_lod_bias", "Texture detail", "Below 0: finer mip levels, sharper textures in the distance (can shimmer). Above 0: blurrier.", "SSBU_TEXTURE_LOD_BIAS", "0", A_SAMPLER, 0,
     {"-1", "-0.5", "0", "0.5", "1"}, {"\xE2\x80\x93" "1", "\xE2\x80\x93" "0.5", "0 \xE2\x80\x94 game", "+0.5", "+1"}},
    {"aspect", "Aspect ratio", "Keep: the 16:9 picture keeps its shape, with black bars on a wider or taller window. Stretch: it fills the window.", NULL, "keep", A_LIVE, 0,
     {"keep", "stretch"}, {"Keep 16:9", "Stretch"}},
    {"button_prompts", "Button prompts", "Which buttons the game shows. Auto follows what you use, pad, keyboard or mouse. The others always show that set.", NULL, "auto", A_PROMPTS, 0,
     {"auto", "playstation", "xbox", "switch", "keyboard"}, {"Auto", "PlayStation", "Xbox", "Switch", "Keyboard"}},
    {"swap_confirm", "Swap Confirm", "Menus confirm with B and go back with A, and show it. Gameplay and the keyboard stay the same. This panel follows when it next opens.", NULL, "off", A_MENU_SWAP, 0,
     {"off", "on"}, {"Off", "On"}},
};
#define NROW ((int)(sizeof k_rows / sizeof k_rows[0]))

static SRWLOCK g_lock = SRWLOCK_INIT;
static volatile LONG g_open, g_work, g_job;
static volatile LONG g_release;                            /* a bit per npad id: that player still holds what was pressed in the menu */
static int g_sel, g_cur[NROW], g_start[NROW], g_shown[NROW], g_lock_why[NROW], g_inited, g_said_restart[NROW], g_nowin;
static char g_raw[NROW][32];                               /* a value of the file that is none of the row's: shown as it is */
static unsigned g_ver;
static int g_env_cut, g_env_set[NROW];
static int g_swap;                                         /* swap_confirm as this opening of the panel began: a change must not turn the press that made it into "close" */
/* the environment as the program started (gfx_options sets SSBU_CUTSCENE_FPS itself, later) */
__attribute__((constructor)) static void pcmenu_env(void) {
    for (int i = 0; i < NROW; i++) g_env_set[i] = k_rows[i].env && getenv(k_rows[i].env) != NULL;
}
static int row_count(int i) { int n = 0; while (n < ROW_VALUES && k_rows[i].val[n]) n++; return n; }
static int find_val(int i, const char *v) {
    for (int k = 0; k < row_count(i); k++) {
        if (!_stricmp(k_rows[i].val[k], v)) return k;
        char *e; double a = strtod(v, &e), b = atof(k_rows[i].val[k]);       /* numbers: 1 = 1.0, 0.50 = 0.5 */
        if (e != v && !*e && strspn(k_rows[i].val[k], "+-.0123456789") == strlen(k_rows[i].val[k]) && fabs(a - b) < 1e-6) return k;
    }
    return -1;
}
static void menu_init(void) {                              /* the first opening: the file's values = what runs */
    const GfxOptions *o = gfx_options();
    char v[64];
    for (int i = 0; i < NROW; i++) {
        const Row *r = &k_rows[i];
        int have = r->apply == A_SIZE || r->apply == A_PROMPTS || r->apply == A_MENU_SWAP || gfx_ini_get(r->key, v, sizeof v);
        if (r->apply == A_SIZE) snprintf(v, sizeof v, "%dx%d", o->width, o->height);      /* SSBU_WINDOW included */
        else if (r->apply == A_PROMPTS) snprintf(v, sizeof v, "%s", nnpad_prompts_setting());
        else if (r->apply == A_MENU_SWAP) snprintf(v, sizeof v, "%s", nnpad_menu_swap_setting());
        else if (!have) snprintf(v, sizeof v, "%s", r->dflt);
        g_shown[i] = !r->optional || have;
        if ((g_cur[i] = find_val(i, v)) < 0) snprintf(g_raw[i], sizeof g_raw[i], "%s", v);
        g_start[i] = g_cur[i];
        g_lock_why[i] = g_env_set[i] ? 1 : o->profile == 1 && (!strcmp(r->key, "vsync") || !strcmp(r->key, "resolution_scale") || !strcmp(r->key, "anisotropic_filtering")) ? 2 : 0;
    }
    if (!g_shown[g_sel]) g_sel = 0;
    g_inited = 1;
}

/* ---------------------------------------------------------------- drawing (BGRA, on the CPU) */
static stbtt_fontinfo g_font;
static uint8_t *g_px;                                      /* the panel */
static int g_pw, g_ph, g_rect[4], g_for_w, g_for_h;
static unsigned g_drawn_ver = ~0u, g_gen;
static int g_row_y[NROW], g_row_h, g_val_x0, g_val_x1, g_close_r[4];
typedef struct { uint8_t b, g, r; } Rgb;
static const Rgb C_BG0 = {26, 14, 18}, C_BG1 = {11, 7, 9}, C_GOLD = {96, 178, 214}, C_GOLD_HI = {140, 214, 244}, C_GOLD_DIM = {48, 88, 110},
                 C_TEXT = {214, 228, 238}, C_GREY = {150, 146, 152}, C_AMBER = {70, 160, 236}, C_LOCK = {130, 120, 120}, C_SEL0 = {26, 60, 86}, C_SEL1 = {14, 14, 18};
static void put(int x, int y, Rgb c, float a) {
    if (x < 0 || y < 0 || x >= g_pw || y >= g_ph || a <= 0) return;
    uint8_t *p = g_px + ((size_t)y * g_pw + x) * 4;
    if (a >= 1) { p[0] = c.b; p[1] = c.g; p[2] = c.r; p[3] = 255; return; }
    p[0] = (uint8_t)(p[0] + (c.b - p[0]) * a); p[1] = (uint8_t)(p[1] + (c.g - p[1]) * a); p[2] = (uint8_t)(p[2] + (c.r - p[2]) * a);
}
static Rgb mix(Rgb a, Rgb b, float t) { Rgb c = {(uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.r + (b.r - a.r) * t)}; return c; }
static void rect(int x, int y, int w, int h, Rgb c, float a) { for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) put(i, j, c, a); }
static void hgrad(int x, int y, int w, int h, Rgb a, Rgb b) { for (int i = 0; i < w; i++) rect(x + i, y, 1, h, mix(a, b, (float)i / (w > 1 ? w - 1 : 1)), 1); }
static void diamond(float cx, float cy, float r, Rgb c) {  /* a small gold lozenge, edge-smoothed */
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
        float d = fabsf(x + 0.5f - cx) + fabsf(y + 0.5f - cy);
        put(x, y, c, r - d + 0.5f > 1 ? 1 : r - d + 0.5f);
    }
}
static void disc(float cx, float cy, float r, Rgb c, float ring) {      /* ring > 0: only the outline */
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
        float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)), a = r - d + 0.5f;
        if (ring > 0) { float b = d - (r - ring) + 0.5f; a = a < b ? a : b; }
        put(x, y, c, a > 1 ? 1 : a);
    }
}
static int utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++;
    if (c >= 0xF0) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = (const char *)p;
    return c;
}
static float text_w(const char *s, float px, float track) {
    float sc = stbtt_ScaleForPixelHeight(&g_font, px), w = 0;
    int prev = 0, adv, lsb;
    while (*s) { int c = utf8(&s); if (prev) w += sc * stbtt_GetCodepointKernAdvance(&g_font, prev, c); stbtt_GetCodepointHMetrics(&g_font, c, &adv, &lsb); w += adv * sc + track; prev = c; }
    return w - (prev ? track : 0);
}
static float text(float x, int base, const char *s, float px, Rgb col, float track) {      /* returns the pen's end */
    static uint8_t buf[256 * 256];
    float sc = stbtt_ScaleForPixelHeight(&g_font, px);
    int prev = 0, adv, lsb;
    while (*s) {
        int c = utf8(&s), x0, y0, x1, y1;
        if (prev) x += sc * stbtt_GetCodepointKernAdvance(&g_font, prev, c);
        float fx = x - floorf(x);
        stbtt_GetCodepointBitmapBoxSubpixel(&g_font, c, sc, sc, fx, 0, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0 && w <= 256 && h <= 256) {
            stbtt_MakeCodepointBitmapSubpixel(&g_font, buf, w, h, w, sc, sc, fx, 0, c);
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) if (buf[j * w + i]) put((int)floorf(x) + x0 + i, base + y0 + j, col, buf[j * w + i] / 255.0f);
        }
        stbtt_GetCodepointHMetrics(&g_font, c, &adv, &lsb);
        x += adv * sc + track;
        prev = c;
    }
    return x;
}
static int wrap(char lines[][160], int maxl, const char *s, float px, float width) {      /* word wrap */
    int n = 0;
    while (*s && n < maxl) {
        char line[160] = "";
        const char *best = s;
        for (const char *p = s; ; p++) {
            if (*p == ' ' || !*p) {
                char t[160]; size_t len = (size_t)(p - s) < sizeof t - 1 ? (size_t)(p - s) : sizeof t - 1;
                memcpy(t, s, len); t[len] = 0;
                if (text_w(t, px, 0) > width && best != s) break;
                memcpy(line, t, len + 1); best = p;
                if (!*p) break;
            }
        }
        snprintf(lines[n++], 160, "%s", line);
        s = *best ? best + 1 : best;
    }
    return n;
}
static const char *val_name(int i) { return g_cur[i] >= 0 ? k_rows[i].name[g_cur[i]] : g_raw[i]; }
static int row_restart(int i) {                            /* a value that is not the running one yet */
    if (g_cur[i] == g_start[i]) return 0;
    return k_rows[i].apply == A_RESTART || (k_rows[i].apply == A_SCALE && g_open) || ((k_rows[i].apply == A_WINDOW || k_rows[i].apply == A_SIZE) && g_nowin);
}
static void button(float x, int base, float px, const char *letter, const char *what, Rgb ring) {
    float r = px * 0.62f, cx = x + r, cy = base - px * 0.36f;
    disc(cx, cy, r, ring, px * 0.09f);
    float lw = text_w(letter, px * 0.78f, 0);
    text(cx - lw / 2, (int)(cy + px * 0.28f), letter, px * 0.78f, ring, 0);
    text(x + 2 * r + px * 0.35f, base, what, px, C_GREY, 0);
}
static void draw_panel(int W, int H) {
    float s = (float)W / 1920.0f < (float)H / 1080.0f ? (float)W / 1920.0f : (float)H / 1080.0f;
    if (s < 0.45f) s = 0.45f;
    int nshow = 0;
    for (int i = 0; i < NROW; i++) nshow += g_shown[i];
    int pad = (int)(48 * s), rowh = (int)(58 * s), title_h = (int)(118 * s), help_h = (int)(116 * s), foot_h = (int)(78 * s);
    int pw = (int)(980 * s), ph = title_h + nshow * rowh + help_h + foot_h + pad;
    if (pw > W) pw = W;
    if (ph > H) ph = H;
    if (pw * ph != g_pw * g_ph) { free(g_px); g_px = malloc((size_t)pw * ph * 4); }
    g_pw = pw; g_ph = ph;
    if (!g_px) return;
    g_rect[0] = (W - pw) / 2; g_rect[1] = (H - ph) / 2; g_rect[2] = pw; g_rect[3] = ph;
    for (int y = 0; y < ph; y++) rect(0, y, pw, 1, mix(C_BG0, C_BG1, (float)y / ph), 1);
    int b = s > 1.4f ? 3 : 2, in = (int)(9 * s);              /* gold frame, a thin inner line, lozenges at the corners */
    rect(0, 0, pw, b, C_GOLD, 1); rect(0, ph - b, pw, b, C_GOLD, 1); rect(0, 0, b, ph, C_GOLD, 1); rect(pw - b, 0, b, ph, C_GOLD, 1);
    rect(in, in, pw - 2 * in, 1, C_GOLD_DIM, 1); rect(in, ph - in - 1, pw - 2 * in, 1, C_GOLD_DIM, 1); rect(in, in, 1, ph - 2 * in, C_GOLD_DIM, 1); rect(pw - in - 1, in, 1, ph - 2 * in, C_GOLD_DIM, 1);
    diamond((float)in, (float)in, 6 * s, C_GOLD); diamond((float)(pw - in - 1), (float)in, 6 * s, C_GOLD); diamond((float)in, (float)(ph - in - 1), 6 * s, C_GOLD); diamond((float)(pw - in - 1), (float)(ph - in - 1), 6 * s, C_GOLD);
    /* title */
    float tpx = 46 * s, tw = text_w("PC SETTINGS", tpx, 7 * s);
    int tb = (int)(74 * s);
    text((pw - tw) / 2, tb, "PC SETTINGS", tpx, C_GOLD_HI, 7 * s);
    int ly = (int)(98 * s);
    hgrad(pad, ly, pw / 2 - pad - (int)(14 * s), 1, C_BG0, C_GOLD); hgrad(pw / 2 + (int)(14 * s), ly, pw / 2 - pad - (int)(14 * s), 1, C_GOLD, C_BG0);
    diamond(pw / 2.0f, (float)ly, 7 * s, C_GOLD);
    /* rows */
    float rpx = 29 * s;
    int y = title_h;
    g_row_h = rowh; g_val_x0 = pw / 2; g_val_x1 = pw - pad;
    for (int i = 0; i < NROW; i++) {
        g_row_y[i] = -1;
        if (!g_shown[i]) continue;
        g_row_y[i] = y;
        int sel = i == g_sel, base = y + (int)(rowh * 0.66f);
        Rgb tc = g_lock_why[i] ? C_LOCK : sel ? C_TEXT : mix(C_TEXT, C_GREY, 0.35f);
        if (sel) { hgrad(pad - (int)(16 * s), y + (int)(4 * s), pw - 2 * pad + (int)(32 * s), rowh - (int)(8 * s), C_SEL0, C_SEL1); rect(pad - (int)(16 * s), y + (int)(4 * s), (int)(4 * s) > 2 ? (int)(4 * s) : 2, rowh - (int)(8 * s), C_GOLD_HI, 1); }
        text(pad + 10 * s, base, k_rows[i].label, rpx, tc, 0);
        const char *v = val_name(i);
        float vw = text_w(v, rpx, 0), cx = (g_val_x0 + g_val_x1) / 2.0f;
        Rgb vc = g_lock_why[i] ? C_LOCK : sel ? C_GOLD_HI : C_GOLD;
        text(cx - vw / 2, base, v, rpx, vc, 0);
        if (sel && !g_lock_why[i]) { text((float)g_val_x0, base, "\xE2\x80\xB9", rpx * 1.25f, C_GOLD_HI, 0); text(g_val_x1 - text_w("\xE2\x80\xBA", rpx * 1.25f, 0), base, "\xE2\x80\xBA", rpx * 1.25f, C_GOLD_HI, 0); }
        if (row_restart(i)) diamond(g_val_x0 - 18 * s, base - rpx * 0.33f, 6 * s, C_AMBER);
        y += rowh;
    }
    /* help and state of the chosen row */
    rect(pad, y + (int)(10 * s), pw - 2 * pad, 1, C_GOLD_DIM, 1);
    char lines[3][160];
    float hpx = 23 * s;
    int n = wrap(lines, 2, k_rows[g_sel].help, hpx, pw - 2 * pad - 20 * s), hy = y + (int)(46 * s);
    for (int k = 0; k < n; k++) text(pad + 10 * s, hy + k * (int)(30 * s), lines[k], hpx, C_GREY, 0);
    char st[160] = "";
    Rgb sc = C_AMBER;
    if (g_lock_why[g_sel] == 1) { snprintf(st, sizeof st, "Set by the environment variable %s", k_rows[g_sel].env); sc = C_LOCK; }
    else if (g_lock_why[g_sel] == 2) { snprintf(st, sizeof st, "Set by profile = low in graphics.ini"); sc = C_LOCK; }
    else if (row_restart(g_sel)) snprintf(st, sizeof st, "Saved \xE2\x80\x94 applies %s", k_rows[g_sel].apply == A_SCALE ? "when this panel closes" : "at the next start");
    else if (g_cur[g_sel] != g_start[g_sel]) { snprintf(st, sizeof st, "Applied and saved"); sc = C_GOLD; }
    if (st[0]) { diamond(pad + 16 * s, hy + 2 * (int)(30 * s) + 3 * s - hpx * 0.33f, 5 * s, sc); text(pad + 30 * s, hy + 2 * (int)(30 * s) + (int)(3 * s), st, hpx, sc, 0); }
    /* footer */
    int fy = ph - (int)(38 * s);
    float fpx = 22 * s, x = pad + 6 * s;
    rect(pad, ph - foot_h, pw - 2 * pad, 1, C_GOLD_DIM, 1);
    button(x, fy, fpx, g_swap ? "B" : "A", "Change", C_GOLD); x += text_w("Change", fpx, 0) + 66 * s;
    g_close_r[0] = (int)x; g_close_r[1] = fy - (int)(30 * s); g_close_r[3] = (int)(44 * s);
    button(x, fy, fpx, g_swap ? "A" : "B", "Close", C_GOLD); x += text_w("Close", fpx, 0) + 40 * s; g_close_r[2] = (int)x - g_close_r[0];
    const char *keys = "F1 \xC2\xB7 Start + Select";
    text(pw - pad - 6 * s - text_w(keys, fpx, 0), fy, keys, fpx, C_GOLD_DIM, 0);
}

/* The hint: a small box at the bottom right for a few seconds when the game's pause menu opens (nnfs.c sees it load), the
 * way into the game's own Options. The Options screen itself is not recognised (its UI is loaded once at the start). */
static volatile ULONGLONG g_hint_until;
static void draw_hint(int W, int H) {
    float s = (float)W / 1920.0f < (float)H / 1080.0f ? (float)W / 1920.0f : (float)H / 1080.0f;
    if (s < 0.45f) s = 0.45f;
    const char *t1 = "PC SETTINGS", *t2 = "F1  \xC2\xB7  Start + Select";
    float p1 = 21 * s, p2 = 19 * s, w1 = text_w(t1, p1, 3 * s), w2 = text_w(t2, p2, 0);
    int pw = (int)((w1 > w2 ? w1 : w2) + 64 * s), ph = (int)(78 * s), b = s > 1.4f ? 3 : 2;
    if (pw * ph != g_pw * g_ph) { free(g_px); g_px = malloc((size_t)pw * ph * 4); }
    g_pw = pw; g_ph = ph;
    if (!g_px) return;
    g_rect[0] = W - pw - (int)(40 * s); g_rect[1] = H - ph - (int)(96 * s); g_rect[2] = pw; g_rect[3] = ph;
    for (int y = 0; y < ph; y++) rect(0, y, pw, 1, mix(C_BG0, C_BG1, (float)y / ph), 1);
    rect(0, 0, pw, b, C_GOLD, 1); rect(0, ph - b, pw, b, C_GOLD, 1); rect(0, 0, b, ph, C_GOLD, 1); rect(pw - b, 0, b, ph, C_GOLD, 1);
    diamond(22 * s, 31 * s - p1 * 0.33f, 5 * s, C_GOLD);
    text(34 * s, (int)(31 * s), t1, p1, C_GOLD_HI, 3 * s);
    text(34 * s, (int)(60 * s), t2, p2, C_TEXT, 0);
}
static int hint_on(void) {
    ULONGLONG u = g_hint_until;
    if (!u) return 0;
    if (GetTickCount64() < u) return 1;
    g_hint_until = 0;
    return 0;
}
static DWORD WINAPI hint_late_job(LPVOID arg) { (void)arg; Sleep(500); nvn_front_post_pcmenu(); return 0; }
void pcmenu_hint(void) {                                   /* nnfs.c, a guest thread: the pause menu is opening */
    static int ms = -1, shown;
    static char when[16] = "first";                        /* graphics.ini pause_hint: first (the first pause of each start), always, off (issue #42) */
    if (ms < 0) {
        const char *e = getenv("SSBU_PCMENU_HINT");        /* seconds; 0 = never */
        ms = e ? atoi(e) * 1000 : 8000;
        gfx_ini_get("pause_hint", when, sizeof when);
        if (!_stricmp(when, "off")) ms = 0;
    }
    if (g_open || !ms || (shown && _stricmp(when, "always"))) return;
    shown = 1;
    if (!g_inited) { AcquireSRWLockExclusive(&g_lock); if (!g_inited && stbtt_InitFont(&g_font, k_font, stbtt_GetFontOffsetForIndex(k_font, 0))) menu_init(); ReleaseSRWLockExclusive(&g_lock); }
    if (!g_inited) return;
    g_hint_until = GetTickCount64() + (ULONGLONG)ms;
    printf("pcmenu: the game's pause menu opened: the hint is shown for %d s\n", ms / 1000);
    nvn_front_post_pcmenu();                               /* its pixels into the staging buffer; the game's presents copy them */
    if (getenv("SSBU_PCMENU_SHOT")) { HANDLE th = CreateThread(NULL, 0, hint_late_job, NULL, 0, NULL); if (th) CloseHandle(th); }      /* (writes the picture) */
}

/* render thread (nvn_vk_pcmenu.c): the panel (or the hint) for this window size, drawn again when something changed */
int pcmenu_is_open(void) { return g_open != 0; }
int pcmenu_visible(void) { return g_open || hint_on(); }
int pcmenu_panel_size(int W, int H, int r[4], unsigned *gen) {
    static int drawn_open = -1;
    int open = g_open != 0;
    if (!open && !hint_on()) return 0;
    AcquireSRWLockExclusive(&g_lock);
    if (g_drawn_ver != g_ver || g_for_w != W || g_for_h != H || drawn_open != open) {
        if (open) draw_panel(W, H); else draw_hint(W, H);
        g_drawn_ver = g_ver; g_for_w = W; g_for_h = H; drawn_open = open; g_gen++;
    }
    memcpy(r, g_rect, sizeof g_rect); *gen = g_gen;
    ReleaseSRWLockExclusive(&g_lock);
    return g_px != NULL;
}
void pcmenu_panel_read(void *dst, int w, int h) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_px && w == g_pw && h == g_ph) memcpy(dst, g_px, (size_t)w * h * 4);
    ReleaseSRWLockExclusive(&g_lock);
}
int pcmenu_take_work(void) { InterlockedExchange(&g_job, 0); return (int)InterlockedExchange(&g_work, 0); }

/* ---------------------------------------------------------------- changes */
static void add_work(LONG bit) { LONG o; do o = g_work; while (InterlockedCompareExchange(&g_work, o | bit, o) != o); }
static void set_value(int i, int k) {                    /* value k of row i: written, and applied where it can be */
    const Row *r = &k_rows[i];
    if (g_lock_why[i] || k < 0 || k >= row_count(i) || k == g_cur[i]) return;
    int ok;
    if (r->apply == A_SIZE) { char w[16], h[16]; sscanf(r->val[k], "%15[0-9]x%15[0-9]", w, h); ok = gfx_option_set("width", w) && gfx_option_set("height", h); }
    else if (r->apply == A_PROMPTS) ok = nnpad_prompts_set(r->val[k]);
    else if (r->apply == A_MENU_SWAP) ok = nnpad_menu_swap_set(r->val[k]);
    else ok = gfx_option_set(r->key, r->val[k]);
    if (!ok) return;
    g_cur[i] = k;
    switch (r->apply) {
    case A_CUT: game_cutscene_fps = atoi(r->val[k]) == 60 ? 60 : 30; break;
    case A_VSYNC: display_vsync_apply(); add_work(2); break;
    case A_WINDOW: case A_SIZE: if (!display_window_apply()) g_nowin = 1; break;
    case A_SAMPLER: add_work(1); break;
    case A_SCALE: if (!g_open) add_work(4); break;         /* (the panel: when it closes) */
    default: break;
    }
    printf("pcmenu: %s = %s (%s)\n", r->key, r->val[k], !row_restart(i) ? "applied" : r->apply == A_SCALE ? "applies when the panel closes" : "applies at the next start");
}
static void change(int i, int dir, int wrap) {             /* wrap 0 (left / right): stops at the ends; 1 (A, a click): round */
    int n = row_count(i);
    set_value(i, g_cur[i] < 0 ? (dir > 0 ? 0 : n - 1) : wrap ? (g_cur[i] + dir + n) % n : g_cur[i] + dir);
}
/* options_row.c, the game's main thread: the rows by key for the game's own Options screen. The value as the panel has it
 * (one of the row's spellings, else the file's text as it is) and whether it waits for the next start; a set goes through
 * set_value, so the file and what applies at once are the panel's. */
static int row_of(const char *key) { for (int i = 0; i < NROW; i++) if (!strcmp(k_rows[i].key, key)) return i; return -1; }
const char *pcmenu_key_get(const char *key, int *pending) {
    int i = row_of(key);
    if (i < 0) return "";
    AcquireSRWLockExclusive(&g_lock);
    if (!g_inited) menu_init();
    const char *v = g_cur[i] >= 0 ? k_rows[i].val[g_cur[i]] : g_raw[i];
    if (pending) *pending = row_restart(i);
    ReleaseSRWLockExclusive(&g_lock);
    return v;
}
void pcmenu_key_set(const char *key, const char *val) {
    int i = row_of(key);
    if (i < 0) return;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_inited) menu_init();
    int was = g_cur[i];
    set_value(i, find_val(i, val));
    if (g_cur[i] != was) g_ver++;
    else if (g_lock_why[i]) printf("pcmenu: %s is set by %s: not changed\n", key, g_lock_why[i] == 1 ? k_rows[i].env : "profile = low");
    ReleaseSRWLockExclusive(&g_lock);
    if (g_work && !g_open) nvn_front_post_pcmenu();        /* (outside the lock: the job takes it, and this may wait for room in the queue) */
}

/* ---------------------------------------------------------------- the menu's thread */
static int g_log = -1;
static void mouse_at(int *x, int *y, int *down) {          /* window pixels; -1 = not over the window */
    *x = *y = -1;
    *down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
#ifdef _WIN32
    POINT p; HWND h = GetForegroundWindow();
    if (h && GetCursorPos(&p) && ScreenToClient(h, &p)) { *x = p.x; *y = p.y; }
#else
    { extern int display_mouse_pixels(int *x, int *y); display_mouse_pixels(x, y); }
#endif
}
static DWORD WINAPI menu_thread(LPVOID arg) {
    (void)arg;
    int prev_dir = 0, prev_ok = 1, prev_back = 1, prev_click = 1, rep_at = 0, mx0 = -2, my0 = -2;
    ULONGLONG t_dir = 0;
    float sens; int inv;
    display_mouse_want(0);                                 /* the cursor back for the menu (the mouse camera lets it go) */
    while (g_open && !g_display_close) {
        uint64_t b; int lx, ly, mx, my, md;
        nnhid_menu_read(&b, &lx, &ly, g_swap);
        enum { BA = 1 << 0, BB = 1 << 1, BPLUS = 1 << 10, BMINUS = 1 << 11, BLEFT = 1 << 12, BUP = 1 << 13, BRIGHT = 1 << 14, BDOWN = 1 << 15 };
        int up = (b & BUP) || ly > 16000 || nnhid_menu_key(VK_UP), down = (b & BDOWN) || ly < -16000 || nnhid_menu_key(VK_DOWN);
        int left = (b & BLEFT) || lx < -16000 || nnhid_menu_key(VK_LEFT), right = (b & BRIGHT) || lx > 16000 || nnhid_menu_key(VK_RIGHT);
        int ok = (b & BA) || nnhid_menu_key(VK_RETURN) || nnhid_menu_key(VK_SPACE);
        int back = (b & BB) || nnhid_menu_key(VK_ESCAPE) || nnhid_menu_key(VK_BACK) || nnhid_menu_key(VK_F1) || (b & (BPLUS | BMINUS)) == (BPLUS | BMINUS);
        int dir = up ? 1 : down ? 2 : left ? 3 : right ? 4 : 0;
        ULONGLONG now = GetTickCount64();
        int step = dir && (dir != prev_dir || (rep_at && now >= t_dir + (ULONGLONG)rep_at));
        if (dir != prev_dir) { t_dir = now; rep_at = 380; } else if (step) { t_dir = now; rep_at = 90; }
        prev_dir = dir;
        mouse_at(&mx, &my, &md);
        AcquireSRWLockExclusive(&g_lock);
        int was_sel = g_sel, was_cur[NROW];
        memcpy(was_cur, g_cur, sizeof g_cur);
        if (step && (dir == 1 || dir == 2)) { do g_sel = (g_sel + (dir == 1 ? NROW - 1 : 1)) % NROW; while (!g_shown[g_sel]); }
        if (step && (dir == 3 || dir == 4)) change(g_sel, dir == 3 ? -1 : 1, 0);
        if (ok && !prev_ok) change(g_sel, 1, 1);
        int px = mx - g_rect[0], py = my - g_rect[1], click = md && !prev_click;
        if (mx >= 0 && (mx != mx0 || my != my0 || click) && px >= 0 && px < g_pw && py >= 0 && py < g_ph) {      /* the mouse: hover chooses, a click changes */
            for (int i = 0; i < NROW; i++) if (g_row_y[i] >= 0 && py >= g_row_y[i] && py < g_row_y[i] + g_row_h) { g_sel = i; if (click && px >= g_val_x0 - g_row_h) change(i, px < g_val_x0 + g_row_h ? -1 : 1, 1); }
            if (click && px >= g_close_r[0] && px < g_close_r[0] + g_close_r[2] && py >= g_close_r[1] && py < g_close_r[1] + g_close_r[3]) back = 1, prev_back = 0;
        }
        mx0 = mx; my0 = my;
        if (g_sel != was_sel || memcmp(was_cur, g_cur, sizeof g_cur)) g_ver++;
        ReleaseSRWLockExclusive(&g_lock);
        if (g_log && (step || (ok && !prev_ok) || (back && !prev_back) || click)) printf("pcmenu: input: %s%s%s%s -> row %d (%s)\n", step ? (dir == 1 ? "up " : dir == 2 ? "down " : dir == 3 ? "left " : "right ") : "", ok && !prev_ok ? "A " : "", back && !prev_back ? "B " : "", click ? "click " : "", g_sel, k_rows[g_sel].key);
        prev_ok = ok; prev_click = md;
        if (back && !prev_back) break;
        prev_back = back;
        if (!InterlockedExchange(&g_job, 1) && !nvn_front_post_pcmenu()) InterlockedExchange(&g_job, 0);
        Sleep(16);
    }
    InterlockedExchange(&g_release, 0xff);                 /* each player reads an idle pad until it has let go of everything */
    AcquireSRWLockExclusive(&g_lock);
    for (int i = 0; i < NROW; i++) if (k_rows[i].apply == A_SCALE) g_start[i] = g_cur[i];
    InterlockedExchange(&g_open, 0);
    ReleaseSRWLockExclusive(&g_lock);
    if (!g_display_close) { add_work(4); nvn_front_post_pcmenu(); }      /* before the game goes on: its next commands come after the job */
    InterlockedExchange(&g_guest_frozen, 0);
    if (nnpad_mouse_cfg(&sens, &inv)) display_mouse_want(1);
    printf("pcmenu: closed; the game goes on\n");
    fflush(stdout);
    return 0;
}
static void menu_open(const char *why) {
    if (InterlockedExchange(&g_open, 1)) return;
    g_hint_until = 0;
    if (g_log < 0) g_log = getenv("SSBU_PCMENU_LOG") && atoi(getenv("SSBU_PCMENU_LOG"));
    AcquireSRWLockExclusive(&g_lock);
    if (!g_font.data && !stbtt_InitFont(&g_font, k_font, stbtt_GetFontOffsetForIndex(k_font, 0))) { ReleaseSRWLockExclusive(&g_lock); InterlockedExchange(&g_open, 0); printf("pcmenu: the font cannot be read\n"); return; }
    if (!g_inited) menu_init();
    g_swap = nnpad_menu_swapped();
    g_ver++;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_guest_frozen, 1);               /* the game stops at its next SDK call (hostrt.c) */
    printf("pcmenu: opened (%s); the game is held\n", why);
    fflush(stdout);
    HANDLE th = CreateThread(NULL, 0, menu_thread, NULL, 0, NULL);
    if (th) CloseHandle(th); else { InterlockedExchange(&g_guest_frozen, 0); InterlockedExchange(&g_open, 0); }
}
void pcmenu_open_game(void) { menu_open("the game's Options: PC Settings"); }     /* options_row.c, the game's main thread */
/* nnhid.c, at every pad state the game reads: opens the menu, and says when the game gets an idle pad */
int pcmenu_gate(uint32_t id, uint64_t buttons) {
    static int prev = 1, test = -1;
    static ULONGLONG t0;
    LONG bit = id < 8 ? 1L << id : 0, o;
    if (g_open) return 1;
    if (id == 0) {
        int f1 = nnhid_menu_key(VK_F1), combo = (buttons & 0xc00) == 0xc00;      /* Start + Select (the game uses L3 and R3) */
        if (test < 0) { const char *e = getenv("SSBU_PCMENU_OPEN"); test = e ? atoi(e) : 0; t0 = GetTickCount64(); }
        if (test > 0 && GetTickCount64() - t0 >= (ULONGLONG)test * 1000) { test = 0; menu_open("SSBU_PCMENU_OPEN"); return 1; }
        if ((f1 || combo) && !prev && !(g_release & 1)) { prev = 1; menu_open(f1 ? "F1" : "Start + Select"); return 1; }
        prev = f1 || combo;
        if (prev && (g_release & 1)) return 1;             /* the F1 that closed it is still down */
    }
    if (!(g_release & bit)) return 0;
    if (buttons) return 1;
    do o = g_release; while (InterlockedCompareExchange(&g_release, o & ~bit, o) != o);
    if (g_log > 0) printf("pcmenu: player %u's input reaches the game again\n", id + 1);
    return 0;
}
