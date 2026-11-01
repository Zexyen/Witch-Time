/* runtime/tex_pack.c: the hash and LZ4 block texpack.rs writes (its tests hold the same values), the lookup, and what a damaged
 * pack does: it reads as "not there", so the game falls back to decoding.
 *   clang -O2 -I runtime tests/tex_pack_test.c runtime/tex_pack.c -o /tmp/tex_pack_test && /tmp/tex_pack_test */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tex_pack.h"
#ifdef _WIN32
#include <direct.h>
#define mkdir(d, m) _mkdir(d)
#else
#include <sys/stat.h>
#endif

static int bad;
#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); bad++; } } while (0)
static const char DIR_[] = "tex_pack_test_dir";
static void put(const char *name, const void *p, size_t n) {
    char path[128]; snprintf(path, sizeof path, "%s/%s", DIR_, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(p, 1, n, f); fclose(f);
}
static void index_of(uint32_t version, const TexPackEntry *e, uint32_t n) {
    uint8_t b[4096] = "WTXI";
    uint32_t h[3] = {version, 1, n};
    memcpy(b + 4, h, 12); memcpy(b + 16, "s.wtx", 6); memcpy(b + 48, e, n * sizeof *e);
    put("index.wtx", b, 48 + n * sizeof *e);
}
int main(void) {
    static const uint8_t lz4[] = {63, 97, 98, 99, 3, 0, 11, 96, 97, 98, 99, 120, 121, 122};      /* what lz4_flex makes of raw */
    static const char raw[] = "abcabcabcabcabcabcabcabcabcabcabcabcxyz";
    uint8_t shard[256], out[64];
    TexPackEntry e[2];
    CHECK(sizeof(TexPackEntry) == 56);
    CHECK(tex_pack_hash((const uint8_t *)"12345678abcdefgh", 16) == 0xbe51ff8d19fcccd3ULL);
    CHECK(tex_pack_hash((const uint8_t *)"", 0) == 0x9E3779B97F4A7C15ULL);
    mkdir(DIR_, 0777);
    memset(e, 0, sizeof e);
    /* sorted by hash: the same hash twice, told apart by the size of the image */
    e[0].hash = e[1].hash = 42; e[0].data_size = e[1].data_size = 512; e[0].format = e[1].format = 0x87; e[0].levels = e[1].levels = 1; e[0].layers = e[1].layers = 1;
    e[0].width = e[0].height = 8; e[1].width = 16; e[1].height = 16;
    e[0].offset = 3; e[0].stored = sizeof lz4; e[0].raw = sizeof raw - 1;
    e[1].offset = 3 + sizeof lz4; e[1].stored = e[1].raw = 16;
    memset(shard, 0xee, sizeof shard);
    memcpy(shard + 3, lz4, sizeof lz4);
    memcpy(shard + 3 + sizeof lz4, "0123456789abcdef", 16);
    put("s.wtx", shard, 3 + sizeof lz4 + 16);

    index_of(TEX_PACK_VERSION + 1, e, 2);
    CHECK(tex_pack_open(DIR_) == 0);                    /* another version: no pack */
    CHECK(tex_pack_find(42, 512, 0x87, 8, 8, 1, 1) == NULL);

    index_of(TEX_PACK_VERSION, e, 2);
    CHECK(tex_pack_open(DIR_) == 2);
    const TexPackEntry *a = tex_pack_find(42, 512, 0x87, 8, 8, 1, 1), *b = tex_pack_find(42, 512, 0x87, 16, 16, 1, 1);
    CHECK(a && a->width == 8); CHECK(b && b->width == 16);
    CHECK(!tex_pack_find(41, 512, 0x87, 8, 8, 1, 1)); CHECK(!tex_pack_find(43, 512, 0x87, 8, 8, 1, 1));
    CHECK(!tex_pack_find(42, 1024, 0x87, 8, 8, 1, 1)); CHECK(!tex_pack_find(42, 512, 0x79, 8, 8, 1, 1)); CHECK(!tex_pack_find(42, 512, 0x87, 8, 8, 2, 1));
    memset(out, 0, sizeof out);
    CHECK(tex_pack_read(a, out) && !memcmp(out, raw, sizeof raw - 1));
    CHECK(tex_pack_read(b, out) && !memcmp(out, "0123456789abcdef", 16));

    /* damage: every byte of the block in turn, and a match reaching back too far, must not get past the bounds */
    TexPackEntry d = e[0];
    uint8_t dmg[sizeof lz4];
    for (size_t i = 0; i < sizeof lz4; i++)
        for (int v = 0; v < 256; v += 17) {
            memcpy(dmg, lz4, sizeof lz4); dmg[i] = (uint8_t)v;
            memcpy(shard + 3, dmg, sizeof lz4);
            put("s.wtx", shard, 3 + sizeof lz4 + 16);
            memset(out, 0, sizeof out);
            tex_pack_read(&d, out);                     /* either answer: the bytes after e->raw stay */
            for (size_t k = d.raw; k < sizeof out; k++) CHECK(out[k] == 0);
        }
    memcpy(shard + 3, lz4, sizeof lz4);
    shard[3 + 4] = 200;                                 /* offset 200 with 3 bytes out */
    put("s.wtx", shard, 3 + sizeof lz4 + 16);
    CHECK(!tex_pack_read(&d, out));
    put("s.wtx", shard, 3 + 2);                         /* cut short */
    CHECK(!tex_pack_read(&d, out));
    remove("tex_pack_test_dir/s.wtx");
    CHECK(!tex_pack_read(&d, out));                     /* gone */
    d.shard = 9;
    CHECK(!tex_pack_read(&d, out));
    remove("tex_pack_test_dir/index.wtx");
    (void)remove(DIR_);
    printf("%s\n", bad ? "FAIL" : "ok");
    return bad != 0;
}
