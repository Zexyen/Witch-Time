/* [witchtime] PlayStation / Xbox button prompts, no mod needed. Every prompt of the game comes from one atlas, core_sys_btn
 * (ui/ui_core*.dtt, texture 0 of core.wtp), 40 x 40 cells on a 4 x 4 block grid, named btn_NNN in core.uvd. A cell holds two
 * pictures: alpha is the button, RGB its backing (white on black): the same outline with its holes filled, 2 texels wider.
 * uibaseshader takes alpha from RGB when a vertex asks for it (in_attr14.y), so the action guides ("Climax") can draw a cell
 * in black behind the button. Most are neutral (d-pad, sticks, the coloured action icons); the 10 below are the Switch's. When the
 * game makes that texture (nvn_gl.c tex_initialize) they are painted over in its memory, in the texture's own format, and
 * again whenever the choice changes: the renderers see an ordinary texture the game has written to. Only the two atlases known by hash are touched, so a mod's atlas stays as it is.
 * Which family: controllers.ini button_prompts (nnpad_button_prompts). The pictures are drawn from plain shapes (prompt_art.h);
 * there is no artwork in the repository.
 *
 * Keyboard: the same cells, and the sticks' too, show the keys bound in [keyboard] (the first of a binding), the mouse as a
 * mouse.
 *
 * Face buttons: by position (default layout) Switch A, the right button, is Circle / Xbox B, and so on. By printed letter
 * (nnpad_prompts_by_letter) A is the bottom button: the letters are right, but the pictures that show WHERE a button sits
 * (four dots, btn_06x) swap the same way. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nnpad.h"
#include "prompt_art.h"
#include "tex_decode.h"

int nnpad_mouse_cfg(float *sens, int *invert);           /* nnpad_sdl.c */
void prompts_tick(void);

#define FMT_ASTC4 0x79
#define FMT_BC3   0x44
#define ATLAS_W 256
#define ATLAS_H 512
#define ATLAS_SIZE 0x20000

enum { WMAX = 52 };                                         /* the widest cell */
typedef struct { int x, y, w; } Cell;                       /* w: 0 = CELL */
static const Cell MINUS = {160, 0}, PLUS = {0, 132}, A = {204, 0}, B = {160, 44}, Y = {204, 44}, X = {0, 88};
static const Cell L1 = {56, 88}, R1 = {112, 88}, L2 = {160, 88}, R2 = {204, 88};
static const Cell DOTS[][2] = {{{160, 308}, {56, 352}},     /* right <-> bottom */
                               {{0, 352}, {112, 352}},      /* top <-> left */
                               {{112, 132}, {204, 352}}};   /* left + right <-> top + bottom */
typedef struct { Cell four, across, around, left, right, updown, up, down, plain, click; } Stick;   /* plain: "the stick", to move it; click: pressed in */
static const Stick STICK_L = {{0, 0, 52}, {56, 44, 52}, {112, 44, 46}, {204, 132}, {0, 176}, {56, 176}, {0, 396, 36}, {56, 396, 36}, {56, 132}, {112, 396, 36}};
static const Stick STICK_R = {{56, 0, 52}, {0, 44, 52}, {112, 0, 46}, {0, 220}, {56, 220}, {56, 440, 36}, {204, 396, 36}, {160, 396, 36}, {160, 176}, {0, 440, 36}};

/* the name of a virtual key on a keycap, as in [keyboard]; 0 (unbound) = "" */
void prompts_key_label(int vk, char *out) {
    static const struct { int vk; const char *s; } name[] = {{0x05, "M4"}, {0x06, "M5"}, {0x08, "BKS"}, {0x09, "TAB"}, {0x0d, "ENT"}, {0x10, "SFT"},
        {0x11, "CTL"}, {0x12, "ALT"}, {0x1b, "ESC"}, {0x20, "SPC"}, {0x25, "LFT"}, {0x26, "UP"}, {0x27, "RGT"}, {0x28, "DWN"}, {0xa0, "SFT"},
        {0xa1, "SFT"}, {0xa2, "CTL"}, {0xa3, "CTL"}, {0xa4, "ALT"}, {0xa5, "ALT"}};
    out[0] = 0;
    if (!vk) return;
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) { out[0] = (char)vk; out[1] = 0; return; }
    if (vk >= 0x60 && vk <= 0x69) { snprintf(out, 4, "N%d", vk - 0x60); return; }
    if (vk >= 0x70 && vk <= 0x7b) { snprintf(out, 4, "F%d", vk - 0x6f); return; }
    for (size_t i = 0; i < sizeof name / sizeof name[0]; i++) if (name[i].vk == vk) { strcpy(out, name[i].s); return; }
    snprintf(out, 4, "%02X", vk & 0xff);
}
static int bound(int idx) { int v = nnpad_kb_vk(idx); return v & 0xff ? v & 0xff : v >> 8 & 0xff; }
static void key(uint8_t *s, int idx) {
    int vk = bound(idx);
    char t[4];
    if (vk == 1 || vk == 2) { mouse(s, vk); return; }
    if (vk == 4) { mouse(s, 3); return; }
    prompts_key_label(vk, t);
    keycap(s, t);
}
/* the first letters of some keys: "WASD" */
static void keys(uint8_t *s, const int *idx, int n) {
    char t[5] = "", l[4];
    for (int i = 0; i < n; i++) { prompts_key_label(bound(idx[i]), l); t[i] = l[0] ? l[0] : ' '; }
    keycap(s, t);
}

