/* runtime/mx_translate.c, motion vectors: MxIn.prev_cbufs makes a vertex stage run its code twice, first on last frame's
 * blocks (mxpN[] at binding prev_base + N) with gl_Position kept in mx_prev, then as always. A hand-made stage:
 * MOV R2, c3[0]; NOP; EXIT.
 *   clang -O2 -Wall -I runtime tests/mx_prev_test.c runtime/mx_translate.c -o build/mx_prev_test */
#include <stdio.h>
#include <string.h>
#include "mx_translate.h"

static MxOut translate(const uint8_t *code, int len, uint32_t prev) {
    MxIn in; MxOut out;
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.stage = MX_VERTEX; in.code = code; in.code_len = len; in.target = 1; in.prev_cbufs = prev; in.prev_base = 20;
    mx_translate(&in, &out);
    if (!out.glsl) printf("not translated: %s %s\n", out.op, out.why);
    return out;
}

int main(void) {
    uint8_t code[0x50 + 32];                       /* a zeroed program header, then one group of three instructions */
    uint64_t w[4] = {0, 0x4c98078c00070002ull,     /* MOV R2, c3[0] */
                     0x50b0000000000f00ull | 7ull << 16, 0xe30000000000000full | 7ull << 16};      /* NOP, EXIT */
    memset(code, 0, sizeof code);
    memcpy(code + 0x50, w, sizeof w);
    MxOut off = translate(code, sizeof code, 0), on = translate(code, sizeof code, 1u << 3 | 1u << 14), other = translate(code, sizeof code, 1u << 14);
    if (!off.glsl || !on.glsl || !other.glsl) return 1;
    const char *body = strstr(on.glsl, "void mx_body() {"), *prev = strstr(on.glsl, "void mx_body_prev() {"), *mn = strstr(on.glsl, "void main() {");
    int ok = !off.prev_cbufs && !strstr(off.glsl, "mx_prev") && !strstr(off.glsl, "mxp3") && strstr(off.glsl, "void main() {");
    ok = ok && !other.prev_cbufs && !strcmp(other.glsl, off.glsl);      /* a buffer the code does not read: the text as before */
    ok = ok && on.prev_cbufs == 1u << 3 && on.prev_loc == 0 && body && prev && mn && body < prev && prev < mn;
    ok = ok && strstr(on.glsl, "layout(std430, set = 0, binding = 23) readonly buffer PB3 { uint mxp3[]; };") && strstr(on.glsl, "layout(location = 0) out vec4 mx_prev;");
    ok = ok && strstr(body, "r2 = c3[0u];") && strstr(body, "r2 = c3[0u];") < prev;      /* this frame's code is untouched */
    ok = ok && strstr(prev, "r2 = mxp3[0u];") && !strstr(prev, " c3[");
    ok = ok && strstr(mn, " mx_body_prev();\n mx_prev = gl_Position;\n mx_body();\n}");
    if (!ok) puts(on.glsl);
    puts(ok ? "mx_prev_test: ok" : "mx_prev_test: FAILED");
    return !ok;
}
