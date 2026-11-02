/* Host implementations of the libc++ (std::__1) functions the game imports
 * from the system module. Objects live in guest memory, so the layouts below
 * must match libc++ on AArch64 exactly.
 *
 * std::string (24 bytes, little endian, default ABI):
 *   short: byte 0 = size << 1, characters from byte 1 (up to 22 + NUL)
 *   long : u64 capacity | 1 at +0, u64 size at +8, char* data at +16
 */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define A5 (c->x[5])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(n) ((void *)(uintptr_t)c->x[n])

typedef uint8_t Str;                                     /* pointer to a guest std::string */

/* String buffers must come from the game's own allocator: most std::string code is inlined in
 * the game and allocates/frees through its own aligned_alloc(align, size) / free(ptr) directly,
 * so a buffer created here can be freed there and vice versa. The stored capacity is the
 * allocation size (libc++ keeps "size | 1"), rounded up to 16 as the inlined code does. */
#include "image_layout.h"          /* [Recomps#1] our main's exported aligned_alloc(align, size) / free(ptr) */
#define GAME_MEMALIGN (GUEST_BASE + GAME_ALIGNED_ALLOC_OFF)
#define GAME_FREE (GUEST_BASE + GAME_FREE_OFF)
static _Thread_local Ctx *t_ctx;                     /* context of the string op in progress */
static char *sbuf_alloc(size_t alloc) {
    Ctx *c = t_ctx;
    uint64_t save[4] = {c->x[0], c->x[1], c->x[2], c->x[30]};
    c->x[0] = 16; c->x[1] = alloc;
    call_guest(c, GAME_MEMALIGN);
    char *p = (char *)(uintptr_t)c->x[0];
    c->x[0] = save[0]; c->x[1] = save[1]; c->x[2] = save[2]; c->x[30] = save[3];
    if (!p) guest_trap("game allocator returned null for a string buffer", alloc);
    return p;
}
static void sbuf_free(char *p, const char *where) {
    (void)where;
    Ctx *c = t_ctx;
    uint64_t save[4] = {c->x[0], c->x[1], c->x[2], c->x[30]};
    c->x[0] = (uint64_t)(uintptr_t)p;
    call_guest(c, GAME_FREE);
    c->x[0] = save[0]; c->x[1] = save[1]; c->x[2] = save[2]; c->x[30] = save[3];
}

static int s_long(const Str *s) { return s[0] & 1; }
static char *s_data(Str *s) { return s_long(s) ? (char *)(uintptr_t)rd64((uint64_t)s + 16) : (char *)s + 1; }
static size_t s_size(const Str *s) { return s_long(s) ? (size_t)rd64((uint64_t)s + 8) : (size_t)(s[0] >> 1); }
static size_t s_cap(const Str *s) { return s_long(s) ? (size_t)(rd64((uint64_t)s) & ~1ull) - 1 : 22; }
static void s_set_size(Str *s, size_t n) { if (s_long(s)) wr64((uint64_t)s + 8, n); else s[0] = (uint8_t)(n << 1); }

static void s_reserve(Str *s, size_t need) {
    size_t cap = s_cap(s);
    if (need <= cap) return;
    size_t want = need > 2 * cap ? need : 2 * cap;
    size_t alloc = (want + 16) & ~(size_t)15;
    size_t size = s_size(s);
    char *p = sbuf_alloc(alloc);
    memcpy(p, s_data(s), size + 1);
    if (s_long(s)) sbuf_free(s_data(s), "reserve");
    wr64((uint64_t)s, alloc | 1);
    wr64((uint64_t)s + 8, size);
    wr64((uint64_t)s + 16, (uint64_t)p);
}
static void s_assign(Str *s, const char *p, size_t n) {
    char *copy = NULL;
    char *d = s_data(s);
    if (p >= d && p <= d + s_size(s)) { copy = malloc(n + 1); memcpy(copy, p, n); p = copy; }   /* self-assignment */
    s_reserve(s, n);
    d = s_data(s);
    memmove(d, p, n);
    d[n] = 0;
    s_set_size(s, n);
    free(copy);
}
static void s_init(Str *s, const char *p, size_t n) { memset(s, 0, 24); s_assign(s, p, n); }

/* For the other C++ runtime files (nncxxrt.c, nncxxlib.c): build / read guest std::string objects and
 * free buffers the game's inlined container code allocated. */
void cxx_string_init(Ctx *c, uint64_t str, const char *p, size_t n) { Ctx *prev = t_ctx; t_ctx = c; s_init((Str *)(uintptr_t)str, p, n); t_ctx = prev; }
void cxx_game_free(Ctx *c, uint64_t p) { Ctx *prev = t_ctx; t_ctx = c; sbuf_free((char *)(uintptr_t)p, "runtime"); t_ctx = prev; }