/* ---- one block */
static void put(uint8_t *b, int pos, int n, unsigned v) {
    for (int i = 0; i < n; i++) if (v >> i & 1) b[(pos + i) / 8] |= (uint8_t)(1 << ((pos + i) & 7));
}
/* ASTC 4 x 4, LDR: one partition, luminance + alpha endpoints (CEM 4), fixed at transparent black and opaque white, two planes
 * of 16 weights of 3 bits: luminance and alpha (ccs 3) are separate pictures, one plane cannot hold both. Block mode 0x453:
 * weight grid 4 x 4, dual plane, range 0..7. */
void prompts_astc_block(const uint8_t *rgba, int stride, uint8_t *out) {
    memset(out, 0, 16);
    put(out, 0, 11, 0x453); put(out, 13, 4, 4);             /* bits 11-12 stay 0: one partition */
    put(out, 20, 3, 7); put(out, 26, 3, 7); put(out, 30, 2, 3);   /* endpoints of 3 bits at 17: 0, 7, 0, 7; ccs */
    for (int i = 0; i < 32; i++) {
        int w = (rgba[((i >> 3) * stride + (i >> 1 & 3)) * 4 + (i & 1) * 3] * 7 + 127) / 255;
        for (int j = 0; j < 3; j++) if (w >> j & 1) { int p = 127 - (i * 3 + j); out[p / 8] |= (uint8_t)(1 << (p & 7)); }   /* weights run from the top, bit-reversed */
    }
}

static uint8_t *block_at(uint8_t *mem, int x, int y, int bh_log2) {
    return mem + tex_bl_offset(x / 4 * 16, y / 4, 0, ATLAS_W / 4 * 16, ATLAS_H / 4, bh_log2, 0);
}
static void cell_swap(uint8_t *mem, Cell a, Cell b, int bh_log2) {
    for (int y = 0; y < CELL; y += 4) for (int x = 0; x < CELL; x += 4) {
        uint8_t *p = block_at(mem, a.x + x, a.y + y, bh_log2), *q = block_at(mem, b.x + x, b.y + y, bh_log2), t[16];
        memcpy(t, p, 16); memcpy(p, q, 16); memcpy(q, t, 16);
    }
}
/* rgba holds the picture in alpha: RGB becomes its backing (top of this file). Anything but this shape shows as a black shape behind the button. */
static void cell_backing(uint8_t *rgba, int w) {
    uint8_t v[WMAX * CELL];
    int x0[CELL], x1[CELL], y0[WMAX], y1[WMAX];             /* where the outline starts and ends on a row, in a column */
    for (int i = 0; i < CELL; i++) { x0[i] = w; x1[i] = -1; }
    for (int i = 0; i < w; i++) { y0[i] = CELL; y1[i] = -1; }
    for (int y = 0; y < CELL; y++) for (int x = 0; x < w; x++) {
        int m = 0;
        for (int j = -2; j <= 2; j++) for (int i = -2; i <= 2; i++) if (i * i + j * j <= 5 && y + j >= 0 && y + j < CELL && x + i >= 0 && x + i < w) {
            int a = rgba[((y + j) * w + x + i) * 4 + 3];
            m = a > m ? a : m;
        }
        v[y * w + x] = (uint8_t)m;
        if (m > 127) { x0[y] = x < x0[y] ? x : x0[y]; x1[y] = x; y0[x] = y < y0[x] ? y : y0[x]; y1[x] = y; }
    }
    for (int y = 0; y < CELL; y++) for (int x = 0; x < w; x++) memset(rgba + (y * w + x) * 4, x > x0[y] && x < x1[y] && y > y0[x] && y < y1[x] ? 255 : v[y * w + x], 3);
}
static void cell_draw(uint8_t *mem, int format, int bh_log2, Cell c, const uint8_t *s) {
    int w = c.w ? c.w : CELL, bw = (w + 3) / 4, ox = (CELL - w) / 2;   /* a picture is centred in its cell */
    uint8_t rgba[WMAX * CELL * 4], bc3[13 * 10 * 16];
    for (int y = 0; y < CELL; y++) for (int x = 0; x < bw * 4; x++) {
        int n = 0, sx = x + ox;
        if (sx >= 0 && sx < CELL) for (int j = 0; j < SS; j++) for (int i = 0; i < SS; i++) n += s[(y * SS + j) * S + sx * SS + i];
        rgba[(y * bw * 4 + x) * 4 + 3] = (uint8_t)(n * 255 / (SS * SS));
    }
    cell_backing(rgba, bw * 4);
    if (format == FMT_BC3) tex_encode_bc3(rgba, bw * 4, CELL, bc3);
    for (int y = 0; y < CELL; y += 4) for (int x = 0; x < bw * 4; x += 4) {
        uint8_t *to = block_at(mem, c.x + x, c.y + y, bh_log2);
        if (format == FMT_BC3) memcpy(to, bc3 + (y / 4 * bw + x / 4) * 16, 16);
        else prompts_astc_block(rgba + (y * bw * 4 + x) * 4, bw * 4, to);
    }
}

