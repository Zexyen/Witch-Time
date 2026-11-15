/* [witchtime] The debug commands' parsing (game_shim_eh.c debug_tick; docs/RE_NOTES.md section 11). Pure, so
 * tests/debug_cmd_test.c checks it without the game. */
#ifndef DEBUG_CMD_H
#define DEBUG_CMD_H
#include <stdlib.h>
#include <string.h>

/* An object's number: its file name ("em0010", as in em/em0010.dat) or the number itself in hex ("20010"). 0 = not one.
 * The categories are the game's table at 0x7101bfe388. */
static unsigned dbg_obj_id(const char *s) {
    static const struct { const char *name; unsigned cat; } cats[] = {{"pl", 1}, {"em", 2}, {"wp", 3}, {"et", 4}, {"ef", 5}, {"es", 6},
        {"it", 7}, {"sc", 9}, {"um", 0xa}, {"bg", 0xc}, {"bm", 0xd}, {"bh", 0xe}, {"ba", 0xf}};
    unsigned cat = 0;
    char *end;
    /* a name is two letters and four digits; anything shorter is a number ("ef001" is 0xef001, not ef + 001) */
    for (size_t i = 0; strlen(s) == 6 && i < sizeof cats / sizeof cats[0]; i++) if (!strncmp(s, cats[i].name, 2)) { cat = cats[i].cat; s += 2; break; }
    unsigned long v = strtoul(s, &end, 16);
    if (end == s || *end || (cat ? v > 0xffff : v < 0x10000 || v > 0xfffff)) return 0;
    return (unsigned)(cat << 16 | v);
}

/* A phase's number from its sub-phase's name: "p9a0_training" -> 0x9a0, "pc20_ChapterSelect" -> 0xc20. -1 = not one. */
static int dbg_phase_of(const char *name) {
    char num[4] = {0}, *end;
    if (name[0] != 'p' || strlen(name) < 4) return -1;
    memcpy(num, name + 1, 3);
    long v = strtol(num, &end, 16);
    return *end ? -1 : (int)v;
}
#endif
