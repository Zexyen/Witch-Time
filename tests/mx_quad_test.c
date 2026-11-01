/* runtime/mx_translate.c, [issue 66]: a fragment stage finds its place in the quad from the pixel and reads its neighbours with fine
 * derivatives, never with gl_SubgroupInvocationID, which AMD's compiler counts over the live lanes only. The read is a snapshot taken at
 * the header of a wave-uniform dispatcher (every lane of the quad present), and a SHFL that is not first in its case starts one, entered
 * through the header. Other stages keep the lane id and subgroupShuffle. Hand-made stage: S2R lane, SHFL.XOR, FSWZADD, EXIT.
 *   clang -O2 -Wall -I runtime tests/mx_quad_test.c runtime/mx_translate.c -o build/mx_quad_test */
#include <stdio.h>
#include <string.h>
#include "mx_translate.h"

static char *translate(int stage, const uint8_t *code, int len) {
    MxIn in; MxOut out;
    memset(&in, 0, sizeof in); memset(&out, 0, sizeof out);
    in.stage = stage; in.code = code; in.code_len = len; in.target = 1;
    mx_translate(&in, &out);
    if (!out.glsl) printf("stage %d not translated: %s %s\n", stage, out.op, out.why);
    return out.glsl;
}

int main(void) {
    uint8_t code[0x50 + 64];                       /* a zeroed program header, then two groups of three instructions */
    uint64_t w[8] = {0,
        0xf0c8000000000000ull | 7ull << 16,                                                                        /* S2R R0, lane */
        0xef10000000000000ull | 7ull << 16 | 2 | 1ull << 8 | 1ull << 20 | 3ull << 28 | 3ull << 30 | 0x1c03ull << 34,      /* SHFL.XOR R2, R1, 1, 0x1c03 */
        0x50f8000000000000ull | 7ull << 16 | 3 | 2ull << 8 | 1ull << 20 | 0x99ull << 28,                           /* FSWZADD R3, R2, R1 */
        0,
        0xe30000000000000full | 7ull << 16,                                                                        /* EXIT */
        0x50b0000000000f00ull | 7ull << 16,                                                                        /* NOP */
        0x50b0000000000f00ull | 7ull << 16};                                                                       /* NOP */
    memset(code, 0, sizeof code);
    memcpy(code + 0x50, w, sizeof w);
    char *f = translate(1, code, sizeof code), *c = translate(5, code + 0x50, sizeof w);
    int ok = f && c && strstr(f, "dFdxFine(h)") && strstr(f, "dFdyFine(x)") && strstr(f, "gl_FragCoord.y") &&
             strstr(f, "while (subgroupAny(pc != 0u)) {") && strstr(f, "uint mq_need = subgroupOr(mx_qneed(pc));") &&
             strstr(f, "uvec3 mq_r1 = uvec3(0u); if ((mq_need & 0x1u) != 0u) mq_r1 = mx_quad3(r1);") &&
             strstr(f, "uint mx_qneed(uint p) { switch (p) { case 2u: return 0x1u; default: return 0u; } }") &&
             strstr(f, "{ pc = 2u; continue; }\ncase 2u:") && strstr(f, "sh = m == 1u ? mq_r1.x : m == 2u ? mq_r1.y : m == 3u ? mq_r1.z : sv; r2 = ib ? sh : sv;") &&
             !strstr(f, "subgroupQuadSwap") && !strstr(f, "GL_KHR_shader_subgroup_shuffle") &&
             !strstr(f, "gl_SubgroupInvocationID") && !strstr(f, "subgroupShuffle(") &&
             strstr(c, "subgroupShuffle(sv") && strstr(c, "gl_SubgroupInvocationID") && !strstr(c, "mx_quad") && !strstr(c, "gl_FragCoord");
    if (!ok && f) puts(f);
    if (!ok && c) puts(c);
    printf("mx_quad_test: %s\n", ok ? "ok" : "FAILED");
    return !ok;
}
