/* [witchtime] `witch-time-game <main.elf> --pkz-dump <out folder | -> [filter...]`: every entry of the RomFS .pkz archives
 * (%WT_ROMFS%, else ../romfs) written to <out folder>/<entry name> (entry names are unique across the 72 archives),
 * Kraken entries decompressed by the game's own Oodle decoder (lifted main), stored ones copied. "-" decodes and checks
 * without writing. A filter keeps entries whose archive file name or entry name contains it. No window, no game loop:
 * only the image, its relocations and the import handlers (the decoder calls memcpy / memset through them).
 *
 * The game's codec object (main 0x7100cbc698 Init, 0x7100cbc7f8 Read) drives Oodle's streaming API; the same calls here:
 *   0x71012fefd0 OodleLZDecoder_MemorySizeNeeded(compressor, rawLen -1)          -> bytes of decoder memory
 *   0x71012ff110 OodleLZDecoder_Create(compressor 8 = Kraken, rawLen, mem, size) -> decoder (lives in mem)
 *   0x71012ff2f0 OodleLZDecoder_DecodeSome(dec, out {s32 decoded, compUsed, ..}, decBuf, decPos, decSize, decAvail,
 *                comp, compAvail; on the stack fuzzSafe 0, checkCRC 0, verbosity 0, threadPhase 3) -> bool; one quantum
 *                (<= 256 KB) a call, called until the whole entry is out
 *   0x71012ff2d0 OodleLZDecoder_Destroy(dec)  (frees only memory it allocated itself)
 * WT_PKZ_JOBS = threads (default 8). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "plat.h"
#include "runtime.h"
#include "hostrt.h"

#ifdef _WIN32
#define fseek64 _fseeki64
#else
#define fseek64 fseeko
#endif

#define OO_SIZE_NEEDED (GUEST_BASE + 0x12fefd0)
#define OO_CREATE      (GUEST_BASE + 0x12ff110)
#define OO_DECODE_SOME (GUEST_BASE + 0x12ff2f0)
#define OO_DESTROY     (GUEST_BASE + 0x12ff2d0)
#define KRAKEN 8

typedef struct { int arc; char *name; int kraken; uint64_t size, off, csize; } Entry;
static char g_romfs[1024], g_out[1024];
static char **g_arcs;
static Entry *g_ent;
static int g_count;
static volatile long g_next, g_fail, g_done, g_ok;
static volatile long long g_bytes;

static void guest_call(Ctx *c, uint64_t fn) { c->x[30] = 0; call_guest(c, fn); }

/* out <- the entry decompressed; 0 = ok */
static int kraken(Ctx *c, uint64_t stack_top, uint8_t *mem, uint64_t memsize, const uint8_t *src, uint64_t csize,
                  uint8_t *dst, uint64_t size, char *why, size_t whyn) {
    c->x[0] = KRAKEN; c->x[1] = size; c->x[2] = (uintptr_t)mem; c->x[3] = memsize;
    guest_call(c, OO_CREATE);
    uint64_t dec = c->x[0];
    if (!dec) { snprintf(why, whyn, "OodleLZDecoder_Create failed"); return 1; }
    uint64_t pos = 0, cpos = 0;
    int32_t out[4];
    while (pos < size) {
        c->x[31] = stack_top - 0x20;
        uint64_t args[4] = {0, 0, 0, 3};
        memcpy((void *)(uintptr_t)c->x[31], args, sizeof args);
        c->x[0] = dec; c->x[1] = (uintptr_t)out; c->x[2] = (uintptr_t)dst; c->x[3] = pos; c->x[4] = size;
        c->x[5] = size - pos; c->x[6] = (uintptr_t)(src + cpos); c->x[7] = csize - cpos;
        memset(out, 0, sizeof out);
        guest_call(c, OO_DECODE_SOME);
        if (!(uint32_t)c->x[0] || out[0] < 0 || out[1] < 0 || (!out[0] && !out[1]) ||
            pos + (uint64_t)out[0] > size || cpos + (uint64_t)out[1] > csize) {
            snprintf(why, whyn, "DecodeSome failed at %llu of %llu bytes (compressed %llu of %llu)", (unsigned long long)pos,
                     (unsigned long long)size, (unsigned long long)cpos, (unsigned long long)csize);
            break;
        }
        pos += (uint64_t)out[0]; cpos += (uint64_t)out[1];
    }
    c->x[0] = dec;
    guest_call(c, OO_DESTROY);
    if (pos != size) return 1;
    if (cpos != csize) { snprintf(why, whyn, "stream ended after %llu of %llu compressed bytes", (unsigned long long)cpos, (unsigned long long)csize); return 1; }
    return 0;
}