static void str_to_string_int(Ctx *c) { t_ctx = c; char b[32]; int n = snprintf(b, sizeof b, "%d", (int)A0); s_init((Str *)(uintptr_t)c->x[8], b, (size_t)n); }
static void str_to_string_uint(Ctx *c) { t_ctx = c; char b[32]; int n = snprintf(b, sizeof b, "%u", (unsigned)A0); s_init((Str *)(uintptr_t)c->x[8], b, (size_t)n); }


/* ---- hashing support */
static void next_prime(Ctx *c) {
    uint64_t n = A0;
    if (n <= 2) { RET(2); return; }
    n |= 1;
    for (;; n += 2) {
        int prime = 1;
        for (uint64_t d = 3; d * d <= n; d += 2) if (n % d == 0) { prime = 0; break; }
        if (prime) { RET(n); return; }
    }
}

/* ---- std::sort on plain arrays (comparator object in x2 is std::less) */
#define SORT(name, T) \
    static int cmp_##name(const void *a, const void *b) { T x = *(const T *)a, y = *(const T *)b; return x < y ? -1 : x > y; } \
    static void sort_##name(Ctx *c) { T *first = P(0), *last = P(1); if (last > first) qsort(first, (size_t)(last - first), sizeof(T), cmp_##name); }
SORT(int, int32_t)

/* ---- shared_ptr control block: { vptr, long shared_owners, long shared_weak_owners } */
static void weak_release(Ctx *c) {                       /* __shared_weak_count::__release_weak() */
    uint64_t self = A0;
    volatile LONG64 *weak = (volatile LONG64 *)(uintptr_t)(self + 16);
    /* libc++: when the count is already 0 this is the last reference (no atomic write needed); otherwise
     * decrement atomically and destroy the block when it drops to -1 */
    if (*weak != 0 && InterlockedDecrement64(weak) != -1) return;
    uint64_t saved_lr = c->x[30];
    call_guest(c, rd64(rd64(self) + 4 * 8));             /* virtual __on_zero_shared_weak() */
    c->x[30] = saved_lr;
}
static void weak_lock(Ctx *c) {                          /* __shared_weak_count::lock() */
    uint64_t self = A0;
    volatile LONG64 *owners = (volatile LONG64 *)(uintptr_t)(self + 8);
    for (;;) {                                           /* take a strong reference unless the object is already gone */
        LONG64 seen = *owners;
        if (seen == -1) { RET(0); return; }
        if (InterlockedCompareExchange64(owners, seen + 1, seen) == seen) { RET(self); return; }
    }
}
static void keep(Ctx *c) { (void)c; }
static void ret0(Ctx *c) { RET(0); }
static void throw_trap(Ctx *c) { guest_trap("libc++ threw length_error / out_of_range", c->x[30] - 4); }

/* ---- clocks */
static void steady_now(Ctx *c) {                         /* nanoseconds */
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    /* whole seconds and the remainder separately: exact in 64 bits for any uptime */
    RET((uint64_t)(now.QuadPart / freq.QuadPart) * 1000000000ull + (uint64_t)(now.QuadPart % freq.QuadPart) * 1000000000ull / (uint64_t)freq.QuadPart);
}

/* ---- std::locale: there is only the classic "C" locale. A locale object is one pointer
 * to a shared implementation; every locale compares equal to the classic one, so number
 * formatting takes its fast (no digit grouping) path and never needs a numpunct facet. */
static void locale_dtor(Ctx *c) { (void)c; }
/* locale::use_facet is in nncxxlib.c (ctype<char> and numpunct<char> facets of the classic locale) */

static const ImportDef cxx_imports[] = {
    {"_ZNSt3__19to_stringEi", str_to_string_int}, {"_ZNSt3__19to_stringEj", str_to_string_uint},
    {"_ZNSt3__112__next_primeEm", next_prime},
    {"_ZNSt3__16__sortIRNS_6__lessIiiEEPiEEvT0_S5_T_", sort_int},
    {"_ZNSt3__119__shared_weak_count14__release_weakEv", weak_release}, {"_ZNSt3__119__shared_weak_count4lockEv", weak_lock},
    {"_ZNSt3__119__shared_weak_countD2Ev", keep},
    {"_ZNKSt3__119__shared_weak_count13__get_deleterERKSt9type_info", ret0},
    {"_ZNKSt3__120__vector_base_commonILb1EE20__throw_length_errorEv", throw_trap},
    {"_ZNKSt3__120__vector_base_commonILb1EE20__throw_out_of_rangeEv", throw_trap},
    {"_ZNKSt3__121__basic_string_commonILb1EE20__throw_length_errorEv", throw_trap},
    {"_ZNKSt3__121__basic_string_commonILb1EE20__throw_out_of_rangeEv", throw_trap},
    {"_ZNSt3__16chrono12steady_clock3nowEv", steady_now},
    {"pthread_mutexattr_init", ret0}, {"pthread_mutexattr_settype", ret0}, {"pthread_mutexattr_destroy", ret0},
      {"_ZNSt3__16localeD1Ev", locale_dtor},
};

void register_cxx_imports(void) {
    register_imports(cxx_imports, (int)(sizeof cxx_imports / sizeof cxx_imports[0]));
}
