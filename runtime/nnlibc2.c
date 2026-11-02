/* C library additions: compiler-rt IEEE binary128 helpers (long double on AArch64), C stdio streams on top of
 * nn::fs, localeconv, gmtime, pthread_join.
 *
 * Only names that have no handler yet are installed (see register_libc2_imports).
 *
 * long double: AArch64 passes and returns binary128 in v0/v1 (q registers), so arguments are c->v[n] as 128 bits
 * and results go to c->v[0]. The arithmetic below is a self-contained soft-float (round to nearest even, the only
 * mode the game can be in: it never changes FPCR rounding through these helpers). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define STR(n) ((const char *)(uintptr_t)c->x[n])

/* ================================================================ binary128 soft-float */
typedef unsigned __int128 u128;
#define Q_BIAS 16383
#define Q_FRAC_MASK ((((u128)1) << 112) - 1)
#define Q_IMPLICIT (((u128)1) << 112)
#define Q_INF ((u128)0x7fff << 112)
#define Q_SIGN ((u128)1 << 127)

static int q_is_nan(u128 a) { return (a & ~Q_SIGN) > Q_INF; }
static int q_is_inf(u128 a) { return (a & ~Q_SIGN) == Q_INF; }
static int q_is_zero(u128 a) { return (a & ~Q_SIGN) == 0; }
static int clz128(u128 v) { uint64_t hi = (uint64_t)(v >> 64); return hi ? __builtin_clzll(hi) : 64 + (v ? __builtin_clzll((uint64_t)v) : 64); }

/* Splits a finite non-zero value into exponent and a significand with the leading one at bit 112. */
static void q_unpack(u128 a, int *exp, u128 *sig) {
    int e = (int)((a >> 112) & 0x7fff);
    u128 m = a & Q_FRAC_MASK;
    if (e == 0) { int sh = clz128(m) - 15; m <<= sh; e = 1 - sh; }
    else m |= Q_IMPLICIT;
    *exp = e; *sig = m;
}


u128 q_from_f64(double d) {
    uint64_t bits;
    memcpy(&bits, &d, 8);
    int sign = (int)(bits >> 63), e = (int)((bits >> 52) & 0x7ff);
    uint64_t f = bits & 0xfffffffffffffull;
    if (e == 0x7ff) return ((u128)sign << 127) | Q_INF | (f ? ((u128)1 << 111) | ((u128)f << 60) : 0);
    if (e == 0) {
        if (!f) return (u128)sign << 127;
        int sh = __builtin_clzll(f) - 11;
        f <<= sh; e = 1 - sh; f &= 0xfffffffffffffull;
    }
    return ((u128)sign << 127) | ((u128)(e - 1023 + Q_BIAS) << 112) | ((u128)f << 60);
}
/* Narrowing: rounds the 113-bit significand to `bits` (53 or 24) with the target's exponent range. */
static uint64_t q_narrow(u128 a, int bits, int ebits) {
    int sign = (int)(a >> 127), emax = (1 << ebits) - 1, bias = emax >> 1;
    uint64_t s = (uint64_t)sign << (bits - 1 + ebits);
    if (q_is_nan(a)) return s | ((uint64_t)emax << (bits - 1)) | (1ull << (bits - 2)) | (uint64_t)((a & Q_FRAC_MASK) >> (113 - bits));
    if (q_is_inf(a)) return s | ((uint64_t)emax << (bits - 1));
    if (q_is_zero(a)) return s;
    int e;
    u128 m;
    q_unpack(a, &e, &m);
    e = e - Q_BIAS + bias;
    /* keep bits+3 bits: leading one at bit (bits+2), then guard/round/sticky */
    int shift = 113 - (bits + 3);
    u128 lost = m & (((u128)1 << shift) - 1);
    uint64_t sig = (uint64_t)(m >> shift) | (lost != 0);
    if (e <= 0) {
        int sh = 1 - e;
        if (sh > bits + 3) sig = sig != 0;
        else { uint64_t l = sig & ((1ull << sh) - 1); sig = (sig >> sh) | (l != 0); }
        e = 0;
    }
    unsigned grs = (unsigned)sig & 7;
    sig >>= 3;
    if (grs > 4 || (grs == 4 && (sig & 1))) sig++;
    if (e == 0) { if (sig >> (bits - 1)) e = 1; }
    else if (sig >> bits) { sig >>= 1; e++; }
    if (e >= emax) return s | ((uint64_t)emax << (bits - 1));
    return s | ((uint64_t)e << (bits - 1)) | (sig & ((1ull << (bits - 1)) - 1));
}
double q_to_f64(u128 a) { uint64_t b = q_narrow(a, 53, 11); double d; memcpy(&d, &b, 8); return d; }
void quad_from_double(double d, uint64_t out[2]) { u128 q = q_from_f64(d); out[0] = (uint64_t)q; out[1] = (uint64_t)(q >> 64); }
double quad_to_double(const uint64_t in[2]) { return q_to_f64(((u128)in[1] << 64) | in[0]); }


