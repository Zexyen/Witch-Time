/* runtime/rs_pyramid.h: every size of the game's dynamic resolution keeps its depth levels, and nothing else is caught.
 * clang -O2 -Wall -I runtime tests/rs_pyramid_test.c -o build/rs_pyramid_test */
#include <stdio.h>
#include "rs_pyramid.h"

static int bad;
static void check(const char *what, int ok) { if (!ok) { printf("FAILED: %s\n", what); bad = 1; } }

int main(void) {
    enum { F = RS_PYRAMID_FORMAT, R11G11B10F = 0x3f };
    static const int half[][2] = {{800, 448}, {720, 408}, {680, 384}, {640, 360}, {512, 288}};      /* of 1600x896 .. 1024x576 */
    for (unsigned i = 0; i < sizeof half / sizeof half[0]; i++) {
        int w = half[i][0], h = half[i][1];
        check("the 1/4 level", rs_pyramid_next(F, w, h, 0, F, w / 2, h / 2));
        check("the 1/8 level", rs_pyramid_next(F, w / 2, h / 2, 0, F, w / 4, h / 4));
    }
    check("an odd size rounded down", rs_pyramid_next(F, 280, 157, 0, F, 140, 78));
    check("an odd size rounded up", rs_pyramid_next(F, 280, 157, 0, F, 140, 79));
    check("fs1727: a UV, not the pixel position", !rs_pyramid_next(F, 640, 360, 1, F, 320, 180));
    check("fs1697: the blur chain's format", !rs_pyramid_next(R11G11B10F, 720, 408, 0, R11G11B10F, 360, 204));
    check("the target's format", !rs_pyramid_next(F, 720, 408, 0, R11G11B10F, 360, 204));
    check("the clouds' target is not half the image", !rs_pyramid_next(F, 720, 408, 0, F, 520, 360));
    check("the same size", !rs_pyramid_next(F, 720, 408, 0, F, 720, 408));
    check("two levels down", !rs_pyramid_next(F, 720, 408, 0, F, 180, 102));
    if (!bad) printf("rs_pyramid: ok\n");
    return bad;
}
