/* runtime/mx_translate.c, [issue 26]: a step back in a graphics stage is counted (the stage leaves the dispatcher after
 * MX_LOOP_MAX of them), a compute stage is left alone; MxIn.loopdebug sets the limit. Nothing returns or discards inside the
 * dispatcher: EXIT and the limit set pc = 0 and the loop ends; KIL only sets a flag (the pixel runs on, as on Maxwell) and its
 * discard comes after the loop; a stage without a dispatcher discards at the KIL. Hand-made stages:
 * NOP, @P0 BRA back to the NOP, EXIT; a fragment stage with @P1 KIL in that loop; and one with @P1 KIL, NOP, EXIT.
 * [issue 257] A fragment stage with a step back and no shuffle runs its blocks in order, each looped stretch in a for (;;);
 * vertex and compute stages keep the dispatcher. "old" as the first argument sets WT_MX_LOOPS=0 and checks the dispatcher
 * text those fragment stages had before.
 *   clang -O2 -Wall -I runtime tests/mx_loop_guard_test.c runtime/mx_translate.c -o build/mx_loop_guard_test
 *   build/mx_loop_guard_test && build/mx_loop_guard_test old */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mx_translate.h"

static char *translate(int stage, const uint8_t *code, int len, unsigned loopdebug) {
    MxIn in; MxOut out;
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.stage = stage; in.code = code; in.code_len = len; in.target = 1; in.loopdebug = loopdebug;
    mx_translate(&in, &out);
    if (!out.glsl) printf("stage %d not translated: %s %s\n", stage, out.op, out.why);
    return out.glsl;
}

