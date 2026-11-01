/* [witchtime] #54: ASTC textures already turned into BC7 (`wtbuild textures`; bc7_textures in graphics.ini, on by default). A texture
 * is found by a hash of its ASTC bytes, so nothing of the game's data changes, a mod's texture is simply not found, and
 * anything not found goes the way it always went (decoded here, BC3).
 *   <dir>/index.wtx   "WTXI", u32 version, u32 shards, u32 entries; shards x 32 bytes of file name; the entries, sorted by hash
 *   <dir>/<name>.wtx  LZ4 blocks one after the other (stored == raw: plain BC7); the shard's own entries and a footer follow,
 *                     from which the build tool resumes and writes the index
 * A read opens the file: the workers of tex_prep read at the same time, and a read is rare next to what it saves. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tex_pack.h"

#ifdef _WIN32
#define fseek64 _fseeki64
#else
#define fseek64 fseeko
#endif

static TexPackEntry *g_e;
static uint32_t g_n, g_shards;
static char (*g_name)[32];
static char g_dir[512];

/* every word of the texture: nvn_gl.c mem_hash(p, n, 1), and texpack.rs */
uint64_t tex_pack_hash(const uint8_t *p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ULL ^ n, w;
    for (size_t i = 0; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001B3ULL; h ^= h >> 29; }
    return h;
}
int tex_pack_open(const char *dir) {
    char path[600];
    uint32_t head[4];
    snprintf(g_dir, sizeof g_dir, "%s", dir);
    snprintf(path, sizeof path, "%s/index.wtx", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    if (fread(head, 4, 4, f) == 4 && !memcmp(head, "WTXI", 4) && head[1] == TEX_PACK_VERSION && head[2] <= 256 && head[3] <= 1u << 24) {
        g_name = malloc((size_t)head[2] * 32 + 1); g_e = malloc((size_t)head[3] * sizeof *g_e + 1);
        if (g_name && g_e && fread(g_name, 32, head[2], f) == head[2] && fread(g_e, sizeof *g_e, head[3], f) == head[3]) { g_shards = head[2]; g_n = head[3]; }
    }
    fclose(f);
    for (uint32_t i = 0; i < g_shards; i++) g_name[i][31] = 0;
    return (int)g_n;
}
const TexPackEntry *tex_pack_find(uint64_t hash, size_t data_size, int format, int width, int height, int levels, int layers) {
    uint32_t lo = 0, hi = g_n;
    while (lo < hi) { uint32_t m = lo + (hi - lo) / 2; if (g_e[m].hash < hash) lo = m + 1; else hi = m; }
    for (; lo < g_n && g_e[lo].hash == hash; lo++) {
        const TexPackEntry *e = &g_e[lo];
        if (e->data_size == data_size && (int)e->format == format && (int)e->width == width && (int)e->height == height && e->levels == levels && e->layers == layers) return e;
    }
    return NULL;
}
/* one LZ4 block; a damaged one ends here, it never writes outside `out` */
static int lz4_block(const uint8_t *s, size_t sn, uint8_t *out, size_t dn) {
    const uint8_t *se = s + sn;
    size_t o = 0;
    while (s < se) {
        unsigned tok = *s++;
        size_t lit = tok >> 4, len = tok & 15, off;
        if (lit == 15) { unsigned b; do { if (s >= se) return 0; b = *s++; lit += b; } while (b == 255); }
        if (lit > (size_t)(se - s) || lit > dn - o) return 0;
        memcpy(out + o, s, lit); s += lit; o += lit;
        if (s == se) break;                        /* the last sequence has no match */
        if (se - s < 2) return 0;
        off = (size_t)s[0] | (size_t)s[1] << 8; s += 2;
        if (!off || off > o) return 0;
        if (len == 15) { unsigned b; do { if (s >= se) return 0; b = *s++; len += b; } while (b == 255); }
        len += 4;
        if (len > dn - o) return 0;
        for (size_t i = 0; i < len; i++) out[o + i] = out[o + i - off];      /* overlaps on purpose */
        o += len;
    }
    return o == dn;
}
int tex_pack_read(const TexPackEntry *e, uint8_t *out) {
    char path[600];
    if (e->shard >= g_shards) return 0;
    snprintf(path, sizeof path, "%s/%s", g_dir, g_name[e->shard]);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t *src = e->stored == e->raw ? out : malloc(e->stored);
    int ok = src && !fseek64(f, (long long)e->offset, SEEK_SET) && fread(src, 1, e->stored, f) == e->stored;
    fclose(f);
    if (ok && src != out) ok = lz4_block(src, e->stored, out, e->raw);
    if (src != out) free(src);
    return ok;
}
