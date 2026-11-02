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
 * subsets of Marcellus and Source Sans 3 (SIL OFL 1.1, third_party/OFL-*.txt), embedded with #embed, the pictures from
 * runtime/art the same way; opaque, the whole window: the launcher's look (launcher/ui/witch.rcss). The same drawing
 * makes the first-start screen (draw_prep) for nvn_vk_draw.c.
 * Some rows are not graphics.ini's: button_prompts, swap_confirm and layout, of controllers.ini, shown by the game when it runs on (prompts.c).
 * A second page, Controls, has the buttons of gameplay by character (docs/RE_NOTES.md section 9): A on a row, then the button
 * to use for it, or left / right to go through them (for a player without a pad to press). A button already in use changes
 * places with the row's: a layout stays one button per action. From the game's Options, Controller, Controls it opens by itself.
 * Test aids: SSBU_PCMENU_SHOT=<folder> (nvn_vk_pcmenu.c), SSBU_PCMENU_OPEN=<seconds> opens it that long after the first pad
 * read (for scripted runs without keys), SSBU_PCMENU_LOG=1 logs each input step. */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <time.h>
#include "gfx_options.h"
#include "rebind.h"
#define STBTT_STATIC
#define STB_TRUETYPE_IMPLEMENTATION
#include "third_party/stb_truetype.h"

static const unsigned char k_font_head[] = {               /* Marcellus (SIL OFL 1.1, third_party/OFL-Marcellus.txt): titles */
#embed "third_party/Marcellus-pcmenu.ttf"
};
static const unsigned char k_font_ui[] = {                 /* Source Sans 3 Regular and SemiBold (SIL OFL 1.1, third_party/OFL-SourceSans3.txt) */
#embed "third_party/SourceSans3-pcmenu.ttf"
};
static const unsigned char k_font_uib[] = {
#embed "third_party/SourceSans3-SemiBold-pcmenu.ttf"
};

extern volatile LONG g_guest_frozen;                       /* hostrt.c */
extern volatile LONG g_display_close;                      /* display.c: the window is being closed (the game must run to exit) */
__declspec(dllimport) extern volatile int game_cutscene_fps;      /* game_shim_eh.c (the game library) */
void nnhid_menu_read(uint64_t *buttons, int *lx, int *ly, int swap_ab);       /* nnhid.c */
int nnhid_menu_key(int vk);
uint32_t nnhid_menu_pad(void);
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
const char *nnpad_layout_setting(void);
int nnpad_layout_set(const char *val);
int nnpad_button_prompts(void);
int nnpad_prompts_by_letter(void);
int nnpad_bind_get(int ch, unsigned char *out);
int nnpad_bind_set(int ch, const unsigned char *b);
int options_control_type(int ch);                          /* options_row.c */

/* A_PROMPTS, A_MENU_SWAP, A_LAYOUT: keys of controllers.ini. A_PAGE: a row that opens the Controls page; from A_CHAR on its rows */
enum { A_LIVE, A_CUT, A_VSYNC, A_WINDOW, A_SIZE, A_SAMPLER, A_SCALE, A_RESTART, A_PROMPTS, A_MENU_SWAP, A_LAYOUT, A_PAGE, A_CHAR, A_PRESET, A_BIND };
#define ROW_VALUES 12
typedef struct {
    const char *key, *label, *help, *env, *dflt;           /* dflt: the reader's value when the key is not in the file */
    int apply, optional;
    const char *val[ROW_VALUES], *name[ROW_VALUES];
    int slot;                                              /* A_BIND: the Pro button that does this in Type A */
} Row;
#define BIND(key, label, slot) {key, label, "A, then the button to use (it changes places with the one that had it). Left / right go through them.", NULL, NULL, A_BIND, 0, {0}, {0}, slot}
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
    {"layout", "Face buttons", "Position: the right button acts as A, as on a Switch. Printed letter: the button marked A, Cross as A on PlayStation pads. Prompts follow.", NULL, "auto", A_LAYOUT, 0,
     {"auto", "positional", "labels"}, {"Auto", "Position", "Printed letter"}},
    {"controls", "Controls", "Rebind every button of gameplay, for Bayonetta and for Viola. Menus keep theirs, and the game's prompts show yours.", NULL, "open", A_PAGE, 0,
     {"open"}, {"Open"}},
    {"bind_character", "Character", "Each character keeps a layout.", NULL, "bayonetta", A_CHAR, 0, {"bayonetta", "viola"}, {"Bayonetta", "Viola"}},
    {"bind_preset", "Preset", "The game's four control types, as a start. Game's setting: no layout, what the save had.", NULL, "game", A_PRESET, 0,
     {"game", "a", "b", "c", "d"}, {"Game's setting", "Type A", "Type B", "Type C", "Type D"}},
    BIND("bind_punch", "Punch", 2), BIND("bind_kick", "Kick", 0), BIND("bind_jump", "Jump", 1), BIND("bind_shoot", "Shoot", 3),
    BIND("bind_dodge", "Dodge", RB_ZR), BIND("bind_demon", "Demon Slave", RB_ZL), BIND("bind_weapon", "Weapon Switch", RB_L),
    BIND("bind_stance", "Stance Change", RB_R), BIND("bind_lock", "Camera / Lock On", 5), BIND("bind_way", "Waypoint", 4),
    BIND("bind_d1", "Demon Select, left", 12), BIND("bind_d2", "Demon Select, up", 13), BIND("bind_d3", "Demon Select, right", 14),
    BIND("bind_taunt", "Taunt", RB_DOWN), BIND("bind_sub", "Subscreen", 11), BIND("bind_pause", "Pause", 10),
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
static int g_page, g_direct;                               /* 1 = Controls, the rows from A_CHAR on; g_direct: opened there, so back closes */
static int g_ch, g_cap = -1, g_fam, g_letter;              /* the character shown; the row that waits for a button; the button names */
static uint8_t g_b[2][16];                                 /* the layouts shown: as saved, else the game's type */
static int g_b_on[2];                                      /* saved */
/* the environment as the program started (gfx_options sets SSBU_CUTSCENE_FPS itself, later) */
__attribute__((constructor)) static void pcmenu_env(void) {
    for (int i = 0; i < NROW; i++) g_env_set[i] = k_rows[i].env && getenv(k_rows[i].env) != NULL;
}
static int row_on(int i) { return g_shown[i] && g_page == (k_rows[i].apply >= A_CHAR); }
static int row_count(int i) { if (k_rows[i].apply == A_BIND) return 16; int n = 0; while (n < ROW_VALUES && k_rows[i].val[n]) n++; return n; }
static int find_val(int i, const char *v) {
    for (int k = 0; k < row_count(i); k++) {
        if (!_stricmp(k_rows[i].val[k], v)) return k;
        char *e; double a = strtod(v, &e), b = atof(k_rows[i].val[k]);       /* numbers: 1 = 1.0, 0.50 = 0.5 */
        if (e != v && !*e && strspn(k_rows[i].val[k], "+-.0123456789") == strlen(k_rows[i].val[k]) && fabs(a - b) < 1e-6) return k;
    }
    return -1;
}
static void binds_show(void) {                             /* the Controls rows from the layouts */
    for (int i = 0; i < NROW; i++) {
        const Row *r = &k_rows[i];
        if (r->apply == A_CHAR) g_cur[i] = g_ch;
        else if (r->apply == A_BIND) g_cur[i] = g_b[g_ch][r->slot];
        else if (r->apply == A_PRESET) {
            g_cur[i] = g_b_on[g_ch] ? -1 : 0;
            for (int t = 0; t < 4 && g_cur[i] < 0; t++) { uint8_t p[16]; rb_preset(g_ch, t, p); if (!memcmp(p, g_b[g_ch], 16)) g_cur[i] = t + 1; }
            if (g_cur[i] < 0) snprintf(g_raw[i], sizeof g_raw[i], "Custom");
        } else continue;
        g_start[i] = g_cur[i];
    }
}
static void binds_load(void) {
    for (int ch = 0; ch < 2; ch++) if (!(g_b_on[ch] = nnpad_bind_get(ch, g_b[ch]))) rb_preset(ch, options_control_type(ch), g_b[ch]);
    binds_show();
}
static int bind_assign(int slot, int button) {             /* the button that had it takes the row's old one */
    uint8_t *r = g_b[g_ch];
    for (int b = 0; b < 16; b++) if (r[b] == button) r[b] = r[slot];
    r[slot] = (uint8_t)button;
    g_b_on[g_ch] = 1;
    return nnpad_bind_set(g_ch, r);
}
static void page_go(int page) {
    g_page = page; g_cap = -1;
    g_sel = 0;
    while (g_sel < NROW - 1 && !row_on(g_sel)) g_sel++;
}
static void menu_init(void) {                              /* the first opening: the file's values = what runs */
    const GfxOptions *o = gfx_options();
    char v[64];
    for (int i = 0; i < NROW; i++) {
        const Row *r = &k_rows[i];
        int have = r->apply == A_SIZE || r->apply == A_PROMPTS || r->apply == A_MENU_SWAP || r->apply >= A_LAYOUT || gfx_ini_get(r->key, v, sizeof v);
        if (r->apply == A_SIZE) snprintf(v, sizeof v, "%dx%d", o->width, o->height);      /* SSBU_WINDOW included */
        else if (r->apply == A_PROMPTS) snprintf(v, sizeof v, "%s", nnpad_prompts_setting());
        else if (r->apply == A_MENU_SWAP) snprintf(v, sizeof v, "%s", nnpad_menu_swap_setting());
        else if (r->apply == A_LAYOUT) snprintf(v, sizeof v, "%s", nnpad_layout_setting());
        else if (r->apply == A_BIND) { g_shown[i] = 1; continue; }
        else if (!have || r->apply >= A_PAGE) snprintf(v, sizeof v, "%s", r->dflt);
        g_shown[i] = !r->optional || have;
        if ((g_cur[i] = find_val(i, v)) < 0) snprintf(g_raw[i], sizeof g_raw[i], "%s", v);
        g_start[i] = g_cur[i];
        g_lock_why[i] = g_env_set[i] ? 1 : o->profile == 1 && (!strcmp(r->key, "vsync") || !strcmp(r->key, "resolution_scale") || !strcmp(r->key, "anisotropic_filtering")) ? 2 : 0;
    }
    if (!g_shown[g_sel]) g_sel = 0;
    g_inited = 1;
}