/* mem: the atlas as it sits in memory (block linear). family: 2 playstation, 3 xbox, 4 keyboard. */
void prompts_paint(uint8_t *mem, int format, int bh_log2, int family, int by_letter) {
    typedef void Face(uint8_t *);
    uint8_t *s = malloc(S * S);
    if (!s) return;
    #define PIC(cell, draw) (memset(s, 0, S * S), draw, cell_draw(mem, format, bh_log2, cell, s))
    #define STICK(c, first, moused) do {                                                   \
            const int l = (first), u = l + 1, r = l + 2, d = l + 3, all[4] = {u, l, d, r}, lr[2] = {l, r}, ud[2] = {u, d}; \
            if (moused) { PIC(c.four, mouse(s, 0)); PIC(c.across, mouse(s, 0)); PIC(c.around, mouse(s, 0)); PIC(c.left, mouse(s, 0)); \
                          PIC(c.right, mouse(s, 0)); PIC(c.updown, mouse(s, 0)); PIC(c.up, mouse(s, 0)); PIC(c.down, mouse(s, 0)); PIC(c.plain, mouse(s, 0)); } \
            else { PIC(c.four, keys(s, all, 4)); PIC(c.across, keys(s, lr, 2)); PIC(c.around, keys(s, all, 4)); PIC(c.left, key(s, l)); \
                   PIC(c.right, key(s, r)); PIC(c.updown, keys(s, ud, 2)); PIC(c.up, key(s, u)); PIC(c.down, key(s, d)); PIC(c.plain, keys(s, all, 4)); } \
        } while (0)
    if (family == 4) {
        PIC(A, key(s, 0)); PIC(B, key(s, 1)); PIC(X, key(s, 2)); PIC(Y, key(s, 3));
        PIC(L1, key(s, 6)); PIC(R1, key(s, 7)); PIC(L2, key(s, 8)); PIC(R2, key(s, 9)); PIC(PLUS, key(s, 10)); PIC(MINUS, key(s, 11));
        PIC(STICK_L.click, key(s, 4)); PIC(STICK_R.click, key(s, 5));
        STICK(STICK_L, 16, 0);
        STICK(STICK_R, 20, nnpad_mouse_cfg(NULL, NULL));
        PIC(DOTS[0][0], key(s, 0)); PIC(DOTS[0][1], key(s, 1)); PIC(DOTS[1][0], key(s, 2)); PIC(DOTS[1][1], key(s, 3));   /* where a button sits: its key */
        by_letter = 0;
    } else if (family == 2) {
        Face *a = by_letter ? ps_cross : ps_circle, *b = by_letter ? ps_circle : ps_cross;
        Face *x = by_letter ? ps_square : ps_triangle, *y = by_letter ? ps_triangle : ps_square;
        PIC(A, a(s)); PIC(B, b(s)); PIC(X, x(s)); PIC(Y, y(s));
        PIC(PLUS, bars_across(s)); PIC(MINUS, bars_up(s));
        PIC(L1, bumper(s, "L1")); PIC(R1, bumper(s, "R1")); PIC(L2, trigger(s, "L2")); PIC(R2, trigger(s, "R2"));
    } else {
        if (!by_letter) { cell_swap(mem, A, B, bh_log2); cell_swap(mem, X, Y, bh_log2); }   /* the game's letters are already right */
        PIC(PLUS, bars_across(s)); PIC(MINUS, view(s));
        PIC(L1, bumper(s, "LB")); PIC(R1, bumper(s, "RB")); PIC(L2, trigger(s, "LT")); PIC(R2, trigger(s, "RT"));
    }
    if (by_letter) for (int i = 0; i < 3; i++) cell_swap(mem, DOTS[i][0], DOTS[i][1], bh_log2);
    free(s);
}

