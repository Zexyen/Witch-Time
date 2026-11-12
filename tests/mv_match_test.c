/* runtime/mv_match.h: a frame's blocks are kept by address for one more frame.
 *   clang -O2 -Wall -I runtime tests/mv_match_test.c -o build/mv_match_test */
#include <stdio.h>
#include "mv_match.h"

int main(void) {
    static MvStore m;
    float a[16] = {1, 0, 0, 5}, b[32] = {2, 0, 0, -40};
    int ok = mv_note(&m, 0x1000, sizeof a, a) == 1 && mv_note(&m, 0x2000, sizeof b, b) == 1 && mv_note(&m, 0x1000, sizeof a, a) == 0;      /* one block per address and frame */
    ok = ok && !mv_note(&m, 0, sizeof a, a) && !mv_note(&m, 0x3000, MV_MAX_BLOCK + 4, a) && !mv_last(&m, 0x1000, sizeof a);      /* not kept; no last frame yet */
    mv_flip(&m);
    float a2[16] = {1, 0, 0, 5.5f};
    ok = ok && mv_note(&m, 0x1400, sizeof a2, a2) == 1;      /* the object's next slot */
    const uint8_t *pa = mv_last(&m, 0x1000, sizeof a), *pb = mv_last(&m, 0x2000, sizeof b);
    ok = ok && pa && !memcmp(pa, a, sizeof a) && pb && !memcmp(pb, b, sizeof b);
    ok = ok && !mv_last(&m, 0x1400, sizeof a2) && !mv_last(&m, 0, 4) && !mv_last(&m, 0x1000, sizeof b);      /* this frame's; none; shorter than asked */
    mv_flip(&m); mv_flip(&m);                      /* two frames on, the blocks are gone */
    ok = ok && !mv_last(&m, 0x1000, sizeof a) && !mv_last(&m, 0x1400, sizeof a2);
    puts(ok ? "mv_match_test: ok" : "mv_match_test: FAILED");
    return !ok;
}