/* ---------------------------------------------------------------- drawing (BGRA, on the CPU) */
/* The look (the launcher's too, launcher/ui/witch.rcss; the design page in the project files): a pearl ground crossed by an
 * indigo panel on a 22 degree diagonal with a runic stripe on its edge, settings as a staircase of rows stepping down the
 * diagonal, the chosen row a violet glow, a clockwork wheel behind, a dark message band and a pale hint band at the bottom.
 * Everything is laid out for 1280 x 720 and scaled by s = min(W / 1280, H / 720); the rows keep to the right edge. */
static stbtt_fontinfo g_f_head, g_f_ui, g_f_uib;          /* Marcellus (titles); Source Sans 3 Regular and SemiBold */
static uint8_t *g_px;                                      /* the panel */
static int g_pw, g_ph, g_rect[4], g_for_w, g_for_h;
/* A layer: the same size as the panel, premultiplied BGRA, with the box that was drawn in. While g_to is set, put() and
 * put_pre() draw into it instead of the panel; layer_over() composes it onto the panel. The panel's moving parts (the
 * wheels) are drawn each frame between a cached ground and the layers drawn once: the hubs, then the words. */
typedef struct { uint8_t *px; int n, x0, y0, x1, y1; } Layer;              /* n: the pixels allocated */
static Layer *g_to;
static void layer_clear(Layer *l) {
    if (!l->px || l->n != g_pw * g_ph) { free(l->px); l->px = calloc((size_t)g_pw * g_ph, 4); l->n = g_pw * g_ph; }
    else memset(l->px, 0, (size_t)g_pw * g_ph * 4);
    l->x0 = g_pw; l->y0 = g_ph; l->x1 = l->y1 = 0;
}
static void layer_free(Layer *l) { free(l->px); l->px = NULL; l->n = 0; }
static void layer_over(const Layer *l) {
    if (!l->px) return;
    for (int j = l->y0; j < l->y1; j++) for (int i = l->x0; i < l->x1; i++) {
        const uint8_t *q = l->px + ((size_t)j * g_pw + i) * 4;
        if (!q[3]) continue;
        uint8_t *p = g_px + ((size_t)j * g_pw + i) * 4;
        float k = 1 - q[3] / 255.0f;
        p[0] = (uint8_t)(p[0] * k + q[0] + 0.5f); p[1] = (uint8_t)(p[1] * k + q[1] + 0.5f); p[2] = (uint8_t)(p[2] * k + q[2] + 0.5f);
    }
}
static void layer_blend(int x, int y, float b, float g, float r, float a) {   /* a premultiplied source over the layer */
    Layer *l = g_to;
    uint8_t *p = l->px + ((size_t)y * g_pw + x) * 4;
    float k = 1 - a;
    p[0] = (uint8_t)(p[0] * k + b + 0.5f); p[1] = (uint8_t)(p[1] * k + g + 0.5f); p[2] = (uint8_t)(p[2] * k + r + 0.5f); p[3] = (uint8_t)(p[3] * k + a * 255 + 0.5f);
    if (x < l->x0) l->x0 = x; if (y < l->y0) l->y0 = y; if (x + 1 > l->x1) l->x1 = x + 1; if (y + 1 > l->y1) l->y1 = y + 1;
}
static unsigned g_drawn_ver = ~0u, g_gen;
static int g_row_y[NROW], g_row_h, g_val_x0[NROW], g_val_x1[NROW], g_close_r[4];
typedef struct { uint8_t b, g, r; } Rgb;
static const Rgb C_PEARL = {242, 237, 238}, C_PEARL_LO = {218, 209, 211}, C_INK = {46, 32, 36}, C_MUTE = {104, 88, 93},
                 C_INDIGO = {40, 22, 27}, C_INDIGO0 = {28, 15, 18}, C_INDIGO1 = {54, 27, 34}, C_INDIGO2 = {66, 33, 42}, C_STRIPE0 = {72, 33, 43}, C_STRIPE1 = {98, 45, 58},
                 C_VIOLET = {208, 73, 106}, C_VIOLET0 = {196, 60, 90}, C_LAV = {236, 144, 166}, C_LAV2 = {230, 117, 143}, C_LILAC = {247, 204, 215},
                 C_WHITE = {255, 255, 255}, C_RED = {75, 51, 229}, C_BAND = {32, 18, 22}, C_FOOT = {222, 212, 214}, C_FOOT_DK = {44, 22, 28};
#define TAN22 0.4040f                                      /* the diagonal: 22 degrees from the vertical */
static int fonts_init(void) {
    return stbtt_InitFont(&g_f_head, k_font_head, stbtt_GetFontOffsetForIndex(k_font_head, 0)) &&
           stbtt_InitFont(&g_f_ui, k_font_ui, stbtt_GetFontOffsetForIndex(k_font_ui, 0)) &&
           stbtt_InitFont(&g_f_uib, k_font_uib, stbtt_GetFontOffsetForIndex(k_font_uib, 0));
}
static void put(int x, int y, Rgb c, float a) {
    if (x < 0 || y < 0 || x >= g_pw || y >= g_ph || a <= 0) return;
    if (g_to) { if (a > 1) a = 1; layer_blend(x, y, c.b * a, c.g * a, c.r * a, a); return; }
    uint8_t *p = g_px + ((size_t)y * g_pw + x) * 4;
    if (a >= 1) { p[0] = c.b; p[1] = c.g; p[2] = c.r; p[3] = 255; return; }
    p[0] = (uint8_t)(p[0] + (c.b - p[0]) * a); p[1] = (uint8_t)(p[1] + (c.g - p[1]) * a); p[2] = (uint8_t)(p[2] + (c.r - p[2]) * a);
}
static Rgb mix(Rgb a, Rgb b, float t) { Rgb c = {(uint8_t)(a.b + (b.b - a.b) * t), (uint8_t)(a.g + (b.g - a.g) * t), (uint8_t)(a.r + (b.r - a.r) * t)}; return c; }
static void rect(int x, int y, int w, int h, Rgb c, float a) { for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) put(i, j, c, a); }
static void hgrad(int x, int y, int w, int h, Rgb a, Rgb b) { for (int i = 0; i < w; i++) rect(x + i, y, 1, h, mix(a, b, (float)i / (w > 1 ? w - 1 : 1)), 1); }
static void hgrad_a(int x, int y, int w, int h, Rgb a, Rgb b, float a0, float a1) {      /* with the alpha fading too */
    for (int i = 0; i < w; i++) { float t = (float)i / (w > 1 ? w - 1 : 1); rect(x + i, y, 1, h, mix(a, b, t), a0 + (a1 - a0) * t); }
}
/* a parallelogram leaning 22 degrees, skewed about its bottom left corner (row j of h is shifted right by (h - j) * TAN22),
 * filled left to right with c0 -> c1 -> c2 (c1 at t1) and the alpha a0 -> a2 */
static void para(float x, float y, float w, float h, Rgb c0, Rgb c1, Rgb c2, float t1, float a0, float a2) {
    for (int j = (int)y; j < (int)(y + h); j++) {
        float off = (y + h - j) * TAN22, x0 = x + off;
        int i0 = (int)floorf(x0), i1 = (int)ceilf(x0 + w);
        for (int i = i0; i < i1; i++) {
            float t = (i + 0.5f - x0) / w, cov = 1;
            if (t < 0 || t > 1) { cov = t < 0 ? 1 + t * w : 1 - (t - 1) * w; if (cov <= 0) continue; t = t < 0 ? 0 : 1; }      /* the slanted ends smoothed */
            Rgb c = t < t1 ? mix(c0, c1, t1 > 0 ? t / t1 : 1) : mix(c1, c2, t1 < 1 ? (t - t1) / (1 - t1) : 1);
            put(i, j, c, (a0 + (a2 - a0) * t) * cov);
        }
    }
}
/* the indigo panel: its right edge runs from x_top at the top of the window down to the right at 22 degrees (as the
 * launcher's skewX from the top left corner); filled from x_left with c0 -> c1 -> c2 over w */
static void wedge(float x_left, float w, float x_top, int H, Rgb c0, Rgb c1, Rgb c2, float t1) {
    for (int j = 0; j < H; j++) {
        float e = x_top + j * TAN22;
        int i1 = (int)ceilf(e);
        for (int i = (int)x_left < 0 ? 0 : (int)x_left; i < i1 && i < g_pw; i++) {
            float t = (i + 0.5f - x_left) / w, cov = e - i; if (t > 1) t = 1; if (t < 0) t = 0;
            put(i, j, t < t1 ? mix(c0, c1, t / t1) : mix(c1, c2, (t - t1) / (1 - t1)), cov > 1 ? 1 : cov);
        }
    }
}
static void slant_line(float x_top, int H, float w, Rgb c, float a) {      /* a thin line down the diagonal */
    for (int j = 0; j < H; j++) { float x = x_top + j * TAN22; int i = (int)floorf(x); float f = x - i; put(i, j, c, a * (1 - f) * w); put(i + 1, j, c, a * f * w); if (w > 1) for (int k = 1; k < (int)w; k++) put(i + k, j, c, a); }
}
static void disc(float cx, float cy, float r, Rgb c, float ring) {      /* ring > 0: only the outline */
    for (int y = (int)(cy - r - 1); y <= (int)(cy + r + 1); y++) for (int x = (int)(cx - r - 1); x <= (int)(cx + r + 1); x++) {
        float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)), a = r - d + 0.5f;
        if (ring > 0) { float b = d - (r - ring) + 0.5f; a = a < b ? a : b; }
        put(x, y, c, a > 1 ? 1 : a);
    }
}
static void soft_disc(float cx, float cy, float r, Rgb c, float a) {     /* a glow: c fading out from the centre */
    for (int y = (int)(cy - r); y <= (int)(cy + r); y++) for (int x = (int)(cx - r); x <= (int)(cx + r); x++) {
        float d = sqrtf((x + 0.5f - cx) * (x + 0.5f - cx) + (y + 0.5f - cy) * (y + 0.5f - cy)) / r;
        if (d < 1) put(x, y, c, a * (1 - d) * (1 - d));
    }
}