static int write_file(const char *name, const uint8_t *data, uint64_t size) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", g_out, name);
    for (char *p = path + 1; *p; p++)                    /* the out folder and every folder below it */
        if (*p == '/') { *p = 0; CreateDirectoryA(path, NULL); *p = '/'; }
    FILE *f = fopen(path, "wb");
    if (!f) return 1;
    int bad = size && fwrite(data, 1, size, f) != size;
    return fclose(f) || bad;
}

static DWORD WINAPI worker(LPVOID arg) {
    (void)arg;
    Ctx c;
    memset(&c, 0, sizeof c);
    uint8_t *stack = plat_alloc(1 << 20);
    c.tpidr = (uintptr_t)plat_alloc(0x10000);
    uint64_t stack_top = (uintptr_t)stack + (1 << 20);
    c.x[31] = stack_top;
    c.x[0] = KRAKEN; c.x[1] = (uint64_t)-1;
    guest_call(&c, OO_SIZE_NEEDED);
    uint64_t memsize = (uint32_t)c.x[0];
    uint8_t *mem = malloc(memsize);
    char why[256];
    for (long i; (i = __atomic_fetch_add(&g_next, 1, __ATOMIC_RELAXED)) < g_count; ) {
        Entry *e = &g_ent[i];
        char apath[2048];
        snprintf(apath, sizeof apath, "%s/%s", g_romfs, g_arcs[e->arc]);
        uint8_t *src = malloc(e->csize + 64), *dst = e->kraken ? malloc(e->size + 64) : src;
        FILE *f = fopen(apath, "rb");
        int bad = !src || !dst || !f || fseek64(f, (long long)e->off, SEEK_SET) || fread(src, 1, e->csize, f) != e->csize;
        if (f) fclose(f);
        snprintf(why, sizeof why, "cannot read it (or out of memory)");
        if (!bad && e->kraken) bad = kraken(&c, stack_top, mem, memsize, src, e->csize, dst, e->size, why, sizeof why);
        if (!bad && strcmp(g_out, "-") && write_file(e->name, dst, e->size)) { bad = 1; snprintf(why, sizeof why, "cannot write it"); }
        if (bad) { __atomic_fetch_add(&g_fail, 1, __ATOMIC_RELAXED); printf("pkz-dump: FAILED %s %s: %s\n", g_arcs[e->arc], e->name, why); }
        else { __atomic_fetch_add(&g_ok, 1, __ATOMIC_RELAXED); __atomic_fetch_add(&g_bytes, (long long)e->size, __ATOMIC_RELAXED); }
        long d = __atomic_add_fetch(&g_done, 1, __ATOMIC_RELAXED);
        if (d % 1000 == 0) printf("pkz-dump: %ld / %d\n", d, g_count);
        if (dst != src) free(dst);
        free(src);
    }
    free(mem);
    return 0;
}