/* ---- the atlas in use. It can change at any time: prompts_tick (from player 1's pad poll) repaints it when the device in use,
 * the bindings or the settings change. A set once painted is kept, so going back and forth is one copy. The renderers are
 * told after the copy is complete (nvn_gl_memory_written), so they cannot keep half of it. */
void nvn_gl_memory_written(uint64_t addr, uint64_t size);
static int g_lock;                                  /* spin: a repaint is short and rare */
static uint8_t *g_mem, *g_shown;                    /* the texture's memory; which of the buffers below it holds */
static uint8_t *g_pic[5];                           /* by family; [1] the game's own */
static uint64_t g_pic_sig[5];
static int g_format, g_bh_log2, g_family;
static void lock_(void) { while (__atomic_exchange_n(&g_lock, 1, __ATOMIC_ACQUIRE)) {} }
static void unlock_(void) { __atomic_store_n(&g_lock, 0, __ATOMIC_RELEASE); }

/* mem holds the game's atlas: remember it, and show what is wanted */
void prompts_adopt(uint8_t *mem, int format, int bh_log2) {
    if (!g_pic[1] && !(g_pic[1] = malloc(ATLAS_SIZE))) return;
    lock_();
    memcpy(g_pic[1], mem, ATLAS_SIZE);
    for (int i = 2; i < 5; i++) { free(g_pic[i]); g_pic[i] = NULL; g_pic_sig[i] = 0; }    /* of the last atlas: it may be another format */
    g_mem = mem; g_shown = g_pic[1]; g_format = format; g_bh_log2 = bh_log2; g_family = 1; g_pic_sig[1] = 0;
    unlock_();
    prompts_tick();
}
void prompts_tick(void) {
    if (!g_mem) return;
    int family = nnpad_button_prompts(), by_letter = family > 1 && family < 4 && nnpad_prompts_by_letter();
    uint64_t sig = (uint64_t)by_letter + 1;
    if (family == 4) { for (int i = 0; i < 25; i++) sig = sig * 31 + (uint64_t)nnpad_kb_vk(i); sig = sig * 2 + (uint64_t)nnpad_mouse_cfg(NULL, NULL); }
    if (family == 1) sig = 0;
    lock_();
    if (!g_mem || (family == g_family && sig == g_pic_sig[family])) { unlock_(); return; }
    if (memcmp(g_mem, g_shown, ATLAS_SIZE)) {       /* not what we left there: the game has reused the memory */
        g_mem = NULL;
        unlock_();
        return;
    }
    if (!g_pic[family] && !(g_pic[family] = malloc(ATLAS_SIZE))) { unlock_(); return; }
    if (family > 1 && g_pic_sig[family] != sig) {
        memcpy(g_pic[family], g_pic[1], ATLAS_SIZE);
        prompts_paint(g_pic[family], g_format, g_bh_log2, family, by_letter);
        g_pic_sig[family] = sig;
    }
    memcpy(g_mem, g_pic[family], ATLAS_SIZE);
    g_shown = g_pic[family]; g_family = family;
    nvn_gl_memory_written((uint64_t)(uintptr_t)g_mem, ATLAS_SIZE);
    unlock_();
    printf("button prompts: %s%s\n", family == 1 ? "Switch" : family == 2 ? "PlayStation" : family == 3 ? "Xbox" : "keyboard", by_letter ? ", face buttons by printed letter" : "");
}

/* nvn_gl.c tex_initialize: is this the atlas? */
void prompts_texture(uint8_t *mem, const uint8_t *limit, int format, int width, int height) {
    static const struct { uint64_t hash; int format, bh_log2; } known[] = {{0xf1e63c42819024aull, FMT_ASTC4, 4}, {0x73597bb152190161ull, FMT_BC3, 3}};
    if (width != ATLAS_W || height != ATLAS_H || !mem || (limit && mem + ATLAS_SIZE > limit)) return;
    if (format != FMT_ASTC4 && format != FMT_BC3) return;
    uint64_t h = 0xcbf29ce484222325ull;
    for (int i = 0; i < ATLAS_SIZE; i++) h = (h ^ mem[i]) * 0x100000001b3ull;
    for (int i = 0; i < 2; i++) if (known[i].hash == h && known[i].format == format) prompts_adopt(mem, format, known[i].bh_log2);
}
/* nvn_gl.c tex_finalize */
void prompts_texture_gone(const uint8_t *mem) {
    lock_();
    if (g_mem == mem) g_mem = NULL;
    unlock_();
}