/* ---- the art (runtime/art, made by the design's scripts): 8-bit BMPs whose palette carries alpha in its fourth byte,
 * colours premultiplied. The wheel in the pearl side's grey and in the indigo side's lilac, the two hands, the stripe. ---- */
typedef struct { int w, h, stride; const uint8_t *pal, *px; int bx0, by0, bx1, by1; } Art;      /* px: the bottom row first, as a BMP is; b: the box with alpha */
static Art art_load(const unsigned char *b, size_t n) {
    Art a = {0};
    if (n < 54 + 1024 || b[0] != 'B' || b[1] != 'M') return a;
    uint32_t hs = b[14] | b[15] << 8 | b[16] << 16 | (uint32_t)b[17] << 24, off = b[10] | b[11] << 8 | b[12] << 16 | (uint32_t)b[13] << 24;
    a.w = (int)(b[18] | b[19] << 8 | b[20] << 16 | (uint32_t)b[21] << 24); a.h = (int)(b[22] | b[23] << 8 | b[24] << 16 | (uint32_t)b[25] << 24);
    a.stride = (a.w + 3) & ~3; a.pal = b + 14 + hs; a.px = b + off;
    if (a.w <= 0 || a.h <= 0 || off + (size_t)a.stride * a.h > n) a.w = 0;
    a.bx0 = a.w; a.by0 = a.h; a.bx1 = a.by1 = 0;
    for (int y = 0; y < a.h; y++) for (int x = 0; x < a.w; x++)
        if (a.pal[(size_t)a.px[(size_t)(a.h - 1 - y) * a.stride + x] * 4 + 3]) { if (x < a.bx0) a.bx0 = x; if (y < a.by0) a.by0 = y; if (x + 1 > a.bx1) a.bx1 = x + 1; if (y + 1 > a.by1) a.by1 = y + 1; }
    if (a.bx1 <= a.bx0) a.w = 0;
    return a;
}
static void art_texel(const Art *a, int x, int y, float out[4]) {       /* B G R A premultiplied, 0..255; outside: nothing */
    if (x < 0 || y < 0 || x >= a->w || y >= a->h) { out[0] = out[1] = out[2] = out[3] = 0; return; }
    const uint8_t *e = a->pal + (size_t)a->px[(size_t)(a->h - 1 - y) * a->stride + x] * 4;
    out[0] = e[0]; out[1] = e[1]; out[2] = e[2]; out[3] = e[3];
}
static const uint8_t k_clear[4];
static inline const uint8_t *art_entry(const Art *a, int x, int y) {    /* the palette entry of a texel; clear outside */
    if (x < 0 || y < 0 || x >= a->w || y >= a->h) return k_clear;
    return a->pal + (size_t)a->px[(size_t)(a->h - 1 - y) * a->stride + x] * 4;
}
static inline void art_sample(const Art *a, float u, float v, float out[4]) {  /* bilinear at source coordinates (u, v) */
    float fu = u - 0.5f, fv = v - 0.5f;
    int x = (int)floorf(fu), y = (int)floorf(fv);
    float fx = fu - x, fy = fv - y;
    const uint8_t *e00 = art_entry(a, x, y), *e10 = art_entry(a, x + 1, y), *e01 = art_entry(a, x, y + 1), *e11 = art_entry(a, x + 1, y + 1);
    if (!(e00[3] | e10[3] | e01[3] | e11[3])) { out[3] = 0; return; }   /* nothing here: most of a gear's box, a hand's */
    float w00 = (1 - fx) * (1 - fy), w10 = fx * (1 - fy), w01 = (1 - fx) * fy, w11 = fx * fy;
    for (int k = 0; k < 4; k++) out[k] = e00[k] * w00 + e10[k] * w10 + e01[k] * w01 + e11[k] * w11;
}
static void put_pre(int x, int y, const float c[4], float a) {          /* premultiplied source over the opaque panel */
    if (x < 0 || y < 0 || x >= g_pw || y >= g_ph) return;
    if (g_to) { layer_blend(x, y, c[0] * a, c[1] * a, c[2] * a, c[3] * a / 255.0f); return; }
    uint8_t *p = g_px + ((size_t)y * g_pw + x) * 4;
    float k = 1 - c[3] * a / 255.0f;
    p[0] = (uint8_t)(p[0] * k + c[0] * a + 0.5f); p[1] = (uint8_t)(p[1] * k + c[1] * a + 0.5f); p[2] = (uint8_t)(p[2] * k + c[2] * a + 0.5f);
}
/* the picture fitted to the box (x, y, w, h), turned by `deg` about the box's centre, at the opacity `alpha`; on the window,
 * only where x < xmax(y) when clip_top >= 0 (a slanted edge: xmax = clip_top + y * TAN22) */
static float g_ann0, g_ann1;                                /* set: only the ring between these fractions of the box's half is drawn */
static int g_clip_right;                                     /* set: clip_top keeps what is right of the slanted edge instead */
/* The GPU path (nvn_vk_pcmenu.c, pcmenu_gpu): the ground and the layers are still drawn here, once a version; the moving
 * parts are not drawn but listed as sprites (PcmSprite: the same box, turn, opacity, ring and slanted clip as art_blit's)
 * for the renderer to draw every frame with the art as textures. While g_emit is set, art_blit lists instead of drawing. */
typedef struct { int art; float x, y, w, h, deg, alpha, ann0, ann1, clip_top; int clip_right; } PcmSprite;      /* art: pcmenu_art_pixels' id; -1 the hubs layer, -2 the words layer */
static PcmSprite *g_emit; static int g_emit_n, g_emit_max, g_gpu;
static int art_id(const Art *a);
static void art_blit(const Art *a, float x, float y, float w, float h, float alpha, float deg, float clip_top) {
    if (!a->w) return;
    if (g_emit) {
        if (g_emit_n < g_emit_max) { PcmSprite e = {art_id(a), x, y, w, h, deg, alpha, g_ann0, g_ann1, clip_top, g_clip_right}; g_emit[g_emit_n++] = e; }
        return;
    }
    float cx = x + w / 2, cy = y + h / 2, cs = cosf(deg * 3.14159265f / 180), sn = sinf(deg * 3.14159265f / 180), sx = a->w / w, sy = a->h / h;
    /* the window's box of the picture's alpha box, turned */
    float bx[2] = {x + a->bx0 / sx, x + a->bx1 / sx}, by[2] = {y + a->by0 / sy, y + a->by1 / sy}, fx0 = 1e9f, fy0 = 1e9f, fx1 = -1e9f, fy1 = -1e9f;
    for (int k = 0; k < 4; k++) {
        float dx = bx[k & 1] - cx, dy = by[k >> 1] - cy, rx = cx + dx * cs - dy * sn, ry = cy + dx * sn + dy * cs;
        if (rx < fx0) fx0 = rx; if (rx > fx1) fx1 = rx; if (ry < fy0) fy0 = ry; if (ry > fy1) fy1 = ry;
    }
    int x0 = (int)floorf(fx0) - 1, y0 = (int)floorf(fy0) - 1, x1 = (int)ceilf(fx1) + 1, y1 = (int)ceilf(fy1) + 1;
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0; if (x1 > g_pw) x1 = g_pw; if (y1 > g_ph) y1 = g_ph;
    float r0 = g_ann0 * w / 2, r1 = g_ann1 * w / 2;
    for (int j = y0; j < y1; j++) {
        int jx0 = x0, jx1 = x1;
        if (clip_top >= 0) { int e = (int)(clip_top + j * TAN22); if (g_clip_right) { if (e > jx0) jx0 = e; } else if (e < jx1) jx1 = e; }
        for (int i = jx0; i < jx1; i++) {
            float dx = i + 0.5f - cx, dy = j + 0.5f - cy;
            if (g_ann1 > 0) { float rr = dx * dx + dy * dy; if (rr < r0 * r0 || rr > r1 * r1) continue; }
            float u = (dx * cs + dy * sn + w / 2) * sx, v = (-dx * sn + dy * cs + h / 2) * sy, c[4];
            if (u < 0 || v < 0 || u >= a->w || v >= a->h) continue;
            art_sample(a, u, v, c);
            if (c[3] > 0) put_pre(i, j, c, alpha);
        }
    }
}
/* the runic stripe: the text picture down the slanted band (x_top, w) over the window's height */
static void stripe_text(const Art *a, float x_top, float w, int H, float alpha) {
    if (!a->w) return;
    for (int j = 0; j < H; j++) {
        float e = x_top + j * TAN22;
        for (int i = (int)e; i < (int)(e + w) + 1; i++) {
            float u = (i + 0.5f - e) / w * a->w, v = (float)j / H * a->h, c[4];
            if (u < 0 || u >= a->w) continue;
            art_sample(a, u, v, c);
            if (c[3] > 0) put_pre(i, j, c, alpha);
        }
    }
}
/* the wheel in the design page's layers: two rings, the base, three gears, the hub; a light set and a dark one */
static const unsigned char k_wl_ring1[] = {
#embed "art/wheel-ring1.bmp"
};
static const unsigned char k_wl_ring2[] = {
#embed "art/wheel-ring2.bmp"
};
static const unsigned char k_wl_base[] = {
#embed "art/wheel-base.bmp"
};
static const unsigned char k_wl_gear1[] = {
#embed "art/wheel-gear1.bmp"
};
static const unsigned char k_wl_gear2[] = {
#embed "art/wheel-gear2.bmp"
};
static const unsigned char k_wl_gear3[] = {
#embed "art/wheel-gear3.bmp"
};
static const unsigned char k_wl_top[] = {
#embed "art/wheel-top.bmp"
};
static const unsigned char k_wd_ring1[] = {
#embed "art/wheel-dark-ring1.bmp"
};
static const unsigned char k_wd_ring2[] = {
#embed "art/wheel-dark-ring2.bmp"
};
static const unsigned char k_wd_base[] = {
#embed "art/wheel-dark-base.bmp"
};
static const unsigned char k_wd_gear1[] = {
#embed "art/wheel-dark-gear1.bmp"
};
static const unsigned char k_wd_gear2[] = {
#embed "art/wheel-dark-gear2.bmp"
};
static const unsigned char k_wd_gear3[] = {
#embed "art/wheel-dark-gear3.bmp"
};
static const unsigned char k_wd_top[] = {
#embed "art/wheel-dark-top.bmp"
};
static const unsigned char k_art_hand_minute[] = {
#embed "art/hand-minute.bmp"
};
static const unsigned char k_art_hand_hour[] = {
#embed "art/hand-hour.bmp"
};
static const unsigned char k_art_stripe[] = {
#embed "art/stripe.bmp"
};
typedef struct { Art ring1, ring2, base, gear1, gear2, gear3, top; } Wheel;
static Wheel g_wheel, g_wheel_dark;
static Art g_hand_minute, g_hand_hour, g_stripe;
static void art_init(void) {
    if (g_wheel.base.w) return;
#define LOAD(dst, src) dst = art_load(src, sizeof src)
    LOAD(g_wheel.ring1, k_wl_ring1); LOAD(g_wheel.ring2, k_wl_ring2); LOAD(g_wheel.base, k_wl_base); LOAD(g_wheel.top, k_wl_top);
    LOAD(g_wheel.gear1, k_wl_gear1); LOAD(g_wheel.gear2, k_wl_gear2); LOAD(g_wheel.gear3, k_wl_gear3);
    LOAD(g_wheel_dark.ring1, k_wd_ring1); LOAD(g_wheel_dark.ring2, k_wd_ring2); LOAD(g_wheel_dark.base, k_wd_base); LOAD(g_wheel_dark.top, k_wd_top);
    LOAD(g_wheel_dark.gear1, k_wd_gear1); LOAD(g_wheel_dark.gear2, k_wd_gear2); LOAD(g_wheel_dark.gear3, k_wd_gear3);
#undef LOAD
    g_hand_minute = art_load(k_art_hand_minute, sizeof k_art_hand_minute); g_hand_hour = art_load(k_art_hand_hour, sizeof k_art_hand_hour);
    g_stripe = art_load(k_art_stripe, sizeof k_art_stripe);
}
static int art_id(const Art *a) {                           /* the art's number for the GPU path: the light wheel's 7 layers, the dark wheel's, the two hands */
    const Art *k[16] = {&g_wheel.ring1, &g_wheel.ring2, &g_wheel.base, &g_wheel.gear1, &g_wheel.gear2, &g_wheel.gear3, &g_wheel.top,
                        &g_wheel_dark.ring1, &g_wheel_dark.ring2, &g_wheel_dark.base, &g_wheel_dark.gear1, &g_wheel_dark.gear2, &g_wheel_dark.gear3, &g_wheel_dark.top, &g_hand_minute, &g_hand_hour};
    for (int i = 0; i < 16; i++) if (k[i] == a) return i;
    return -3;
}
/* the art as premultiplied BGRA, the top row first, for the renderer's textures (malloc'd; NULL: no such art) */
void *pcmenu_art_pixels(int id, int *w, int *h) {
    const Art *k[16] = {&g_wheel.ring1, &g_wheel.ring2, &g_wheel.base, &g_wheel.gear1, &g_wheel.gear2, &g_wheel.gear3, &g_wheel.top,
                        &g_wheel_dark.ring1, &g_wheel_dark.ring2, &g_wheel_dark.base, &g_wheel_dark.gear1, &g_wheel_dark.gear2, &g_wheel_dark.gear3, &g_wheel_dark.top, &g_hand_minute, &g_hand_hour};
    art_init();
    if (id < 0 || id >= 16 || !k[id]->w) return NULL;
    const Art *a = k[id];
    uint8_t *px = malloc((size_t)a->w * a->h * 4);
    if (!px) return NULL;
    for (int y = 0; y < a->h; y++) for (int x = 0; x < a->w; x++) memcpy(px + ((size_t)y * a->w + x) * 4, art_entry(a, x, y), 4);
    *w = a->w; *h = a->h;
    return px;
}
/* The wheel of radius R at (cx, cy), in three passes, as the launcher's layers: wheel_still draws the base once into the
 * ground; wheel_turn draws, each frame, the rings (once round in 120 s, the inner one the other way in 160 s), the gears
 * (10 s and 14 s, about their own centres) and the hands at this PC's local time; wheel_hub draws the hub (into a layer
 * composed over the moving parts). The gears' centres and size are fractions of the 800 px drawing. */
