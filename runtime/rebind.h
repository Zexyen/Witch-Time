/* [witchtime] Button rebinding, what the game library and the runtime share (docs/RE_NOTES.md section 9).
 * A layout is a permutation r of the 16 Pro buttons (nnpad.h's numbers): r[b] = the button pressed for what button b does in
 * the game's Type A. A permutation, so no action is ever left without a button and press and release edges stay true. */
#ifndef REBIND_H
#define REBIND_H
#include <stdint.h>

/* a Pro button's bit in the game's pad words (0x10 A ...), and its number in the game's prompts (0x7100b22cc0) */
static const uint32_t k_rb_bit[16] = {0x10, 0x20, 0x40, 0x80, 0x1000, 0x8000, 0x400, 0x2000, 0x800, 0x4000, 0x100, 0x200, 1, 8, 2, 4};
static const uint8_t k_rb_num[16] = {4, 5, 6, 7, 0x13, 0x1b, 8, 0xa, 9, 0xb, 2, 3, 0x1e, 0x1c, 0x1f, 0x1d};
enum { RB_L = 6, RB_R = 7, RB_ZL = 8, RB_ZR = 9, RB_DOWN = 15 };

static inline void rb_identity(uint8_t *r) { for (int b = 0; b < 16; b++) r[b] = (uint8_t)b; }
static inline int rb_valid(const uint8_t *r) {
    unsigned seen = 0;
    for (int b = 0; b < 16; b++) { if (r[b] > 15) return 0; seen |= 1u << r[b]; }
    return seen == 0xffff;
}
/* the pad's bits -> what the game should see; the stick-as-button bits above stay */
static inline uint32_t rb_to_game(const uint8_t *r, uint32_t v) {
    uint32_t o = v & 0xffff0000u;
    for (int b = 0; b < 16; b++) if (v & k_rb_bit[r[b]]) o |= k_rb_bit[b];
    return o;
}
/* a mask the game means -> the bits of the pad that press it */
static inline uint32_t rb_to_pad(const uint8_t *r, uint32_t v) {
    uint32_t o = v & 0xffff0000u;
    for (int b = 0; b < 16; b++) if (v & k_rb_bit[b]) o |= k_rb_bit[r[b]];
    return o;
}
static inline int rb_number(const uint8_t *r, int n) {
    for (int b = 0; b < 16; b++) if (k_rb_num[b] == n) return k_rb_num[r[b]];
    return n;
}
/* the game's Type A .. D (0..3) as layouts: only five buttons move. ch: 0 Bayonetta, 1 Viola, whose Type D differs (0x7100b22ee0) */
static inline void rb_preset(int ch, int type, uint8_t *r) {
    static const uint8_t slot[5] = {RB_R, RB_ZR, RB_L, RB_ZL, RB_DOWN};
    static const uint8_t bt[4][5] = {{RB_R, RB_ZR, RB_L, RB_ZL, RB_DOWN}, {RB_R, RB_ZR, RB_DOWN, RB_ZL, RB_L},
                                     {RB_ZL, RB_ZR, RB_R, RB_L, RB_DOWN}, {RB_R, RB_ZL, RB_L, RB_ZR, RB_DOWN}};
    static const uint8_t violad[5] = {RB_ZR, RB_R, RB_L, RB_ZL, RB_DOWN};
    const uint8_t *t = ch == 1 && type == 3 ? violad : bt[type & 3];
    rb_identity(r);
    for (int i = 0; i < 5; i++) r[slot[i]] = t[i];
}
#endif