/* entries of one archive appended to g_ent (layout: docs/RE_NOTES.md section 4, scripts/pkz.py); 0 = ok */
static int read_archive(int arc, char **filters, int nfilters) {
    char path[2048];
    snprintf(path, sizeof path, "%s/%s", g_romfs, g_arcs[arc]);
    FILE *f = fopen(path, "rb");
    uint8_t h[0x20];
    if (!f || fread(h, 1, sizeof h, f) != sizeof h || memcmp(h, "pkzl", 4) || *(uint32_t *)(h + 4) != 0x20000) {
        if (f) fclose(f);
        printf("pkz-dump: %s: not a pkz archive of version 0x20000\n", path);
        return 1;
    }
    uint32_t count = *(uint32_t *)(h + 0x10), toff = *(uint32_t *)(h + 0x14), nsize = *(uint32_t *)(h + 0x18);
    uint64_t fsize = *(uint64_t *)(h + 8);
    uint8_t *t = malloc((size_t)count * 0x20 + nsize + 1);
    int bad = !t || fseek64(f, toff, SEEK_SET) || fread(t, 1, (size_t)count * 0x20 + nsize, f) != (size_t)count * 0x20 + nsize;
    fclose(f);
    if (bad) { printf("pkz-dump: %s: cannot read its entry table\n", path); free(t); return 1; }
    char *names = (char *)t + (size_t)count * 0x20;
    names[nsize] = 0;
    g_ent = realloc(g_ent, sizeof *g_ent * (g_count + count));
    for (uint32_t i = 0; i < count; i++) {
        uint32_t no, co; uint64_t size, off, csize;
        memcpy(&no, t + i * 0x20, 4); memcpy(&co, t + i * 0x20 + 4, 4); memcpy(&size, t + i * 0x20 + 8, 8);
        memcpy(&off, t + i * 0x20 + 0x10, 8); memcpy(&csize, t + i * 0x20 + 0x18, 8);
        const char *name = no < nsize ? names + no : "", *codec = co < nsize ? names + co : "";
        int kr = !strcmp(codec, "OodleKraken");
        /* the archive is untrusted input: names must stay inside the out folder, data inside the file */
        if (!name[0] || name[0] == '/' || strchr(name, '\\') || strstr(name, "..") || strchr(name, ':') ||
            (!kr && (strcmp(codec, "None") || size != csize)) || off + csize > fsize || off + csize < off) {
            printf("pkz-dump: %s: entry %u (%s, codec %s) refused\n", g_arcs[arc], i, name, codec);
            g_fail++;
            continue;
        }
        int keep = !nfilters;
        for (int k = 0; k < nfilters && !keep; k++) keep = strstr(g_arcs[arc], filters[k]) || strstr(name, filters[k]);
        if (keep) g_ent[g_count++] = (Entry){arc, strdup(name), kr, size, off, csize};
    }
    free(t);
    return 0;
}

static int by_size(const void *a, const void *b) {          /* biggest first: the threads finish together */
    uint64_t x = ((const Entry *)a)->size, y = ((const Entry *)b)->size;
    return x < y ? 1 : x > y ? -1 : 0;
}

int pkz_dump(const char *out, char **filters, int nfilters) {
    /* the decoder entry must be the one this build has (setup.py verifies main's build id; this guards a stale image) */
    if (rd32(OO_DECODE_SOME) != 0xd103c3ffu || rd32(OO_CREATE) != 0xa9bc5ff8u) {
        printf("pkz-dump: main's Oodle decoder is not at the expected address (another build of the game?)\n");
        return 1;
    }
    const char *env = getenv("WT_ROMFS");
    snprintf(g_romfs, sizeof g_romfs, "%s", env && env[0] ? env : "../romfs");
    snprintf(g_out, sizeof g_out, "%s", out);
    if (strcmp(out, "-")) CreateDirectoryA(out, NULL);
    char pat[1100];
    snprintf(pat, sizeof pat, "%s/*.pkz", g_romfs);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    int narcs = 0;
    if (h != INVALID_HANDLE_VALUE) {
        do { g_arcs = realloc(g_arcs, sizeof *g_arcs * (narcs + 1)); g_arcs[narcs++] = strdup(fd.cFileName); } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!narcs) { printf("pkz-dump: no .pkz archives in %s (set WT_ROMFS)\n", g_romfs); return 1; }
    for (int a = 0; a < narcs; a++) if (read_archive(a, filters, nfilters)) g_fail++;
    qsort(g_ent, (size_t)g_count, sizeof *g_ent, by_size);
    long nk = 0;
    for (int i = 0; i < g_count; i++) nk += g_ent[i].kraken;
    const char *j = getenv("WT_PKZ_JOBS");
    int jobs = j && atoi(j) > 0 ? atoi(j) : 8;
    printf("pkz-dump: %d archives, %d entries (%ld Kraken, %ld stored) -> %s, %d threads\n", narcs, g_count, nk, g_count - nk,
           strcmp(out, "-") ? out : "(decode only)", jobs);
    struct timespec t0, t1;
    timespec_get(&t0, TIME_UTC);
    HANDLE th[64];
    if (jobs > 64) jobs = 64;
    for (int i = 0; i < jobs; i++) th[i] = CreateThread(NULL, (SIZE_T)64 << 20, worker, NULL, 0, NULL);   /* lifted code runs on the host stack */
    for (int i = 0; i < jobs; i++) WaitForSingleObject(th[i], INFINITE);
    timespec_get(&t1, TIME_UTC);
    double s = (double)(t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;
    printf("pkz-dump: %ld of %d entries ok, %ld failed; %.0f MB out in %.1f s (%.0f MB/s)\n", g_ok, g_count, g_fail, g_bytes / 1e6, s, g_bytes / 1e6 / (s > 0 ? s : 1));
    return g_fail != 0;
}