static void wheel_still(const Wheel *w, float cx, float cy, float R, float alpha, float clip_top) { art_blit(&w->base, cx - R, cy - R, 2 * R, 2 * R, alpha, 0, clip_top); }
static void wheel_hub(const Wheel *w, float cx, float cy, float R, float alpha, float clip_top) { art_blit(&w->top, cx - R, cy - R, 2 * R, 2 * R, alpha, 0, clip_top); }
static void wheel_turn(const Wheel *w, float cx, float cy, float R, float alpha, int hands, float clip_top, double t) {
    float deg = (float)fmod(t * 3, 360), deg2 = (float)fmod(t * 360 / 160, 360), dg1 = (float)fmod(t * 36, 360), dg2 = (float)fmod(t * 360 / 14, 360);
    g_ann0 = 0.78f; g_ann1 = 0.985f; art_blit(&w->ring1, cx - R, cy - R, 2 * R, 2 * R, alpha, deg, clip_top);
    g_ann0 = 0.61f; g_ann1 = 0.73f; art_blit(&w->ring2, cx - R, cy - R, 2 * R, 2 * R, alpha, -deg2, clip_top);
    g_ann0 = g_ann1 = 0;
    static const float gc[3][2] = {{0.3726f, 0.3481f}, {0.6274f, 0.6519f}, {0.7254f, 0.4608f}};
    const Art *g[3] = {&w->gear1, &w->gear2, &w->gear3};
    float gs = 0.2f * 2 * R;
    for (int k = 0; k < 3; k++) art_blit(g[k], cx - R + gc[k][0] * 2 * R - gs / 2, cy - R + gc[k][1] * 2 * R - gs / 2, gs, gs, alpha, k == 0 ? -dg1 : dg2, clip_top);
    if (!hands) return;
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);
    float m = tm ? tm->tm_min + tm->tm_sec / 60.0f : 0, h = tm ? (tm->tm_hour % 12) + m / 60 : 0, hb = 2 * R;      /* the hand pictures are drawn in the wheel's box */
    art_blit(&g_hand_hour, cx - hb / 2, cy - hb / 2, hb, hb, alpha, h * 30, clip_top);
    art_blit(&g_hand_minute, cx - hb / 2, cy - hb / 2, hb, hb, alpha, m * 6, clip_top);
}
static double anim_t(void) { return (double)(GetTickCount64() % 3600000ull) / 1000.0; }     /* seconds, wrapping each hour (the turns divide an hour) */

/* ---- text ---- */
static int utf8(const char **s) {
    const unsigned char *p = (const unsigned char *)*s;
    int c = *p++;
    if (c >= 0xF0) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = (const char *)p;
    return c;
}
static float text_w(const stbtt_fontinfo *f, const char *s, float px, float track) {
    float sc = stbtt_ScaleForPixelHeight(f, px), w = 0;
    int prev = 0, adv, lsb;
    while (*s) { int c = utf8(&s); if (prev) w += sc * stbtt_GetCodepointKernAdvance(f, prev, c); stbtt_GetCodepointHMetrics(f, c, &adv, &lsb); w += adv * sc + track; prev = c; }
    return w - (prev ? track : 0);
}
static float text_a(const stbtt_fontinfo *f, float x, int base, const char *s, float px, Rgb col, float track, float alpha) {      /* returns the pen's end */
    static uint8_t buf[256 * 256];
    float sc = stbtt_ScaleForPixelHeight(f, px);
    int prev = 0, adv, lsb;
    while (*s) {
        int c = utf8(&s), x0, y0, x1, y1;
        if (prev) x += sc * stbtt_GetCodepointKernAdvance(f, prev, c);
        float fx = x - floorf(x);
        stbtt_GetCodepointBitmapBoxSubpixel(f, c, sc, sc, fx, 0, &x0, &y0, &x1, &y1);
        int w = x1 - x0, h = y1 - y0;
        if (w > 0 && h > 0 && w <= 256 && h <= 256) {
            stbtt_MakeCodepointBitmapSubpixel(f, buf, w, h, w, sc, sc, fx, 0, c);
            for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) if (buf[j * w + i]) put((int)floorf(x) + x0 + i, base + y0 + j, col, buf[j * w + i] / 255.0f * alpha);
        }
        stbtt_GetCodepointHMetrics(f, c, &adv, &lsb);
        x += adv * sc + track;
        prev = c;
    }
    return x;
}
static float text(const stbtt_fontinfo *f, float x, int base, const char *s, float px, Rgb col, float track) { return text_a(f, x, base, s, px, col, track, 1); }
static int wrap(const stbtt_fontinfo *f, char lines[][160], int maxl, const char *s, float px, float width) {      /* word wrap */
    int n = 0;
    while (*s && n < maxl) {
        char line[160] = "";
        const char *best = s;
        for (const char *p = s; ; p++) {
            if (*p == ' ' || !*p) {
                char t[160]; size_t len = (size_t)(p - s) < sizeof t - 1 ? (size_t)(p - s) : sizeof t - 1;
                memcpy(t, s, len); t[len] = 0;
                if (text_w(f, t, px, 0) > width && best != s) break;
                memcpy(line, t, len + 1); best = p;
                if (!*p) break;
            }
        }
        snprintf(lines[n++], 160, "%s", line);
        s = *best ? best + 1 : best;
    }
    return n;
}
static const char *pad_name(int b) {                       /* the button on a pad of the family in use: by position, its Pro name's place */
    static const char *const sw[16] = {"A", "B", "X", "Y", "Left stick press", "Right stick press", "L", "R", "ZL", "ZR", "Plus", "Minus", "D-pad left", "D-pad up", "D-pad right", "D-pad down"};
    static const char *const face[2][2][4] = {{{"Circle", "Cross", "Triangle", "Square"}, {"Cross", "Circle", "Square", "Triangle"}}, {{"B", "A", "Y", "X"}, {"A", "B", "X", "Y"}}};
    static const char *const ps[6] = {"L1", "R1", "L2", "R2", "Options", "Create"}, *const xb[6] = {"LB", "RB", "LT", "RT", "Menu", "View"};
    if (g_fam != 2 && g_fam != 3) return sw[b];
    if (b < 4) return face[g_fam == 3][g_letter][b];
    if (b >= 6 && b <= 11) return (g_fam == 3 ? xb : ps)[b - 6];
    return sw[b];
}
static const char *row_label(int i) {                      /* the words of the game's own picture of the pad for Viola, who has no weapons or demons */
    int s = k_rows[i].slot;
    if (k_rows[i].apply != A_BIND || !g_ch) return k_rows[i].label;
    return s == RB_R ? "Block" : s == RB_ZL ? "Summon Cheshire" : s == RB_L || (s >= 12 && s <= 14) ? "Not used" : k_rows[i].label;
}
static const char *val_name(int i) {
    if (i == g_cap) return "Press a button\xE2\x80\xA6";
    if (k_rows[i].apply == A_BIND) return pad_name(g_cur[i]);
    return g_cur[i] >= 0 ? k_rows[i].name[g_cur[i]] : g_raw[i];
}
static int row_restart(int i) {                            /* a value that is not the running one yet */
    if (g_cur[i] == g_start[i]) return 0;
    return k_rows[i].apply == A_RESTART || (k_rows[i].apply == A_SCALE && g_open) || ((k_rows[i].apply == A_WINDOW || k_rows[i].apply == A_SIZE) && g_nowin);
}
/* a button hint on the pale band: the button that acts as Pro button b (0 = A, 1 = B) on player 1's pad, drawn as the
 * prompts show it (g_fam: 1 Switch, 2 PlayStation, 3 Xbox, 4 the keyboard; g_letter: PlayStation and Xbox pads by printed
 * letter or by position, the Face buttons row): a letter cut pale in a dark disc, a PlayStation shape, or a key cap;
 * then the word. Returns the width used; draw = 0 measures only. */
