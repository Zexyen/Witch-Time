/* runtime/prompts.c through the texture decoders: the ASTC blocks must decode to the luminance and alpha asked for, and each
 * prompt set must paint what it says. With an argument, writes the four sets as <arg>/<family><format>.rgba (256 x 512).
 *   clang -O2 -I runtime tests/prompts_test.c runtime/prompts.c runtime/tex_decode.c -lm -o /tmp/prompts_test && /tmp/prompts_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tex_decode.h"

void prompts_astc_block(const uint8_t *rgba, int stride, uint8_t *out);
void prompts_paint(uint8_t *mem, int format, int bh_log2, int family, int by_letter);
static int g_family = 1, g_written;
int nnpad_button_prompts(void) { return g_family; }
void nvn_gl_memory_written(uint64_t addr, uint64_t size) { g_written++; }
void prompts_adopt(uint8_t *mem, int format, int bh_log2);
void prompts_tick(void);
void prompts_texture_gone(const uint8_t *mem);
int nnpad_prompts_by_letter(void) { return 0; }
void prompts_key_label(int vk, char *out);
int nnpad_mouse_cfg(float *sens, int *invert) { return 1; }
int nnpad_kb_vk(int idx) {                                  /* the defaults of nnpad_sdl.c g_kb_default */
    static const int k[25] = {2 | 0x0d << 8, 0x20 | 8 << 8, 1, 'E', 'C', 4 | 'V' << 8, 'R', 'F', 'Q' | 5 << 8, 0xa0, 0x1b, 9, 0x25 | '1' << 8, 0x26 | '2' << 8,
                              0x27 | '3' << 8, 0x28 | 'T' << 8, 'A', 'W', 'D', 'S', 'J', 'I', 'L', 'K', 0xa2};
    return k[idx];
}

