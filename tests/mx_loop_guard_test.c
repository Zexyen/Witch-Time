/* runtime/mx_translate.c, [issue 26]: a step back in a graphics stage is counted (the stage leaves the dispatcher after
 * MX_LOOP_MAX of them), a compute stage is left alone; MxIn.loopdebug sets the limit. Nothing returns or discards inside the
 * dispatcher: EXIT and the limit set pc = 0 and the loop ends; KIL only sets a flag (the pixel runs on, as on Maxwell) and its
 * discard comes after the loop. Hand-made stages:
 * NOP, @P0 BRA back to the NOP, EXIT; and a fragment stage with @P1 KIL in that loop.
 *   clang -O2 -Wall -I runtime tests/mx_loop_guard_test.c runtime/mx_translate.c -o build/mx_loop_guard_test */
#include <stdio.h>
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

int main(void) {
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
    char *k = translate(1, code, sizeof code, 0), *loop_end = k ? strstr(k, "default: pc = 0u; continue;") : NULL;
    ok = ok && loop_end && strstr(k, "if (p1) { if (true) mx_kill = true; }") && !strstr(k, "mx_kill = true; pc = 0u") && strstr(loop_end, "if (mx_kill) discard;");
    for (const char *t = k; ok && (t = strstr(t, "discard")) && t < loop_end; t++) ok = 0;        /* no discard inside the loop */
    for (const char *t = k; ok && (t = strstr(t, "return;")) && t < loop_end; t++) ok = 0;        /* nor a return */
    /* [issue 92] only forward jumps: blocks in order, no dispatcher, no counter; a colour pass's depth test for equality needs the invariant position */
    uint64_t g[4] = {0,
        0x50b0000000000f00ull | 7ull << 16,                            /* NOP */
        0xe24000000000000full,                                         /* @P0 BRA to the EXIT */
        0xe30000000000000full | 7ull << 16};                           /* EXIT */
    memcpy(code + 0x50, g, sizeof g);
    char *s = translate(0, code, sizeof code, 0);
    int fwd = s && strstr(s, "do { if (pc != 3u) continue;") && strstr(s, "pc = 0u; } while (false);") && !strstr(s, "switch (pc)") && !strstr(s, "mx_back") && strstr(s, "invariant gl_Position;");
    if (!fwd && s) puts(s);
    ok = ok && fwd;
    if (!ok && k) puts(k);
    printf("mx_loop_guard_test: %s\n", ok ? "ok" : "FAILED");
    return !ok;
}