static float hint_btn(float x, int base, float px, int b, const char *what, Rgb disc_c, Rgb mark_c, Rgb text_c, int draw) {
    float r = px * 0.6f, cx = x + r, cy = base - px * 0.33f, gw;
    if (g_fam == 4) {                                      /* the keyboard: Enter confirms, Esc leaves (menu_read) */
        const char *key = b ? "ESC" : "ENT";
        float kpx = px * 0.5f, kw = text_w(&g_f_uib, key, kpx, 1 * px / 20) + px * 0.6f, kh = 2 * r * 0.85f, rr = px * 0.15f;
        gw = kw;
        if (draw) {
            float x0 = x, y0 = cy - kh / 2;
            for (int j = (int)y0 - 1; j <= (int)(y0 + kh) + 1; j++) for (int i = (int)x0 - 1; i <= (int)(x0 + kw) + 1; i++) {      /* a rounded box */
                float dx = fabsf(i + 0.5f - (x0 + kw / 2)) - (kw / 2 - rr), dy = fabsf(j + 0.5f - (y0 + kh / 2)) - (kh / 2 - rr);
                float ox = dx > 0 ? dx : 0, oy = dy > 0 ? dy : 0, in = dx > dy ? dx : dy, d = sqrtf(ox * ox + oy * oy) + (in < 0 ? in : 0) - rr;
                float a = 0.5f - d;
                if (a > 0) put(i, j, disc_c, a > 1 ? 1 : a);
            }
            float lw = text_w(&g_f_uib, key, kpx, 1 * px / 20);
            text(&g_f_uib, x0 + (kw - lw) / 2, (int)(cy + kpx * 0.36f), key, kpx, mark_c, 1 * px / 20);
        }
    } else {
        gw = 2 * r;
        if (draw) {
            disc(cx, cy, r, disc_c, 0);
            if (g_fam == 2) {                              /* PlayStation: by position Circle acts as A, by letter Cross does */
                int cross = (b == 0) == (g_letter != 0);
                if (cross) {
                    float L = r * 0.4f, w = px * 0.07f;
                    for (int j = (int)(cy - L - 1); j <= (int)(cy + L + 1); j++) for (int i = (int)(cx - L - 1); i <= (int)(cx + L + 1); i++) {
                        float dx = i + 0.5f - cx, dy = j + 0.5f - cy, u = fabsf(dx + dy) * 0.70710678f, v = fabsf(dx - dy) * 0.70710678f;
                        float d1 = u > w ? u - w : 0, e1 = v > L ? v - L : 0, d2 = v > w ? v - w : 0, e2 = u > L ? u - L : 0;
                        float d = sqrtf(d1 * d1 + e1 * e1); float f = sqrtf(d2 * d2 + e2 * e2); if (f < d) d = f;
                        float a = 1 - d; if (a > 0) put(i, j, mark_c, a > 1 ? 1 : a);
                    }
                } else disc(cx, cy, r * 0.46f, mark_c, px * 0.085f);
            } else {
                const char *letter = pad_name(b);
                float lw = text_w(&g_f_uib, letter, px * 0.72f, 0);
                text(&g_f_uib, cx - lw / 2, (int)(cy + px * 0.26f), letter, px * 0.72f, mark_c, 0);
            }
        }
    }
    if (draw) text(&g_f_ui, x + gw + px * 0.4f, base, what, px, text_c, 0);
    return gw + px * 0.4f + text_w(&g_f_ui, what, px, 0);
}
static void alloc_panel(int pw, int ph) {
    if (pw * ph != g_pw * g_ph) { free(g_px); g_px = malloc((size_t)pw * ph * 4); }
    g_pw = pw; g_ph = ph;
}
/* the stage every full screen shares: the pearl ground, the indigo panel from the left with its stripe and rules; its edge
 * at x_top (1280-scale) at the top of the window */