int main(int argc, char **argv) {
    int old = argc > 1 && !strcmp(argv[1], "old");
    if (old) putenv("WT_MX_LOOPS=0");
    uint8_t code[0x50 + 32];                       /* a zeroed program header, then one group of three instructions */
    uint64_t w[4] = {0,
        0x50b0000000000f00ull | 7ull << 16,                            /* NOP */
        0xe24000000000000full | (uint64_t)(-16 & 0xffffff) << 20,      /* @P0 BRA to the NOP */
        0xe30000000000000full | 7ull << 16};                           /* EXIT */
    memset(code, 0, sizeof code);
    memcpy(code + 0x50, w, sizeof w);
    char *v = translate(0, code, sizeof code, 0), *c = translate(5, code + 0x50, sizeof w, 0), *d = translate(0, code, sizeof code, 10);
    int ok = v && c && d && strstr(v, "uint mx_back = 0u;") && strstr(v, "if (++mx_back > 4096u) { pc = 0u; continue; }") && !strstr(c, "mx_back") &&
             strstr(d, "if (++mx_back > 10u)") && strstr(v, "while (pc != 0u) { switch (pc) {") && strstr(v, "default: pc = 0u; continue;");
    uint64_t f[4] = {0,
        0xe33000000000000full | 1ull << 16,                            /* @P1 KIL */
        0xe24000000000000full | (uint64_t)(-16 & 0xffffff) << 20,      /* @P0 BRA to the KIL */
        0xe30000000000000full | 7ull << 16};                           /* EXIT */
    memcpy(code + 0x50, f, sizeof f);
    const char *end_mark = old ? "default: pc = 0u; continue;" : " pc = 0u; } while (false);\n if (pc < 1u || pc >= ";      /* the last block's end and the for's */
    char *k = translate(1, code, sizeof code, 0), *loop_end = k ? strstr(k, end_mark) : NULL;
    ok = ok && k && strstr(k, "if (++mx_back > 4096u) { pc = 0u; continue; }") &&
         (old ? strstr(k, "while (pc != 0u) { switch (pc) {") != NULL : strstr(k, " for (;;) {\n do { if (pc != 1u) continue;") && strstr(k, "pc = 1u; continue; }") && !strstr(k, "switch (pc)"));
    ok = ok && loop_end && strstr(k, "if (p1) { if (true) mx_kill = true; }") && !strstr(k, "mx_kill = true; pc = 0u") && strstr(loop_end, "if (mx_kill) discard;");
    for (const char *t = k; ok && (t = strstr(t, "discard")) && t < loop_end; t++) ok = 0;        /* no discard inside the loop */
    for (const char *t = k; ok && (t = strstr(t, "return;")) && t < loop_end; t++) ok = 0;        /* nor a return */
    /* [issue 92] only forward jumps: blocks in order, no dispatcher, no counter; a colour pass's depth test for equality needs the invariant position */
    uint64_t j[4] = {0,
        0x50b0000000000f00ull | 7ull << 16,                            /* NOP */
        0xe24000000000000full,                                         /* @P0 BRA to the EXIT */
        0xe30000000000000full | 7ull << 16};                           /* EXIT */
    memcpy(code + 0x50, j, sizeof j);
    char *s = translate(0, code, sizeof code, 0);
    int fwd = s && strstr(s, "do { if (pc != 3u) continue;") && strstr(s, "pc = 0u; } while (false);") && !strstr(s, "switch (pc)") && !strstr(s, "mx_back") && strstr(s, "invariant gl_Position;");
    char *sf = translate(1, code, sizeof code, 0);                 /* a fragment stage keeps the dispatcher */
    fwd = fwd && sf && strstr(sf, "while (pc != 0u) { switch (pc) {") && !strstr(sf, "do { if (pc");
    if (!fwd && s) puts(s);
    ok = ok && fwd;
    /* [issue 100] a long chain of forward jumps in a fragment stage runs in order, with a SHFL too (issue 107) */
    uint8_t chain[0x50 + 24 * 8];
    uint64_t l[24];
    for (int n = 0; n < 24; n++) l[n] = n % 4 ? 0xe24000000000000full : 0;         /* @P0 BRA to the next instruction */
    l[23] = 0xe30000000000000full | 7ull << 16;                                    /* EXIT */
    memset(chain, 0, sizeof chain);
    memcpy(chain + 0x50, l, sizeof l);
    char *lf = translate(1, chain, sizeof chain, 0);
    l[1] = 0xef10000000000000ull | 7ull << 16 | 2 | 1ull << 8 | 1ull << 20 | 3ull << 28 | 3ull << 30 | 0x1c03ull << 34;      /* SHFL.XOR R2, R1, 1, 0x1c03 */
    memcpy(chain + 0x50, l, sizeof l);
    char *ls = translate(1, chain, sizeof chain, 0);
    int lng = lf && strstr(lf, "do { if (pc != 2u) continue;") && !strstr(lf, "switch (pc)") && ls && strstr(ls, "do { if (pc") && !strstr(ls, "while (subgroupAny(pc != 0u)) {");
    if (!lng && lf) puts(lf);
    ok = ok && lng;
    uint64_t g[4] = {0,
        0xe33000000000000full | 1ull << 16,                            /* @P1 KIL */
        0x50b0000000000f00ull | 7ull << 16,                            /* NOP */
        0xe30000000000000full | 7ull << 16};                           /* EXIT */
    memcpy(code + 0x50, g, sizeof g);
    char *n = translate(1, code, sizeof code, 0);
    ok = ok && n && strstr(n, "if (p1) { if (true) discard; }") && !strstr(n, "mx_kill = true") && !strstr(n, "while (");
    if (!ok && k) puts(k);
    if (!ok && n) puts(n);
    /* [issue 163] a compute BAR leaves the dispatcher: barrier() stands once, in a loop of a fixed count, never inside the switch;
     * with a step back in the stage the old text stays */
    uint64_t bar = 0xf0a8000000000000ull | 1ull << 39 | 1ull << 43 | 1ull << 44 | 7ull << 16;
    uint64_t b1[4] = {0, bar, 0x50b0000000000f00ull | 7ull << 16, 0xe30000000000000full | 7ull << 16};                          /* BAR, NOP, EXIT */
    uint64_t b2[4] = {0, bar, 0xe24000000000000full | (uint64_t)(-16 & 0xffffff) << 20, 0xe30000000000000full | 7ull << 16};    /* BAR, @P0 BRA to it, EXIT */
    char *cb = translate(5, (const uint8_t *)b1, sizeof b1, 0), *cl = translate(5, (const uint8_t *)b2, sizeof b2, 0);
    const char *sw = cb ? strstr(cb, "switch (pc)") : NULL, *first = cb ? strstr(cb, "barrier();") : NULL;
    int bok = sw && first && first < sw && !strstr(first + 1, "barrier();") && strstr(cb, "for (uint mx_b = 0u; mx_b <= 1u; mx_b++) {") &&
              strstr(cb, "{ mx_bar = true; pc = 2u; continue; }") && strstr(cb, "case 2u:") && strstr(cb, "while (pc != 0u && !mx_bar)") &&
              cl && !strstr(cl, "mx_bar") && strstr(cl, "barrier();");
    if (!bok && cb) puts(cb);
    ok = ok && bok;
    /* [issue 257] a loop inside a loop is a for inside a for; a loop after another one is a for of its own */
    if (!old) {
        uint8_t nest[0x50 + 12 * 8];
        uint64_t q[12] = {0,
            0x50b0000000000f00ull | 7ull << 16,                            /* 1: NOP (the outer loop's first block) */
            0x50b0000000000f00ull | 7ull << 16,                            /* 2: NOP (the inner loop's block) */
            0xe24000000000000full | (uint64_t)(-16 & 0xffffff) << 20,      /* 3: @P0 BRA to 2 */
            0,
            0xe24000000000000full | (uint64_t)(-40 & 0xffffff) << 20,      /* 5: @P0 BRA to 1 */
            0x50b0000000000f00ull | 7ull << 16,                            /* 6: NOP (a second loop) */
            0xe24000000000000full | (uint64_t)(-16 & 0xffffff) << 20,      /* 7: @P0 BRA to 6 */
            0,
            0xe30000000000000full | 7ull << 16, 0, 0};                     /* 9: EXIT */
        memset(nest, 0, sizeof nest);
        memcpy(nest + 0x50, q, sizeof q);
        char *t = translate(1, nest, sizeof nest, 0);
        /* (a word after a jump that is no target starts no block: 2, 3 and 5 are one block, and so are 6, 7 and 9) */
        const char *o1 = t ? strstr(t, " for (;;) {\n do { if (pc != 1u) continue;") : NULL, *i1 = o1 ? strstr(o1, " for (;;) {\n do { if (pc != 2u) continue;") : NULL,
                   *ic = i1 ? strstr(i1, " if (pc < 2u || pc >= 6u) break; }\n if (pc < 1u || pc >= 6u) break; }") : NULL,
                   *s2 = ic ? strstr(ic, " for (;;) {\n do { if (pc != 6u) continue;") : NULL, *sc = s2 ? strstr(s2, " pc = 0u; } while (false);\n if (pc < 6u || pc >= 12u) break; }") : NULL;
        int nok = sc && !strstr(t, "switch (pc)");
        if (!nok && t) puts(t);
        ok = ok && nok;
    }
    printf("mx_loop_guard_test: %s\n", ok ? "ok" : "FAILED");
    return !ok;
}