/* ================================================================ stdio streams
 * A guest FILE * is a pointer to the host structure below (the game only passes it back to these functions).
 * Files are opened through the registered nn::fs handlers, so "rom:/..." and the other mounts resolve exactly
 * as they do for the game's own file access. Reading only: the game's fopen users are Lua's loadfile/io library. */
typedef struct {
    uint32_t magic;                  /* 'FILE' */
    int kind;                        /* 0 nn::fs file, 1 stdin, 2 stdout, 3 stderr */
    uint64_t handle;
    int64_t pos, size;
    int eof, err, open, ungot;       /* ungot: -1 or a pushed-back byte */
} GFile;
#define GFILE_MAGIC 0x454c4946u
static GFile g_std[3] = {{GFILE_MAGIC, 1, 0, 0, 0, 0, 0, 1, -1}, {GFILE_MAGIC, 2, 0, 0, 0, 0, 0, 1, -1}, {GFILE_MAGIC, 3, 0, 0, 0, 0, 0, 1, -1}};
static uint64_t g_std_ptr[3];        /* the objects behind the imported `stdin`, `stdout`, `stderr` (FILE * variables) */
#define FS_OPEN "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"
#define FS_CLOSE "_ZN2nn2fs9CloseFileENS0_10FileHandleE"
#define FS_READ "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm"
#define FS_SIZE "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE"

static uint64_t fs_call(Ctx *c, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    ImportFn fn = import_lookup(name);
    if (!fn) { printf("\nnnlibc2: %s has no handler, needed by stdio\n", name); report_and_exit(3); }
    uint64_t save[5] = {c->x[0], c->x[1], c->x[2], c->x[3], c->x[4]};
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4;
    fn(c);
    uint64_t r = c->x[0];
    c->x[1] = save[1]; c->x[2] = save[2]; c->x[3] = save[3]; c->x[4] = save[4];
    (void)save[0];
    return r;
}
static GFile *gfile(uint64_t p) {
    GFile *f = (GFile *)(uintptr_t)p;
    return f && f->magic == GFILE_MAGIC ? f : NULL;
}
static int gfile_open(Ctx *c, GFile *f, const char *path, const char *mode) {
    uint64_t handle = 0, size = 0;
    if (!path || !mode || mode[0] != 'r' || strchr(mode, '+')) {       /* writing through stdio is not provided */
        trace_printf("     fopen(\"%s\", \"%s\"): only read modes are supported -> NULL\n", path ? path : "(null)", mode ? mode : "(null)");
        return 0;
    }
    if ((uint32_t)fs_call(c, FS_OPEN, (uint64_t)(uintptr_t)&handle, (uint64_t)(uintptr_t)path, 1 /* OpenMode_Read */, 0, 0) != 0) return 0;
    fs_call(c, FS_SIZE, (uint64_t)(uintptr_t)&size, handle, 0, 0, 0);
    f->magic = GFILE_MAGIC; f->kind = 0; f->handle = handle; f->pos = 0; f->size = (int64_t)size;
    f->eof = f->err = 0; f->open = 1; f->ungot = -1;
    return 1;
}
static void gfile_close(Ctx *c, GFile *f) {
    if (f->kind == 0 && f->open) fs_call(c, FS_CLOSE, f->handle, 0, 0, 0, 0);
    f->open = 0;
}
static size_t gfile_read(Ctx *c, GFile *f, void *dst, size_t n) {
    size_t done = 0;
    if (!n) return 0;
    if (f->ungot >= 0) { *(uint8_t *)dst = (uint8_t)f->ungot; f->ungot = -1; done = 1; }
    if (f->kind != 0 || !f->open) { if (done < n) f->eof = 1; return done; }      /* stdin has no data */
    if (done < n) {
        uint64_t got = 0;
        int64_t want = (int64_t)(n - done);
        if (want > f->size - f->pos) want = f->size - f->pos;
        if (want > 0) {
            if ((uint32_t)fs_call(c, FS_READ, (uint64_t)(uintptr_t)&got, f->handle, (uint64_t)f->pos, (uint64_t)(uintptr_t)((uint8_t *)dst + done), (uint64_t)want) != 0) { f->err = 1; got = 0; }
            f->pos += (int64_t)got;
            done += (size_t)got;
        }
        if (done < n && !f->err) f->eof = 1;
    }
    return done;
}
static void c_fopen(Ctx *c) {
    const char *path = STR(0), *mode = STR(1);
    GFile *f = calloc(1, sizeof *f);
    if (!gfile_open(c, f, path, mode)) { free(f); RET(0); return; }
    RET(f);
}
static void c_fclose(Ctx *c) {
    GFile *f = gfile(A0);
    if (!f) { RET(-1); return; }
    gfile_close(c, f);
    if (f != &g_std[0] && f != &g_std[1] && f != &g_std[2]) { f->magic = 0; free(f); }
    RET(0);
}
static void c_fread(Ctx *c) {                           /* (ptr, size, nmemb, stream) -> complete items read */
    GFile *f = gfile(A3);
    uint64_t size = A1, count = A2;
    if (!f || !size || !count) { RET(0); return; }
    size_t got = gfile_read(c, f, (void *)(uintptr_t)A0, (size_t)(size * count));
    RET(got / size);
}
static void c_ferror(Ctx *c) { GFile *f = gfile(A0); RET(f ? f->err : 0); }
static void c_fflush(Ctx *c) { RET(0); }               /* output streams go to the trace unbuffered (see clib.c fprintf) */