static void stage_ground(int W, int H) {
    for (int y = 0; y < H; y++) rect(0, y, W, 1, mix(C_PEARL, C_PEARL_LO, (float)y / H), 1);
    soft_disc(W * 0.7f, H * 0.45f, H * 0.9f, C_WHITE, 0.9f);
}
static void stage_panel(int W, int H, float s, float x_top) {
    (void)W;
    wedge(-300 * s, 660 * s, x_top, H, C_INDIGO0, C_INDIGO1, C_INDIGO2, 0.7f);
    for (int j = 0; j < H; j++) { float e = x_top + j * TAN22; hgrad((int)e, j, (int)(40 * s) + 1, 1, C_STRIPE0, C_STRIPE1); }
    stripe_text(&g_stripe, x_top, 40 * s, H, 1);
    slant_line(x_top + 50 * s, H, 1, C_INK, 0.35f); slant_line(x_top + 55 * s, H, 1, C_INK, 0.35f);
}
static void bands(int W, int H, float s, Rgb band_c, float band_a, Rgb foot_c, float foot_a) {
    rect(0, (int)(H - 126 * s), W, (int)(52 * s), band_c, band_a);
    for (int j = 0; j < (int)(74 * s); j++) rect(0, (int)(H - 74 * s) + j, W, 1, mix(foot_c, mix(foot_c, C_INK, 0.06f), j / (74 * s)), foot_a);
}
static void draw_panel_words(int W, int H, float s);
static uint8_t *g_pbg; static int g_pbg_w, g_pbg_h; static unsigned g_pbg_ver = ~0u;     /* the panel's ground, once a size */
static Layer g_hubs, g_words;                                                          /* over the wheels: the hubs, then the words, once a version */
static float scale_of(int W, int H) { float s = (float)W / 1280.0f < (float)H / 720.0f ? (float)W / 1280.0f : (float)H / 720.0f; return s < 0.45f ? 0.45f : s; }
/* the panel's wheels: the light one (centre, radius) on the pearl side, right of the slanted edge; the dark one on the indigo panel, left of it */
static void panel_wheels(int W, float s, float wl[3], float wd[3], float *edge) { wl[0] = W - 300 * s; wl[1] = 360 * s; wl[2] = 360 * s; wd[0] = 170 * s; wd[1] = 430 * s; wd[2] = 240 * s; *edge = 360 * s; }
static void draw_panel(int W, int H) {
    float s = scale_of(W, H), wl[3], wd[3], edge;
    art_init();
    alloc_panel(W, H);
    if (!g_px) return;
    g_rect[0] = 0; g_rect[1] = 0; g_rect[2] = W; g_rect[3] = H;
    panel_wheels(W, s, wl, wd, &edge);
    const float wl_x = wl[0], wl_y = wl[1], wl_r = wl[2], wd_x = wd[0], wd_y = wd[1], wd_r = wd[2];
    if (g_pbg && (g_pbg_w != W || g_pbg_h != H)) { free(g_pbg); g_pbg = NULL; }
    if (g_pbg && g_pbg_ver == g_ver) {
        memcpy(g_px, g_pbg, (size_t)W * H * 4);
    } else {
        if (!g_pbg) {
            /* the stage: the ground, the base of the wheel on the pearl side, the indigo panel, the lilac wheel's base on it */
            stage_ground(W, H);
            g_clip_right = 1; wheel_still(&g_wheel, wl_x, wl_y, wl_r, 0.34f, edge); g_clip_right = 0;
            stage_panel(W, H, s, edge);
            wheel_still(&g_wheel_dark, wd_x, wd_y, wd_r, 0.4f, edge - 1);
            g_pbg = malloc((size_t)W * H * 4); g_pbg_w = W; g_pbg_h = H;
            if (g_pbg) memcpy(g_pbg, g_px, (size_t)W * H * 4);
        } else memcpy(g_px, g_pbg, (size_t)W * H * 4);
        layer_clear(&g_hubs); layer_clear(&g_words);
        g_to = &g_hubs;
        g_clip_right = 1; wheel_hub(&g_wheel, wl_x, wl_y, wl_r, 0.34f, edge); g_clip_right = 0;
        wheel_hub(&g_wheel_dark, wd_x, wd_y, wd_r, 0.4f, edge - 1);
        g_to = &g_words;
        draw_panel_words(W, H, s);
        g_to = NULL;
        g_pbg_ver = g_ver;
    }
    if (g_gpu) return;                             /* the renderer draws the moving parts and the layers over this ground */
    double t = anim_t();
    g_clip_right = 1; wheel_turn(&g_wheel, wl_x, wl_y, wl_r, 0.34f, 0, edge, t); g_clip_right = 0;
    layer_over(&g_hubs);
    wheel_turn(&g_wheel_dark, wd_x, wd_y, wd_r, 0.4f, 1, edge - 1, t);
    layer_over(&g_words);
}
static void draw_panel_words(int W, int H, float s) {
    int nshow = 0;
    for (int i = 0; i < NROW; i++) nshow += row_on(i);
    /* the indigo side: where you are */
    int ty = (int)(170 * s);
    float ttl = 38 * s;
    if (g_page) text(&g_f_ui, 56 * s, ty - (int)(30 * s), "PC Settings  \xE2\x80\xBA  Controls", 15 * s, C_LILAC, 2 * s);
    text(&g_f_head, 56 * s, ty + (int)(ttl * 0.78f), g_page ? "Controls" : "PC Settings", ttl, C_WHITE, 0);
    hgrad_a((int)(56 * s), ty + (int)(ttl * 1.05f), (int)(190 * s), 1, C_LAV, C_LAV, 1, 0.1f);
    text(&g_f_ui, 56 * s, ty + (int)(ttl * 1.05f + 26 * s), g_page ? "Each character keeps a layout." : "The game waits while this is open.", 16 * s, C_LILAC, 1 * s);
    /* the rows: a staircase down the diagonal, both ends stepping as the launcher's rows do; the last ends 50 from the edge */
    float band_y = H - 126 * s, top = nshow > 9 ? 110 * s : 200 * s, rowh = 50 * s;
    if (top + nshow * rowh > band_y - 40 * s) rowh = (band_y - 40 * s - top) / nshow;
    float st = 26 * s, w0 = 460 * s;
    if (nshow > 1 && st * (nshow - 1) > 200 * s) st = 200 * s / (nshow - 1);
    float x0 = W - 50 * s - w0 - st * (nshow > 1 ? nshow - 1 : 0);
    float rpx = rowh * 0.46f, vpx = rowh * 0.44f;
    g_row_h = (int)rowh;
    int y = (int)top, k = 0;
    for (int i = 0; i < NROW; i++) {
        g_row_y[i] = -1;
        if (!row_on(i)) continue;
        g_row_y[i] = y;
        float left = x0 + k * st, width = w0;
        g_val_x0[i] = (int)(left + width - 232 * s); g_val_x1[i] = (int)(left + width - 22 * s);
        int sel = i == g_sel, base = y + (int)(rowh * 0.66f);
        if (sel) {
            para(left, (float)y, width, rowh, C_VIOLET0, C_LAV2, C_LAV, 0.55f, 1, 0.55f);
            text(&g_f_ui, left + 6 * s, base + (int)(rowh * 0.02f), "\xC2\xBB", rowh * 0.6f, C_WHITE, 0);      /* inside the glow, where white shows */
        } else hgrad_a((int)left, y + (int)rowh - 1, (int)width, 1, C_INK, C_INK, 0.28f, 0);
        Rgb tc = g_lock_why[i] ? C_MUTE : sel ? C_WHITE : C_INK;
        text(&g_f_ui, left + 30 * s, base, row_label(i), rpx, tc, 0.5f * s);
        const char *v = val_name(i);
        float vx1 = left + width - 22 * s, vx0 = vx1 - 210 * s, vw = text_w(&g_f_uib, v, vpx, 0);
        Rgb vc = g_lock_why[i] ? C_MUTE : sel ? C_WHITE : C_VIOLET;
        text(&g_f_uib, (vx0 + vx1) / 2 - vw / 2, base, v, vpx, vc, 0);
        if (!g_lock_why[i]) {
            text_a(&g_f_ui, vx0, base, "\xE2\x80\xB9", vpx * 1.25f, sel ? C_WHITE : C_VIOLET, 0, sel ? 1 : 0.55f);
            text_a(&g_f_ui, vx1 - text_w(&g_f_ui, "\xE2\x80\xBA", vpx * 1.25f, 0), base, "\xE2\x80\xBA", vpx * 1.25f, sel ? C_WHITE : C_VIOLET, 0, sel ? 1 : 0.55f);
        }
        if (row_restart(i)) disc(vx0 - 14 * s, base - rpx * 0.3f, 5 * s, C_RED, 0);
        y += (int)rowh; k++;
    }
    /* the state of the chosen row, under the rows at the right */
    char st_txt[160] = "";
    Rgb sc = C_INK; int dot = 0;
    if (g_lock_why[g_sel] == 1) { snprintf(st_txt, sizeof st_txt, "Set by the environment variable %s", k_rows[g_sel].env); sc = C_MUTE; }
    else if (g_lock_why[g_sel] == 2) { snprintf(st_txt, sizeof st_txt, "Set by profile = low in graphics.ini"); sc = C_MUTE; }
    else if (row_restart(g_sel)) { snprintf(st_txt, sizeof st_txt, "Saved; applies %s", k_rows[g_sel].apply == A_SCALE ? "when this panel closes" : "at the next start"); dot = 1; }
    else if (g_cur[g_sel] != g_start[g_sel]) snprintf(st_txt, sizeof st_txt, "Applied and saved");
    if (st_txt[0]) {
        float px = 16 * s, w = text_w(&g_f_ui, st_txt, px, 0.5f * s), xr = W - 50 * s - 22 * s;
        int sb = y + (int)(28 * s);
        if (dot) disc(xr - w - 14 * s, sb - px * 0.3f, 5 * s, C_RED, 0);
        text(&g_f_ui, xr - w, sb, st_txt, px, sc, 0.5f * s);
    }
    /* the message band: the chosen row's help, one line; the hint band: the buttons */
    bands(W, H, s, C_BAND, 0.78f, C_FOOT, 0.94f);
    const char *help = g_cap >= 0 ? "Press the button for this, on the pad. Esc leaves it as it is." : k_rows[g_sel].help;
    float hpx = 20 * s, hw = text_w(&g_f_ui, help, hpx, 1.5f * s);
    if (hw > W - 60 * s) { hpx *= (W - 60 * s) / hw; hw = text_w(&g_f_ui, help, hpx, 1.5f * s); }
    text(&g_f_ui, (W - hw) / 2, (int)(band_y + 26 * s + hpx * 0.35f), help, hpx, C_WHITE, 1.5f * s);
    int fb = (int)(H - 37 * s + 7 * s);
    float fpx = 20 * s;
    text(&g_f_ui, 60 * s, fb, "F1  \xC2\xB7  Start + Select", 15 * s, C_MUTE, 1 * s);
    const char *close = g_page && !g_direct ? "Back" : "Close";
    float wc = hint_btn(0, fb, fpx, g_swap ? 1 : 0, "Change", C_INK, C_PEARL, C_INK, 0), wb = hint_btn(0, fb, fpx, g_swap ? 0 : 1, close, C_INK, C_PEARL, C_INK, 0), x = W - 60 * s - wc - 30 * s - wb;
    hint_btn(x, fb, fpx, g_swap ? 1 : 0, "Change", C_INK, C_PEARL, C_INK, 1);
    x += wc + 30 * s;
    g_close_r[0] = (int)(x - 10 * s); g_close_r[1] = fb - (int)(30 * s); g_close_r[2] = (int)(wb + 20 * s); g_close_r[3] = (int)(44 * s);
    hint_btn(x, fb, fpx, g_swap ? 0 : 1, close, C_INK, C_PEARL, C_INK, 1);
}

/* The hint: a small slanted bar at the bottom right for a few seconds when the game's pause menu opens (nnfs.c sees it
 * load), the way into the game's own Options. The Options screen itself is not recognised (its UI is loaded once at the start). */
static volatile ULONGLONG g_hint_until;
static void draw_hint(int W, int H) {
    float s = (float)W / 1920.0f < (float)H / 1080.0f ? (float)W / 1920.0f : (float)H / 1080.0f;
    if (s < 0.45f) s = 0.45f;
    const char *t1 = "PC Settings", *t2 = "F1  \xC2\xB7  Start + Select";
    float p1 = 24 * s, p2 = 17 * s, w1 = text_w(&g_f_head, t1, p1, 0), w2 = text_w(&g_f_ui, t2, p2, 1 * s);
    int ph = (int)(78 * s), lean = (int)(ph * TAN22), pw = (int)((w1 > w2 ? w1 : w2) + 64 * s) + lean;
    alloc_panel(pw, ph);
    if (!g_px) return;
    g_rect[0] = W - pw - (int)(40 * s); g_rect[1] = H - ph - (int)(96 * s); g_rect[2] = pw; g_rect[3] = ph;
    for (int y = 0; y < ph; y++) rect(0, y, pw, 1, C_PEARL, 1);      /* copied over the game opaque: a pale ground, the slanted bar on it */
    para(0, 0, (float)(pw - lean - 1), (float)ph, C_INDIGO0, C_INDIGO1, C_INDIGO2, 0.7f, 1, 1);
    text(&g_f_head, lean + 28 * s, (int)(34 * s), t1, p1, C_WHITE, 0);
    text(&g_f_ui, lean + 28 * s, (int)(60 * s), t2, p2, C_LILAC, 1 * s);
}

/* The first-start screen (nvn_vk_draw.c prep_first_hold): the game is held at its first frame while every shader is
 * prepared; the render thread sets the numbers here about ten times a second and copies the panel over the frame. The
 * ground, the wheel and the words are drawn once a size (g_bg) and the numbers over a copy of it. */