static uint8_t g_mem[0x20000], g_lin[0x20000], g_px[256 * 512 * 4];
static int alpha_at(int x, int y) { return g_px[(y * 256 + x) * 4 + 3]; }
static int red_at(int x, int y) { return g_px[(y * 256 + x) * 4]; }
static void paint(int format, int bh, int family, int by_letter) {
    memset(g_mem, 0, sizeof g_mem);
    if (format == 0x79) for (size_t i = 0; i < sizeof g_mem; i += 16) { static const uint8_t none[16] = {0xfc, 0xfd, 255, 255, 255, 255, 255, 255}; memcpy(g_mem + i, none, 16); }   /* transparent */
    prompts_paint(g_mem, format, bh, family, by_letter);
    tex_bl_to_linear(g_lin, g_mem, 64 * 16, 128, 1, bh, 0);
    tex_decode_rgba8(format, g_lin, 256, 512, g_px);
}
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main(int argc, char **argv) {
    int bad = 0;
    for (int seed = 0; seed < 2000; seed++) {               /* ASTC: luminance and alpha come back apart, within half a step of 7 */
        uint8_t a[64], blk[16], px[64];
        srand((unsigned)seed);
        for (int i = 0; i < 64; i++) a[i] = (uint8_t)rand();
        for (int i = 0; i < 16; i++) a[i * 4 + 1] = a[i * 4 + 2] = a[i * 4];
        prompts_astc_block(a, 4, blk);
        tex_decode_block_rgba8(0x79, blk, px, 16);
        for (int i = 0; i < 16; i++) {
            CHECK(px[i * 4] == px[i * 4 + 1] && px[i * 4] == px[i * 4 + 2]);
            CHECK(abs(px[i * 4] - a[i * 4]) <= 19 && abs(px[i * 4 + 3] - a[i * 4 + 3]) <= 19);
        }
    }
    CHECK(tex_astc_bad_blocks == 0);
    {
        static const struct { int vk; const char *s; } lab[] = {{0, ""}, {'E', "E"}, {'7', "7"}, {0x0d, "ENT"}, {0xa0, "SFT"}, {0x70, "F1"}, {0x7b, "F12"}, {0x61, "N1"}, {0xe0, "E0"}};
        for (int i = 0; i < 9; i++) { char t[4]; prompts_key_label(lab[i].vk, t); CHECK(!strcmp(t, lab[i].s)); }
    }
    static const struct { int format, bh; } f[2] = {{0x79, 4}, {0x44, 3}};
    for (int i = 0; i < 2; i++) {
        paint(f[i].format, f[i].bh, 2, 0);
        CHECK(alpha_at(204 + 20, 20) > 200 && alpha_at(204 + 20 + 5, 20) < 60);        /* A: Circle, ring around a filled centre */
        CHECK(alpha_at(160 + 20, 44 + 20) < 60 && alpha_at(160 + 20 + 7, 44 + 20) > 200); /* B: Cross, cut at the middle */
        CHECK(red_at(204 + 20 + 5, 20) > 200);                                         /* the backing is white in the cut ... */
        CHECK(red_at(204 + 20 - 11, 20) > 200 && red_at(204 + 20 + 11, 20) > 200);     /* ... and just outside the disc ... */
        CHECK(red_at(204 + 20 - 16, 20) < 40 && red_at(204 + 20, 20 + 16) < 40);       /* ... but stops 2 texels out ... */
        CHECK(red_at(204 + 9, 9) < 40 && red_at(204 + 30, 30) < 40);                   /* ... and is round: the corners of its square are black */
        CHECK(alpha_at(56 + 20, 44 + 20) == 0);                                        /* a cell of no prompt stays empty */
        paint(f[i].format, f[i].bh, 2, 1);
        CHECK(alpha_at(204 + 20, 20) < 60);                                            /* by letter A is Cross */
        CHECK(alpha_at(160 + 20, 44 + 20) > 200 && alpha_at(160 + 20 + 5, 44 + 20) < 60);
        paint(f[i].format, f[i].bh, 4, 0);
        CHECK(alpha_at(204 + 20, 12) > 200);                                           /* A: the right mouse button, filled ... */
        CHECK(alpha_at(204 + 16, 12) < 60);                                            /* ... and not the left */
        CHECK(alpha_at(204 + 9, 44 + 20) > 200 && alpha_at(204 + 20, 44 + 19) < 60);   /* Y: a keycap, E's strokes cut out */
        CHECK(alpha_at(12, 11) > 200 && alpha_at(40, 11) > 200);                   /* the wide stick cell: WASD's cap across it */
        if (argc > 1) {
            for (int fam = 2; fam <= 4; fam++) {
                char path[512];
                paint(f[i].format, f[i].bh, fam, 0);
                snprintf(path, sizeof path, "%s/%d%x.rgba", argv[1], fam, f[i].format);
                FILE *o = fopen(path, "wb");
                if (o) { fwrite(g_px, 1, sizeof g_px, o); fclose(o); }
            }
        }
    }
    {   /* the atlas follows the family; back to Switch gives the game's bytes again */
        static uint8_t game[0x20000], live[0x20000];
        for (size_t i = 0; i < sizeof game; i++) game[i] = (uint8_t)(i * 7);
        memcpy(live, game, sizeof live);
        prompts_adopt(live, 0x79, 4);
        CHECK(!memcmp(live, game, sizeof live) && g_written == 0);
        g_family = 2; prompts_tick();
        CHECK(memcmp(live, game, sizeof live) && g_written == 1);
        prompts_tick();
        CHECK(g_written == 1);                              /* nothing changed, nothing written */
        g_family = 4; prompts_tick();
        CHECK(g_written == 2);
        g_family = 1; prompts_tick();
        CHECK(!memcmp(live, game, sizeof live) && g_written == 3);
        g_family = 2; live[100] ^= 1; prompts_tick();        /* the game wrote there: hands off */
        CHECK(g_written == 3);
    }
    printf("prompts_test: %s\n", bad ? "FAILED" : "ok");
    return bad != 0;
}
