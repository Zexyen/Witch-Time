/* runtime/rebind.h: a layout moves buttons and nothing else, a mask comes back to the button that presses it, and the game's
 * four types are what 0x7100b22ee0 says.   clang -O2 -Wall -I runtime tests/rebind_test.c -o build/rebind_test && build/rebind_test */
#include <stdio.h>
#include <string.h>
#include "rebind.h"

static int bad;
static void check(const char *what, int ok) { if (!ok) { printf("FAILED: %s\n", what); bad = 1; } }

int main(void) {
    uint8_t r[16];
    rb_identity(r);
    check("identity is a layout", rb_valid(r));
    check("identity leaves every word", rb_to_game(r, 0xdeadbeefu) == 0xdeadbeefu && rb_to_pad(r, 0xdeadbeefu) == 0xdeadbeefu);
    uint32_t all = 0;
    for (int b = 0; b < 16; b++) all |= k_rb_bit[b];
    check("the 16 buttons are the low 16 bits", all == 0xffff);

    /* issue 82: R and ZR changed places. The pad's R dodges (the game's ZR), the sticks' bits are not touched */
    r[RB_R] = RB_ZR; r[RB_ZR] = RB_R;
    check("a swap is a layout", rb_valid(r));
    check("pad R -> game ZR", rb_to_game(r, 0x2000) == 0x4000);
    check("pad ZR -> game R", rb_to_game(r, 0x4000) == 0x2000);
    check("other buttons stay", rb_to_game(r, 0x10) == 0x10);
    check("stick bits stay", rb_to_game(r, 0xf0000) == 0xf0000);
    check("the game's ZR is pressed by the pad's R", rb_to_pad(r, 0x4000) == 0x2000);
    check("the prompt of ZR shows R", rb_number(r, 0xb) == 0xa);
    check("a stick's prompt stays", rb_number(r, 0xc) == 0xc);
    for (uint32_t v = 0; v < 0x10000; v += 7) check("there and back", rb_to_pad(r, rb_to_game(r, v)) == v);

    uint8_t twice[16];
    rb_identity(twice);
    twice[0] = 1;
    check("a button used twice is no layout", !rb_valid(twice));
    twice[0] = 16;
    check("a number out of range is no layout", !rb_valid(twice));

    /* the masks of actions 30..34 (R ZR L ZL D-pad down in Type A), type by type, as 0x7100b22ee0 returns them */
    static const uint32_t want[2][4][5] = {
        {{0x2000, 0x4000, 0x400, 0x800, 4}, {0x2000, 0x4000, 4, 0x800, 0x400}, {0x800, 0x4000, 0x2000, 0x400, 4}, {0x2000, 0x800, 0x400, 0x4000, 4}},
        {{0x2000, 0x4000, 0x400, 0x800, 4}, {0x2000, 0x4000, 4, 0x800, 0x400}, {0x800, 0x4000, 0x2000, 0x400, 4}, {0x4000, 0x2000, 0x400, 0x800, 4}}};
    static const uint32_t type_a[5] = {0x2000, 0x4000, 0x400, 0x800, 4};
    for (int ch = 0; ch < 2; ch++) for (int t = 0; t < 4; t++) {
        rb_preset(ch, t, r);
        check("a type is a layout", rb_valid(r));
        for (int i = 0; i < 5; i++) check("the type's mask", rb_to_pad(r, type_a[i]) == want[ch][t][i]);
    }
    if (!bad) printf("rebind: ok\n");
    return bad;
}