static struct { int on, pct, eta, reading; long long done, total; char info[96]; } g_prep;
static uint8_t *g_bg; static int g_bg_w, g_bg_h;
static void draw_prep(int W, int H) {
    float s = scale_of(W, H);
    art_init();
    alloc_panel(W, H);
    if (!g_px) return;
    g_rect[0] = 0; g_rect[1] = 0; g_rect[2] = W; g_rect[3] = H;
    if (g_bg && (g_bg_w != W || g_bg_h != H)) { free(g_bg); g_bg = NULL; }
    if (!g_bg) {
        const Rgb c_in = {66, 33, 42}, c_mid = {31, 16, 21}, c_out = {20, 9, 12};
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {      /* the dark ground, lit a little behind the wheel */
            float dx = (x + 0.5f - 0.62f * W) / (0.55f * W), dy = (y + 0.5f - 0.48f * H) / (0.7f * H), d = sqrtf(dx * dx + dy * dy);
            put(x, y, d < 0.55f ? mix(c_in, c_mid, d / 0.55f) : mix(c_mid, c_out, d > 1 ? 1 : (d - 0.55f) / 0.45f), 1);
        }
        wheel_still(&g_wheel_dark, 760 * s, 330 * s, 300 * s, 0.75f, -1);
        float x_top = W - 200 * s;                             /* the pearl panel at the right, its stripe and rules */
        for (int j = 0; j < H; j++) { float e = x_top + j * TAN22; int i0 = (int)e; for (int i = i0; i < W; i++) put(i, j, mix(C_PEARL, C_PEARL_LO, (float)(i - i0) / (W - x_top)), i == i0 ? 1 - (e - i0) : 1); }
        for (int j = 0; j < H; j++) { float e = x_top - 40 * s + j * TAN22; hgrad((int)e, j, (int)(40 * s) + 1, 1, C_STRIPE0, C_STRIPE1); }
        stripe_text(&g_stripe, x_top - 40 * s, 40 * s, H, 1);
        slant_line(x_top + 18 * s, H, 1, C_LILAC, 0.35f); slant_line(x_top + 23 * s, H, 1, C_LILAC, 0.35f);
        /* the plate and the title */
        hgrad_a(0, (int)(30 * s), (int)(420 * s), (int)(44 * s), C_INDIGO0, C_INDIGO, 1, 0);
        float px = 14 * s, x = 44 * s;
        int pb = (int)(30 * s + 44 * s * 0.66f);
        x = text(&g_f_ui, x, pb, "WITCH TIME", px, C_LILAC, 3 * s) + 12 * s;
        { float r = 4 * s, cx = x + r, cy = pb - px * 0.33f; for (int j = (int)(cy - r - 1); j <= (int)(cy + r + 1); j++) for (int i = (int)(cx - r - 1); i <= (int)(cx + r + 1); i++) { float d = fabsf(i + 0.5f - cx) + fabsf(j + 0.5f - cy), a = r - d + 0.5f, b = d - (r - 1.2f * s) + 0.5f; a = a < b ? a : b; put(i, j, C_LILAC, a > 1 ? 1 : a); } x += 2 * r + 12 * s; }
        text(&g_f_uib, x, pb, "FIRST START", 22 * s, C_WHITE, 3 * s);
        float ttl = 54 * s;
        text(&g_f_head, 72 * s, (int)(150 * s + ttl * 0.78f), "Preparing shaders", ttl, C_WHITE, 0);
        hgrad_a((int)(72 * s), (int)(150 * s + ttl * 1.05f), (int)(260 * s), 1, C_LAV, C_LAV, 1, 0.1f);
        text(&g_f_ui, 72 * s, (int)(572 * s + 14 * s), "FIRST START ONLY  \xC2\xB7  CLOSING THE WINDOW KEEPS WHAT IS DONE", 14 * s, C_LILAC, 2 * s);
        for (int j = 0; j < (int)(74 * s); j++) rect(0, (int)(H - 74 * s) + j, W, 1, mix(C_FOOT_DK, C_INDIGO0, j / (74 * s)), 0.94f);
        text(&g_f_ui, 60 * s, (int)(H - 30 * s), g_prep.info, 15 * s, C_LILAC, 1 * s);
        g_bg = malloc((size_t)W * H * 4); g_bg_w = W; g_bg_h = H;
        if (g_bg) memcpy(g_bg, g_px, (size_t)W * H * 4);
        layer_clear(&g_hubs); g_to = &g_hubs; wheel_hub(&g_wheel_dark, 760 * s, 330 * s, 300 * s, 0.75f, -1); g_to = NULL;
    } else memcpy(g_px, g_bg, (size_t)W * H * 4);
    if (!g_gpu) { wheel_turn(&g_wheel_dark, 760 * s, 330 * s, 300 * s, 0.75f, 1, -1, anim_t()); layer_over(&g_hubs); }      /* else the renderer draws them over the numbers */
    /* the numbers */
    char t[64];
    float big = 84 * s, x = 72 * s;
    int pb = (int)(430 * s + big * 0.78f);
    snprintf(t, sizeof t, "%d", g_prep.pct);
    x = text(&g_f_head, x, pb, t, big, C_WHITE, 0);
    x = text(&g_f_head, x + 2 * s, pb, "%", 36 * s, C_LILAC, 0) + 18 * s;
    if (g_prep.reading) snprintf(t, sizeof t, "reading the game data");
    else snprintf(t, sizeof t, "%lld of about %lld stages", g_prep.done, g_prep.total);
    text(&g_f_ui, x, pb - (int)(24 * s), t, 18 * s, C_LILAC, 1 * s);
    if (g_prep.eta >= 0) snprintf(t, sizeof t, "about %d:%02d left", g_prep.eta / 60, g_prep.eta % 60); else snprintf(t, sizeof t, "working out the time left");
    text(&g_f_ui, x, pb, t, 18 * s, C_LILAC, 1 * s);
    para(72 * s, 548 * s, 560 * s, 10 * s, C_LILAC, C_LILAC, C_LILAC, 0.5f, 0.18f, 0.18f);
    float f = g_prep.pct / 100.0f;
    if (f > 0.005f) para(72 * s, 548 * s, 560 * s * f, 10 * s, C_VIOLET0, C_LAV2, C_LAV, 0.6f, 1, 1);
}
/* the render thread (nvn_vk_draw.c): the first-start screen's numbers; reading: the game data is still being read */
void pcmenu_prep_show(int pct, long long done, long long total, int eta, int reading, const char *info) {
    AcquireSRWLockExclusive(&g_lock);
    if (!g_inited && fonts_init()) menu_init();
    if (!g_prep.on || g_prep.pct != pct || g_prep.done != done || g_prep.total != total || g_prep.eta != eta || g_prep.reading != reading) g_ver++;
    g_prep.on = 1; g_prep.pct = pct; g_prep.done = done; g_prep.total = total; g_prep.eta = eta; g_prep.reading = reading;
    snprintf(g_prep.info, sizeof g_prep.info, "%s", info ? info : "");
    ReleaseSRWLockExclusive(&g_lock);
}
void pcmenu_prep_hide(void) {
    AcquireSRWLockExclusive(&g_lock);
    g_prep.on = 0; g_ver++;
    free(g_bg); g_bg = NULL; layer_free(&g_hubs);
    ReleaseSRWLockExclusive(&g_lock);
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
    if (!g_inited) { AcquireSRWLockExclusive(&g_lock); if (!g_inited && fonts_init()) menu_init(); ReleaseSRWLockExclusive(&g_lock); }
    if (!g_inited) return;
    g_hint_until = GetTickCount64() + (ULONGLONG)ms;
    printf("pcmenu: the game's pause menu opened: the hint is shown for %d s\n", ms / 1000);
    nvn_front_post_pcmenu();                               /* its pixels into the staging buffer; the game's presents copy them */
    if (getenv("SSBU_PCMENU_SHOT")) { HANDLE th = CreateThread(NULL, 0, hint_late_job, NULL, 0, NULL); if (th) CloseHandle(th); }      /* (writes the picture) */
}