/* ================================================================ gmtime / gmtime_r
 * musl struct tm: nine ints (sec, min, hour, mday, mon, year, wday, yday, isdst), long gmtoff at +40, const char *zone at +48. */
int libc2_gmtime(int64_t t, int32_t tm[9]) {
    int64_t days = t / 86400, rem = t % 86400;
    if (rem < 0) { rem += 86400; days--; }
    if (days < -1000000000ll || days > 1000000000ll) return 0;        /* year would overflow an int */
    tm[2] = (int32_t)(rem / 3600); tm[1] = (int32_t)(rem % 3600 / 60); tm[0] = (int32_t)(rem % 60);
    int64_t wd = (days + 4) % 7;                                      /* 1970-01-01 was a Thursday */
    tm[6] = (int32_t)(wd < 0 ? wd + 7 : wd);
    /* civil date from day count (proleptic Gregorian), era-based */
    int64_t z = days + 719468, era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = yoe + era * 400, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    int64_t d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9;
    if (m <= 2) y++;
    int leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    static const int cum[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
    tm[3] = (int32_t)d; tm[4] = (int32_t)(m - 1); tm[5] = (int32_t)(y - 1900);
    tm[7] = cum[m - 1] + (int32_t)d - 1 + (leap && m > 2);
    tm[8] = 0;
    return 1;
}
static void tm_store(uint64_t dst, const int32_t tm[9]) {
    static const char utc[] = "UTC";
    memcpy((void *)(uintptr_t)dst, tm, 36);
    wr32(dst + 36, 0);
    wr64(dst + 40, 0);
    wr64(dst + 48, (uint64_t)(uintptr_t)utc);
}
static void c_gmtime_r(Ctx *c) {
    int32_t tm[9];
    if (!libc2_gmtime((int64_t)rd64(A0), tm)) { RET(0); return; }
    tm_store(A1, tm);
    RET(A1);
}


static const ImportDef libc2_imports[] = {
    {"fopen", c_fopen},  {"fclose", c_fclose}, {"fread", c_fread},
      {"ferror", c_ferror}, {"fflush", c_fflush},
      {"gmtime_r", c_gmtime_r},
};

/* Called before runtime_init (from cxxrt_install_data): the imported FILE * variables. */
void libc2_install_data(void) {
    for (int i = 0; i < 3; i++) g_std_ptr[i] = (uint64_t)(uintptr_t)&g_std[i];
    import_set_address("stdin", &g_std_ptr[0]);
    import_set_address("stdout", &g_std_ptr[1]);
    import_set_address("stderr", &g_std_ptr[2]);
}

void register_libc2_imports(void) {
    int n = (int)(sizeof libc2_imports / sizeof libc2_imports[0]);
    for (int i = 0; i < n; i++)
        if (!import_lookup(libc2_imports[i].name)) register_imports(&libc2_imports[i], 1);
}
