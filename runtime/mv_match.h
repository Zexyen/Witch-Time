/* Motion vectors, step 2: last frame's constants of the objects the depth pre-pass draws (docs/STATUS.md "Object motion
 * vectors"). The game keeps no previous transforms; each object's palette moves through a ring of buffer slots, and the game
 * library says which slot was last frame's (game_shim_eh.c h_palette_upload). Here: the bytes of every block of this frame and
 * the last, found again by address. Pure C, tested by tests/mv_match_test.c. */
#ifndef MV_MATCH_H
#define MV_MATCH_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define MV_MAX_BLOCK 0x10000u                      /* larger blocks are not kept (the game's largest seen: 0x2000) */
typedef struct { uint64_t addr; uint32_t size; size_t off; } MvBlk;
typedef struct { MvBlk *b; int n, cap; uint8_t *d; size_t len, dcap; } MvSet;
typedef struct { MvSet s[2]; int cur; } MvStore;

/* a frame ends: this frame's set becomes the last one */
static void mv_flip(MvStore *m) { m->cur ^= 1; m->s[m->cur].n = 0; m->s[m->cur].len = 0; }
static int mv_find(const MvSet *s, uint64_t addr) {      /* shortcut: linear, a few hundred blocks a frame; upgrade to a hash if a scene has thousands */
    for (int i = 0; i < s->n; i++) if (s->b[i].addr == addr) return i;
    return -1;
}
/* a draw's block: kept once per address and frame. Returns 1 when it was kept now, 0 when it was there (or is not kept). */
static int mv_note(MvStore *m, uint64_t addr, uint32_t size, const void *bytes) {
    MvSet *s = &m->s[m->cur];
    if (!addr || !size || size > MV_MAX_BLOCK || mv_find(s, addr) >= 0) return 0;
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 256; s->b = realloc(s->b, (size_t)s->cap * sizeof *s->b); }
    if (s->len + size > s->dcap) { s->dcap = (s->len + size) * 2; s->d = realloc(s->d, s->dcap); }
    memcpy(s->d + s->len, bytes, size);
    s->b[s->n++] = (MvBlk){addr, size, s->len};
    s->len += size;
    return 1;
}
/* last frame's bytes of the block that was at addr then, at least size of them; NULL when it was not drawn */
static const uint8_t *mv_last(const MvStore *m, uint64_t addr, uint32_t size) {
    const MvSet *p = &m->s[m->cur ^ 1];
    int i = addr ? mv_find(p, addr) : -1;
    return i >= 0 && p->b[i].size >= size ? p->d + p->b[i].off : NULL;
}
#endif