/* render thread (nvn_vk_pcmenu.c): the panel (or the hint) for this window size, drawn again when something changed */
int pcmenu_is_open(void) { return g_open != 0; }
int pcmenu_kind(void) { return g_open ? 1 : g_prep.on ? 3 : hint_on() ? 2 : 0; }      /* what is shown: the menu, the first-start screen, the hint */
int pcmenu_visible(void) { return pcmenu_kind() != 0; }
int pcmenu_panel_size(int W, int H, int r[4], unsigned *gen) {
    static int drawn_kind = -1;
    static ULONGLONG drawn_at;
    int kind = pcmenu_kind();
    if (!kind) return 0;
    AcquireSRWLockExclusive(&g_lock);
    static ULONGLONG took = 33;
    ULONGLONG now = GetTickCount64();
    int turn = !g_gpu && (kind == 1 || kind == 3) && now - drawn_at >= (took > 33 ? took : 33);     /* the wheel turns: up to 30 frames a second, never more than half a core (the GPU path turns it every frame) */
    if (g_drawn_ver != g_ver || g_for_w != W || g_for_h != H || drawn_kind != kind || turn) {
        if (kind == 1) draw_panel(W, H); else if (kind == 3) draw_prep(W, H); else draw_hint(W, H);
        g_drawn_ver = g_ver; g_for_w = W; g_for_h = H; drawn_kind = kind; g_gen++; drawn_at = now;
        took = GetTickCount64() - now;
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
/* The GPU path (nvn_vk_pcmenu.c): on, the panel and the first-start screen stop composing their moving parts here; the
 * renderer draws them every frame from pcmenu_gpu_scene over the ground it copies, with the layers (pcmenu_layer_read:
 * 1 the hubs, 2 the words; premultiplied BGRA of the panel's size, drawn once a version) between. Changing the mode draws
 * the panel again. */
void pcmenu_gpu(int on) {
    AcquireSRWLockExclusive(&g_lock);
    if (g_gpu != (on != 0)) { g_gpu = on != 0; g_drawn_ver = ~0u; }
    ReleaseSRWLockExclusive(&g_lock);
}
int pcmenu_gpu_scene(int W, int H, PcmSprite *out, int max) {        /* the sprites to draw over the ground, in order, for the window (W, H) now */
    int kind = pcmenu_kind();
    if (kind != 1 && kind != 3) return 0;
    AcquireSRWLockExclusive(&g_lock);
    float s = scale_of(W, H);
    double t = anim_t();
    g_emit = out; g_emit_n = 0; g_emit_max = max;
    PcmSprite hubs = {-1, 0, 0, (float)W, (float)H, 0, 1, 0, 0, -1, 0}, words = hubs;
    words.art = -2;
    if (kind == 1) {
        float wl[3], wd[3], edge;
        panel_wheels(W, s, wl, wd, &edge);
        g_clip_right = 1; wheel_turn(&g_wheel, wl[0], wl[1], wl[2], 0.34f, 0, edge, t); g_clip_right = 0;
        if (g_emit_n < max) out[g_emit_n++] = hubs;
        wheel_turn(&g_wheel_dark, wd[0], wd[1], wd[2], 0.4f, 1, edge - 1, t);
        if (g_emit_n < max) out[g_emit_n++] = words;
    } else {
        wheel_turn(&g_wheel_dark, 760 * s, 330 * s, 300 * s, 0.75f, 1, -1, t);
        if (g_emit_n < max) out[g_emit_n++] = hubs;
    }
    int n = g_emit_n;
    g_emit = NULL; g_emit_n = g_emit_max = 0;
    ReleaseSRWLockExclusive(&g_lock);
    return n;
}
int pcmenu_layer_read(int which, void *dst, int w, int h) {          /* the layer's pixels (w x h, the panel's size): 0 when there are none */
    const Layer *l = which == 1 ? &g_hubs : which == 2 ? &g_words : NULL;
    int ok = 0;
    AcquireSRWLockExclusive(&g_lock);
    if (l && l->px && w == g_pw && h == g_ph && l->n == w * h && l->x1 > l->x0) { memcpy(dst, l->px, (size_t)w * h * 4); ok = 1; }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

/* ---------------------------------------------------------------- changes */
static void add_work(LONG bit) { LONG o; do o = g_work; while (InterlockedCompareExchange(&g_work, o | bit, o) != o); }
static void set_value(int i, int k) {                    /* value k of row i: written, and applied where it can be */
    const Row *r = &k_rows[i];
    if (g_lock_why[i] || k < 0 || k >= row_count(i) || k == g_cur[i]) return;
    int ok;
    if (r->apply == A_SIZE) { char w[16], h[16]; sscanf(r->val[k], "%15[0-9]x%15[0-9]", w, h); ok = gfx_option_set("width", w) && gfx_option_set("height", h); }
    else if (r->apply == A_PROMPTS) { ok = nnpad_prompts_set(r->val[k]); g_fam = nnpad_button_prompts(); g_letter = nnpad_prompts_by_letter(); }      /* the hints and the bind names follow */
    else if (r->apply == A_MENU_SWAP) ok = nnpad_menu_swap_set(r->val[k]);
    else if (r->apply == A_LAYOUT) { ok = nnpad_layout_set(r->val[k]); g_letter = nnpad_prompts_by_letter(); }
    else if (r->apply == A_CHAR) { g_ch = k; binds_show(); return; }
    else if (r->apply == A_BIND) { if (bind_assign(r->slot, k)) printf("pcmenu: %s = %s (%s)\n", r->key, pad_name(k), g_ch ? "Viola" : "Bayonetta"); binds_show(); return; }
    else if (r->apply == A_PRESET) {
        g_b_on[g_ch] = k != 0;
        rb_preset(g_ch, k ? k - 1 : options_control_type(g_ch), g_b[g_ch]);
        nnpad_bind_set(g_ch, k ? g_b[g_ch] : NULL);
        printf("pcmenu: %s = %s (%s)\n", r->key, r->val[k], g_ch ? "Viola" : "Bayonetta");
        binds_show();
        return;
    }
    else if (r->apply == A_PAGE) return;
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
    if (k_rows[i].apply == A_BIND) wrap = 1;
    set_value(i, g_cur[i] < 0 ? (dir > 0 ? 0 : n - 1) : wrap ? (g_cur[i] + dir + n) % n : g_cur[i] + dir);
}
static ULONGLONG g_cap_t;
static int g_cap_armed;
static int activate(int i, int dir) {                      /* A or a click on a row; 1 = it took the press and the row's value is not what changes */
    if (k_rows[i].apply == A_PAGE) { page_go(1); return 1; }
    if (k_rows[i].apply == A_BIND) { g_cap = i; g_cap_t = GetTickCount64(); g_cap_armed = 0; return 1; }
    change(i, dir, 1);
    return 0;
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
        if (g_cap >= 0) {                                  /* a row waits: only the pad counts, and what is down now has to go up first */
            uint32_t p = nnhid_menu_pad();
            int esc = nnhid_menu_key(VK_ESCAPE);
            g_cap_armed |= !p;
            if (esc || now - g_cap_t > 8000 || (g_cap_armed && p)) {
                AcquireSRWLockExclusive(&g_lock);
                if (!esc && p) { int k = 0; while (!(p >> k & 1)) k++; set_value(g_cap, k); }
                g_cap = -1; g_ver++;
                ReleaseSRWLockExclusive(&g_lock);
            }
            dir = 0; ok = back = prev_ok = prev_back = 1;  /* (the press that ends it does nothing else) */
        }
        int step = dir && (dir != prev_dir || (rep_at && now >= t_dir + (ULONGLONG)rep_at));
        if (dir != prev_dir) { t_dir = now; rep_at = 380; } else if (step) { t_dir = now; rep_at = 90; }
        prev_dir = dir;
        mouse_at(&mx, &my, &md);
        AcquireSRWLockExclusive(&g_lock);
        int was_sel = g_sel, was_cur[NROW];
        memcpy(was_cur, g_cur, sizeof g_cur);
        if (step && (dir == 1 || dir == 2)) { do g_sel = (g_sel + (dir == 1 ? NROW - 1 : 1)) % NROW; while (!row_on(g_sel)); }
        if (step && (dir == 3 || dir == 4)) change(g_sel, dir == 3 ? -1 : 1, 0);
        if (ok && !prev_ok) { activate(g_sel, 1); g_ver++; }
        int px = mx - g_rect[0], py = my - g_rect[1], click = md && !prev_click;
        if (mx >= 0 && (mx != mx0 || my != my0 || click) && px >= 0 && px < g_pw && py >= 0 && py < g_ph) {      /* the mouse: hover chooses, a click changes */
            for (int i = 0; i < NROW; i++) if (g_row_y[i] >= 0 && py >= g_row_y[i] && py < g_row_y[i] + g_row_h && g_cap < 0) { g_sel = i; if (click && px >= g_val_x0[i] - g_row_h) { activate(i, px < g_val_x0[i] + g_row_h ? -1 : 1); g_ver++; break; } }
            if (click && px >= g_close_r[0] && px < g_close_r[0] + g_close_r[2] && py >= g_close_r[1] && py < g_close_r[1] + g_close_r[3]) back = 1, prev_back = 0;
        }
        mx0 = mx; my0 = my;
        if (g_sel != was_sel || memcmp(was_cur, g_cur, sizeof g_cur)) g_ver++;
        ReleaseSRWLockExclusive(&g_lock);
        if (g_log && (step || (ok && !prev_ok) || (back && !prev_back) || click)) printf("pcmenu: input: %s%s%s%s -> row %d (%s)\n", step ? (dir == 1 ? "up " : dir == 2 ? "down " : dir == 3 ? "left " : "right ") : "", ok && !prev_ok ? "A " : "", back && !prev_back ? "B " : "", click ? "click " : "", g_sel, k_rows[g_sel].key);
        prev_ok = ok; prev_click = md;
        if (back && !prev_back) {
            if (!g_page || g_direct) break;
            AcquireSRWLockExclusive(&g_lock);
            page_go(0); g_ver++;
            ReleaseSRWLockExclusive(&g_lock);
        }
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
static void menu_open(const char *why, int controls, int ch) {
    if (InterlockedExchange(&g_open, 1)) return;
    g_hint_until = 0;
    if (g_log < 0) g_log = getenv("SSBU_PCMENU_LOG") && atoi(getenv("SSBU_PCMENU_LOG"));
    AcquireSRWLockExclusive(&g_lock);
    if (!g_f_ui.data && !fonts_init()) { ReleaseSRWLockExclusive(&g_lock); InterlockedExchange(&g_open, 0); printf("pcmenu: the font cannot be read\n"); return; }
    if (!g_inited) menu_init();
    g_swap = nnpad_menu_swapped();
    g_fam = nnpad_button_prompts();
    g_letter = nnpad_prompts_by_letter();
    g_ch = ch; g_direct = controls;
    binds_load();
    page_go(controls);
    g_ver++;
    ReleaseSRWLockExclusive(&g_lock);
    InterlockedExchange(&g_guest_frozen, 1);               /* the game stops at its next SDK call (hostrt.c) */
    printf("pcmenu: opened (%s); the game is held\n", why);
    fflush(stdout);
    HANDLE th = CreateThread(NULL, 0, menu_thread, NULL, 0, NULL);
    if (th) CloseHandle(th); else { InterlockedExchange(&g_guest_frozen, 0); InterlockedExchange(&g_open, 0); }
}
void pcmenu_open_game(void) { menu_open("the game's Options: PC Settings", 0, 0); }     /* options_row.c, the game's main thread */
void pcmenu_open_controls(int ch) { menu_open("the game's Options: Controls", 1, ch); }
/* nnhid.c, at every pad state the game reads: opens the menu, and says when the game gets an idle pad */
int pcmenu_gate(uint32_t id, uint64_t buttons) {
    static int prev = 1, test = -1;
    static ULONGLONG t0;
    LONG bit = id < 8 ? 1L << id : 0, o;
    if (g_open) return 1;
    if (id == 0) {
        int f1 = nnhid_menu_key(VK_F1), combo = (buttons & 0xc00) == 0xc00;      /* Start + Select (the game uses L3 and R3) */
        if (test < 0) { const char *e = getenv("SSBU_PCMENU_OPEN"); test = e ? atoi(e) : 0; t0 = GetTickCount64(); }
        if (test > 0 && GetTickCount64() - t0 >= (ULONGLONG)test * 1000) { test = 0; menu_open("SSBU_PCMENU_OPEN", 0, 0); return 1; }
        if ((f1 || combo) && !prev && !(g_release & 1)) { prev = 1; menu_open(f1 ? "F1" : "Start + Select", 0, 0); return 1; }
        prev = f1 || combo;
        if (prev && (g_release & 1)) return 1;             /* the F1 that closed it is still down */
    }
    if (!(g_release & bit)) return 0;
    if (buttons) return 1;
    do o = g_release; while (InterlockedCompareExchange(&g_release, o & ~bit, o) != o);
    if (g_log > 0) printf("pcmenu: player %u's input reaches the game again\n", id + 1);
    return 0;
}
