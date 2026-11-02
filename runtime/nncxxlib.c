/* libc++ (std::__1) pieces beyond libcxx.c / nniostream.c: futures, locale facets, random_device, stream vtables.
 * Layouts marked [sdk] were read from the real SDK module (python sdkdis.py / sdkdata.py <symbol>).
 *
 * std::__assoc_sub_state [sdk: promise<void>::promise() allocates 0x48 bytes and initialises them; set_value,
 * __sub_wait, ~promise read the fields]:
 *   +0x00 vptr (vtable: ~D1, ~D0, __on_zero_shared, __execute)      +0x08 long shared_owners (0 = one owner)
 *   +0x10 exception_ptr                                            +0x18 mutex (0x20 bytes)
 *   +0x38 condition_variable (8 bytes)                             +0x40 unsigned state
 *   state bits: 1 = value constructed, 2 = future attached, 4 = ready, 8 = deferred
 * promise<void> and future<void> are one pointer to that state. The mutex and condition variable are driven through
 * the registered std::mutex / std::condition_variable handlers (os_threads.c), exactly like the game's inlined code.
 *
 * std::random_device [sdk]: on this platform it is NOT a hardware source: the constructor seeds a mt19937 with the
 * default seed 5489 inside the object (624 words, index at +0x9c0) and operator() is the generator's next output, so
 * every random_device yields the same sequence 3499211612, 581869302, ... The game sees the same here.
 *
 * Locale facets [sdk]: ctype<char> = { vptr, refs, const unsigned short *table, bool del }, classic table copied from
 * the SDK (mask bits: 0x1 blank, 0x2 cntrl, 0x4 punct, 0x100 upper, 0x200 lower, 0x400 alpha, 0x800 digit,
 * 0x1000 xdigit, 0x2000 space, 0x4000 print); numpunct<char> = { vptr, refs, char '.', char ',', string grouping "" }.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <windows.h>
#include "image_layout.h"          /* [Recomps#1] GAME_FREE_OFF */
#include "runtime.h"
#include "hostrt.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P64(p) ((uint64_t)(uintptr_t)(p))

/* nncxxrt.c */
uint64_t cxxrt_call(Ctx *c, uint64_t addr, uint64_t a0, uint64_t a1, uint64_t a2);
void cxxrt_guest_delete(Ctx *c, uint64_t p);
uint64_t cxxrt_allocate_exception(size_t size);
void cxxrt_exception_init(uint64_t obj, uint64_t type, uint64_t dtor);
void cxxrt_exception_release(Ctx *c, uint64_t obj);
void cxxrt_throw_report(Ctx *c, const char *how, uint64_t obj, uint64_t type);
uint64_t cxxrt_refstring_new(const char *s, size_t n);
uint64_t cxxrt_ti_logic_error(void);
uint64_t cxxrt_class_ti_vptr(int kind);
/* libcxx.c */
void cxx_string_init(Ctx *c, uint64_t str, const char *p, size_t n);

/* a short std::string (at most 22 characters) written in place: no allocation */
static void short_string(uint64_t s, const char *text) {
    size_t n = strlen(text);
    memset((void *)(uintptr_t)s, 0, 24);
    wr8(s, (uint8_t)(n << 1));
    memcpy((void *)(uintptr_t)(s + 1), text, n);
}

/* ================================================================ future_error / future_category */
static uint64_t g_vt_future_error[5], g_ti_future_error[3];
static uint64_t g_vt_future_category[9], g_ti_future_category[3], g_ti_error_category[2], g_future_category[1];
static const char *future_message(int ev) {
    switch (ev) {                                       /* libc++ __future_error_category::message; future_errc values */
    case 4: return "The associated promise has been destructed prior to the associated state becoming ready.";
    case 1: return "The future has already been retrieved from the promise or packaged_task.";
    case 2: return "The state of the promise has already been set.";
    case 3: return "Operation not permitted on an object without an associated state.";
    }
    return "unspecified future_errc value\n";
}
static void future_error_build(uint64_t self, int ev) {
    const char *msg = future_message(ev);
    wr64(self, P64(&g_vt_future_error[2]));
    wr64(self + 8, cxxrt_refstring_new(msg, strlen(msg)));
    wr64(self + 0x10, (uint32_t)ev);
    wr64(self + 0x18, P64(g_future_category));
}
static void throw_future_error(Ctx *c, int ev) {
    uint64_t obj = cxxrt_allocate_exception(0x20);
    future_error_build(obj, ev);
    cxxrt_exception_init(obj, P64(g_ti_future_error), g_vt_future_error[2]);
    cxxrt_throw_report(c, "throw by the future/promise library", obj, P64(g_ti_future_error));
}
static void cat_dtor(Ctx *c) { RET(A0); }
static void cat_name(Ctx *c) { RET("future"); }
static void cat_default_condition(Ctx *c) { uint64_t self = A0; RET((uint32_t)A1); c->x[1] = self; }      /* error_condition { int, category * } in x0, x1 */
static void cat_equivalent_cond(Ctx *c) { RET((uint32_t)rd32(A2) == (uint32_t)A1 && rd64(A2 + 8) == A0); } /* (int code, const error_condition &) */
static void cat_equivalent_code(Ctx *c) { RET(rd64(A1 + 8) == A0 && rd32(A1) == (uint32_t)A2); }           /* (const error_code &, int cond) */
static void cat_message(Ctx *c) {                       /* string message(int) const: result through x8 */
    const char *m = future_message((int)(int32_t)A1);
    cxx_string_init(c, c->x[8], m, strlen(m));
}

/* ================================================================ __assoc_sub_state, promise<void>, future<void> */
enum { ST_OWNERS = 8, ST_EXCEPTION = 0x10, ST_MUTEX = 0x18, ST_CV = 0x38, ST_STATE = 0x40, ST_SIZE = 0x48 };
enum { S_CONSTRUCTED = 1, S_ATTACHED = 2, S_READY = 4, S_DEFERRED = 8 };
static uint64_t g_vt_assoc[6], g_ti_assoc[3], g_ti_shared_count2[2];

static void assoc_dtor(Ctx *c) {                        /* ~__assoc_sub_state: the members' destructors */
    uint64_t st = A0;
    cxxrt_exception_release(c, rd64(st + ST_EXCEPTION));
    wr64(st + ST_EXCEPTION, 0);
    RET(st);
}
static void assoc_dtor_delete(Ctx *c) {                 /* only objects created by promise<void>() below have this entry */
    uint64_t st = A0;
    cxxrt_exception_release(c, rd64(st + ST_EXCEPTION));
    _aligned_free((void *)(uintptr_t)st);
}
static void assoc_on_zero_shared(Ctx *c) { uint64_t st = A0; if (st) cxxrt_call(c, rd64(rd64(st) + 8), st, 0, 0); }   /* [sdk] delete this (virtual D0) */
static void assoc_execute(Ctx *c) { throw_future_error(c, 3); }                                                      /* [sdk] base __execute throws no_state */


/* ================================================================ locale facets of the classic locale */
/* [Recomps#1] ctype<char>::classic_table(): the mask of every char in the "C" locale, built here from public definitions
 * (2026-10-16; upstream copied the 256 values from the SDK). Classes per character: libc++ src/locale.cpp, the
 * builtin_table of ctype<char>::classic_table() (_LIBCPP_PROVIDES_DEFAULT_RUNE_TABLE), which is the C standard's "C"
 * locale (ISO C 7.4.1): space = cntrl|space|blank for \t, cntrl|space for \n \v \f \r, space|blank|print for ' ', no
 * class above 0x7f. Bit of each class: libc++ include/__locale, ctype_base for __GLIBC__ = glibc <ctype.h> _ISbit(n) on a
 * little-endian target, upper..print = bits 0..6 -> 0x100..0x4000, blank/cntrl/punct = bits 8..10 -> 0x1/0x2/0x4 (libc++
 * treats graph and alnum as composites and stores neither). Gives the same 256 values as upstream's table. */
enum { CT_BLANK = 0x1, CT_CNTRL = 0x2, CT_PUNCT = 0x4, CT_UPPER = 0x100, CT_LOWER = 0x200, CT_ALPHA = 0x400,
       CT_DIGIT = 0x800, CT_XDIGIT = 0x1000, CT_SPACE = 0x2000, CT_PRINT = 0x4000 };
#define CT_UP(ch) ((ch) >= 'A' && (ch) <= 'Z')
#define CT_LO(ch) ((ch) >= 'a' && (ch) <= 'z')
#define CT_DI(ch) ((ch) >= '0' && (ch) <= '9')
#define CT_GR(ch) ((ch) > ' ' && (ch) < 0x7f)
#define CT(ch) ((((ch) < ' ' || (ch) == 0x7f) ? CT_CNTRL : 0) | (((ch) == ' ' || ((ch) >= '\t' && (ch) <= '\r')) ? CT_SPACE : 0) | \
                (((ch) == ' ' || (ch) == '\t') ? CT_BLANK : 0) | (((ch) >= ' ' && (ch) < 0x7f) ? CT_PRINT : 0) | \
                (CT_UP(ch) ? CT_UPPER | CT_ALPHA : 0) | (CT_LO(ch) ? CT_LOWER | CT_ALPHA : 0) | (CT_DI(ch) ? CT_DIGIT : 0) | \
                ((CT_DI(ch) || ((ch) >= 'A' && (ch) <= 'F') || ((ch) >= 'a' && (ch) <= 'f')) ? CT_XDIGIT : 0) | \
                ((CT_GR(ch) && !CT_UP(ch) && !CT_LO(ch) && !CT_DI(ch)) ? CT_PUNCT : 0))
#define CT4(b) CT(b), CT(b + 1), CT(b + 2), CT(b + 3)
#define CT16(b) CT4(b), CT4(b + 4), CT4(b + 8), CT4(b + 12)
#define CT64(b) CT16(b), CT16(b + 16), CT16(b + 32), CT16(b + 48)
static const uint16_t classic_table[256] = { CT64(0), CT64(64), CT64(128), CT64(192) };
static uint64_t g_id_ctype[2], g_id_numpunct[2];        /* the imported locale::id objects: only their addresses matter */
static uint64_t g_vt_ctype[13], g_ti_ctype[2], g_ctype[4];
static uint64_t g_vt_numpunct[10], g_ti_numpunct[2], g_numpunct[6];
static uint8_t g_classic_impl[64];

static int lower(int ch) { return ch >= 'a' && ch <= 'z'; }
static int upper(int ch) { return ch >= 'A' && ch <= 'Z'; }
static void facet_dtor(Ctx *c) { RET(A0); }             /* the facets are static: nothing to destroy or free */
static void facet_nop(Ctx *c) { (void)c; }
static void ct_toupper(Ctx *c) { int ch = (uint8_t)A1; RET(lower(ch) ? ch - 0x20 : ch); }
static void ct_toupper_range(Ctx *c) { for (uint8_t *p = (uint8_t *)(uintptr_t)A1; P64(p) < A2; p++) if (lower(*p)) *p -= 0x20; RET(A2); }
static void ct_tolower(Ctx *c) { int ch = (uint8_t)A1; RET(upper(ch) ? ch + 0x20 : ch); }
static void ct_tolower_range(Ctx *c) { for (uint8_t *p = (uint8_t *)(uintptr_t)A1; P64(p) < A2; p++) if (upper(*p)) *p += 0x20; RET(A2); }
static void ct_widen(Ctx *c) { RET((uint8_t)A1); }
static void ct_widen_range(Ctx *c) { memmove((void *)(uintptr_t)A3, (void *)(uintptr_t)A1, (size_t)(A2 - A1)); RET(A2); }     /* (low, high, dest) */
static void ct_narrow(Ctx *c) { RET((int8_t)A1 >= 0 ? (uint8_t)A1 : (uint8_t)A2); }                                          /* [sdk] non-ASCII -> default */
static void ct_narrow_range(Ctx *c) {                   /* (low, high, dfault, dest) */
    uint8_t *d = (uint8_t *)(uintptr_t)A4;
    for (const int8_t *p = (const int8_t *)(uintptr_t)A1; P64(p) < A2; p++) *d++ = *p >= 0 ? (uint8_t)*p : (uint8_t)A3;
    RET(A2);
}
static void np_decimal_point(Ctx *c) { RET(rd8(A0 + 0x10)); }
static void np_thousands_sep(Ctx *c) { RET(rd8(A0 + 0x11)); }
static void np_grouping(Ctx *c) { short_string(c->x[8], ""); }
static void np_truename(Ctx *c) { short_string(c->x[8], "true"); }
static void np_falsename(Ctx *c) { short_string(c->x[8], "false"); }
static void locale_use_facet(Ctx *c) {                  /* const facet *locale::use_facet(id &) const */
    uint64_t id = A1;
    if (id == P64(g_id_ctype)) { RET(g_ctype); return; }
    if (id == P64(g_id_numpunct)) { RET(g_numpunct); return; }
    printf("\nC++ RUNTIME: std::locale::use_facet for an id at 0x%llx that is neither ctype<char> nor numpunct<char> (std::bad_cast); guest caller 0x%llx\n",
           (unsigned long long)id, (unsigned long long)c->x[30] - 4);
    cxxrt_throw_report(c, "throw by std::locale::use_facet", 0, 0);
}


/* ================================================================ basic_ostream / basic_istream vtables
 * [sdk] _ZTVNSt3__113basic_ostreamIcNS_11char_traitsIcEEEE: { vbase offset 8, 0, typeinfo, D1, D0,
 *   vcall offset -8, offset-to-top -8, typeinfo, virtual thunk D1, virtual thunk D0 } (istream: 0x10 / -0x10).
 *
 * Destructors, re-implemented from the SDK's code (fake audit, closing session):
 *   [sdk 0x464d38] ios_base::~ios_base: vptr = ios_base vtable; for i = event_size-1 .. 0: fn[i](erase_event = 0, *this, index[i])
 *       (fn array +0x38, index array +0x40, event_size +0x48); ~locale() on +0x30; free(+0x38), free(+0x40), free(+0x58), free(+0x70)
 *       with the C `free` the SDK module imports from the game.
 *   [sdk 0x464dc0] basic_ios::~basic_ios (D2) = tail call to it.
 *   [sdk 0x46bd10] ostream D1: `add x0, x0, #8; b ios_base D1`; [sdk 0x46bd30] D0: the same, then operator delete(this).
 *       istream ([sdk] _ZNSt3__113basic_istream...D1Ev): its basic_ios part is at +0x10.
 *   [sdk 0x4656c0] basic_streambuf D2: vptr = basic_streambuf vtable; ~locale() on +8.
 *   [sdk 0x46be38] ostream::sentry::~sentry: if rdbuf && state == good && (flags & unitbuf) && !uncaught_exception():
 *       rdbuf->pubsync() == -1 -> setstate(badbit).
 * The vptr written is the SDK image's own vtable (mapped by sdklift_rt.c), as on the console. */
uint64_t sdk_symbol(const char *name);                    /* sdklift_rt.c */
static uint64_t g_vt_ostream[10], g_vt_istream[10], g_ti_ostream[2], g_ti_istream[2];
static void call_handler(Ctx *c, const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("nncxxlib: no handler for %s\n", name); guest_trap("missing C++ library handler", c->x[30] - 4); }
    f(c);
}
static void ios_base_destroy(Ctx *c, uint64_t ios) {
    uint64_t vt = sdk_symbol("_ZTVNSt3__18ios_baseE");
    if (vt) wr64(ios, vt + 0x10);
    for (uint64_t n = rd64(ios + 0x48); n-- > 0;) {
        uint64_t fn = rd64(rd64(ios + 0x38) + n * 8);
        c->x[2] = rd32(rd64(ios + 0x40) + n * 4);
        c->x[0] = 0; c->x[1] = ios;
        call_guest(c, fn);
    }
    c->x[0] = ios + 0x30; call_handler(c, "_ZNSt3__16localeD1Ev");
    static const int arrays[4] = {0x38, 0x40, 0x58, 0x70};
    for (int i = 0; i < 4; i++) { c->x[0] = rd64(ios + (uint64_t)arrays[i]); if (c->x[0]) call_guest(c, GUEST_BASE + GAME_FREE_OFF); }   /* [Recomps#1] our main's free */   /* free(NULL) does nothing */
}
static void ios_dtor(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self); RET(self); }
static void sbuf_dtor(Ctx *c) {
    uint64_t self = A0, vt = sdk_symbol("_ZTVNSt3__115basic_streambufIcNS_11char_traitsIcEEEE");
    if (vt) wr64(self, vt + 0x10);
    c->x[0] = self + 8; call_handler(c, "_ZNSt3__16localeD1Ev");
    RET(self);
}
static void sentry_dtor(Ctx *c) {
    uint64_t s = A0, os = rd64(s + 8), ios = os + rd64(rd64(os) - 0x18);
    if (rd64(ios + 0x28) && rd32(ios + 0x20) == 0 && (rd8(ios + 9) & 0x20)) {
        call_handler(c, "_ZSt18uncaught_exceptionv");
        if (!(c->x[0] & 1)) {
            ios = rd64(s + 8) + rd64(rd64(rd64(s + 8)) - 0x18);
            uint64_t sb = rd64(ios + 0x28);
            c->x[0] = sb;
            call_guest(c, rd64(rd64(sb) + 0x30));                                   /* pubsync -> virtual sync() */
            if ((uint32_t)c->x[0] == 0xffffffffu) {
                ios = rd64(s + 8) + rd64(rd64(rd64(s + 8)) - 0x18);
                c->x[0] = ios; c->x[1] = rd32(ios + 0x20) | 1;
                call_handler(c, "_ZNSt3__18ios_base5clearEj");
            }
        }
    }
    RET(s);
}
/* complete-object destructors: `this` is the stream; its basic_ios part is at +8 (ostream) or +0x10 (istream) = vtable[-3] */
static void stream_dtor(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + rd64(rd64(self) - 0x18)); RET(self); }
static void ostream_d1(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 8); RET(self); }
static void istream_d1(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 0x10); RET(self); }
static void ostream_d0(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 8); cxxrt_guest_delete(c, self); }
static void istream_d0(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + 0x10); cxxrt_guest_delete(c, self); }
static void stream_dtor_delete(Ctx *c) { uint64_t self = A0; ios_base_destroy(c, self + rd64(rd64(self) - 0x18)); cxxrt_guest_delete(c, self); }
/* virtual thunks: `this` is the basic_ios part; this += vcall offset (vtable[-3] of the ios vptr) gives the stream */
static void stream_thunk_dtor(Ctx *c) { uint64_t ios = A0, self = ios + rd64(rd64(ios) - 0x18); ios_base_destroy(c, ios); RET(self); }
static void stream_thunk_dtor_delete(Ctx *c) { uint64_t ios = A0, self = ios + rd64(rd64(ios) - 0x18); ios_base_destroy(c, ios); cxxrt_guest_delete(c, self); }

static void make_stream_vtable(uint64_t *vt, uint64_t *ti, const char *cls, int64_t ios_offset) {
    char name[72];
    vt[0] = (uint64_t)ios_offset; vt[1] = 0; vt[2] = P64(ti);
    snprintf(name, sizeof name, "std::%s::~%s [D1]", cls, cls); vt[3] = hostfn_address(name, stream_dtor);
    snprintf(name, sizeof name, "std::%s::~%s [D0]", cls, cls); vt[4] = hostfn_address(name, stream_dtor_delete);
    vt[5] = (uint64_t)-ios_offset; vt[6] = (uint64_t)-ios_offset; vt[7] = P64(ti);
    snprintf(name, sizeof name, "virtual thunk to std::%s::~%s [D1]", cls, cls); vt[8] = hostfn_address(name, stream_thunk_dtor);
    snprintf(name, sizeof name, "virtual thunk to std::%s::~%s [D0]", cls, cls); vt[9] = hostfn_address(name, stream_thunk_dtor_delete);
}

void cxxlib_install_data(void) {
    uint64_t vt_class = cxxrt_class_ti_vptr(1), vt_si = cxxrt_class_ti_vptr(2);
    /* future_error, future_category */
    g_ti_future_error[0] = vt_si; g_ti_future_error[1] = P64("NSt3__112future_errorE"); g_ti_future_error[2] = cxxrt_ti_logic_error();
    g_vt_future_error[0] = 0; g_vt_future_error[1] = P64(g_ti_future_error);
    g_vt_future_error[2] = hostfn_address("std::logic_error::~std::logic_error [D1]", NULL);       /* same entries as logic_error (nncxxrt.c) */
    g_vt_future_error[3] = hostfn_address("std::logic_error::~std::logic_error [D0]", NULL);
    g_vt_future_error[4] = hostfn_address("std::logic_error::what", NULL);
    g_ti_error_category[0] = vt_class; g_ti_error_category[1] = P64("NSt3__114error_categoryE");
    g_ti_future_category[0] = vt_si; g_ti_future_category[1] = P64("NSt3__123__future_error_categoryE"); g_ti_future_category[2] = P64(g_ti_error_category);
    g_vt_future_category[0] = 0; g_vt_future_category[1] = P64(g_ti_future_category);
    g_vt_future_category[2] = hostfn_address("future_category::~ [D1]", cat_dtor);
    g_vt_future_category[3] = hostfn_address("future_category::~ [D0]", cat_dtor);
    g_vt_future_category[4] = hostfn_address("future_category::name", cat_name);
    g_vt_future_category[5] = hostfn_address("error_category::default_error_condition", cat_default_condition);
    g_vt_future_category[6] = hostfn_address("error_category::equivalent(int, error_condition)", cat_equivalent_cond);
    g_vt_future_category[7] = hostfn_address("error_category::equivalent(error_code, int)", cat_equivalent_code);
    g_vt_future_category[8] = hostfn_address("future_category::message", cat_message);
    g_future_category[0] = P64(&g_vt_future_category[2]);
    import_set_address("_ZTVNSt3__112future_errorE", g_vt_future_error);
    import_set_address("_ZTINSt3__112future_errorE", g_ti_future_error);

    /* __assoc_sub_state: [sdk] { 0, typeinfo, D1, D0, __on_zero_shared, __execute } */
    g_ti_shared_count2[0] = vt_class; g_ti_shared_count2[1] = P64("NSt3__114__shared_countE");
    g_ti_assoc[0] = vt_si; g_ti_assoc[1] = P64("NSt3__117__assoc_sub_stateE"); g_ti_assoc[2] = P64(g_ti_shared_count2);
    g_vt_assoc[0] = 0; g_vt_assoc[1] = P64(g_ti_assoc);
    g_vt_assoc[2] = hostfn_address("std::__assoc_sub_state::~ [D1]", assoc_dtor);
    g_vt_assoc[3] = hostfn_address("std::__assoc_sub_state::~ [D0]", assoc_dtor_delete);
    g_vt_assoc[4] = hostfn_address("std::__assoc_sub_state::__on_zero_shared", assoc_on_zero_shared);
    g_vt_assoc[5] = hostfn_address("std::__assoc_sub_state::__execute", assoc_execute);
    import_set_address("_ZTVNSt3__117__assoc_sub_stateE", g_vt_assoc);
    import_set_address("_ZTINSt3__117__assoc_sub_stateE", g_ti_assoc);

    /* facets: [sdk] vtable orders of ctype<char> and numpunct<char> */
    static const struct { const char *name; ImportFn fn; } ct[] = {
        {"std::ctype<char>::~ [D1]", facet_dtor}, {"std::ctype<char>::~ [D0]", facet_dtor}, {"std::locale::facet::__on_zero_shared", facet_nop},
        {"std::ctype<char>::do_toupper(char)", ct_toupper}, {"std::ctype<char>::do_toupper(range)", ct_toupper_range},
        {"std::ctype<char>::do_tolower(char)", ct_tolower}, {"std::ctype<char>::do_tolower(range)", ct_tolower_range},
        {"std::ctype<char>::do_widen(char)", ct_widen}, {"std::ctype<char>::do_widen(range)", ct_widen_range},
        {"std::ctype<char>::do_narrow(char)", ct_narrow}, {"std::ctype<char>::do_narrow(range)", ct_narrow_range},
    }, np[] = {
        {"std::numpunct<char>::~ [D1]", facet_dtor}, {"std::numpunct<char>::~ [D0]", facet_dtor}, {"std::locale::facet::__on_zero_shared", facet_nop},
        {"std::numpunct<char>::do_decimal_point", np_decimal_point}, {"std::numpunct<char>::do_thousands_sep", np_thousands_sep},
        {"std::numpunct<char>::do_grouping", np_grouping}, {"std::numpunct<char>::do_truename", np_truename},
        {"std::numpunct<char>::do_falsename", np_falsename},
    };
    g_ti_ctype[0] = vt_class; g_ti_ctype[1] = P64("NSt3__15ctypeIcEE");
    g_vt_ctype[0] = 0; g_vt_ctype[1] = P64(g_ti_ctype);
    for (int i = 0; i < 11; i++) g_vt_ctype[2 + i] = hostfn_address(ct[i].name, ct[i].fn);
    g_ctype[0] = P64(&g_vt_ctype[2]); g_ctype[1] = 0x7fffffff; g_ctype[2] = P64(classic_table); g_ctype[3] = 0;
    g_ti_numpunct[0] = vt_class; g_ti_numpunct[1] = P64("NSt3__18numpunctIcEE");
    g_vt_numpunct[0] = 0; g_vt_numpunct[1] = P64(g_ti_numpunct);
    for (int i = 0; i < 8; i++) g_vt_numpunct[2 + i] = hostfn_address(np[i].name, np[i].fn);
    g_numpunct[0] = P64(&g_vt_numpunct[2]); g_numpunct[1] = 0x7fffffff; g_numpunct[2] = 0x2c2e;        /* [sdk] '.' at +0x10, ',' at +0x11 */
    import_set_address("_ZNSt3__15ctypeIcE2idE", g_id_ctype);
    import_set_address("_ZNSt3__18numpunctIcE2idE", g_id_numpunct);
    import_set_address("_ZTVNSt3__15ctypeIcEE", g_vt_ctype);
    import_set_address("_ZTVNSt3__18numpunctIcEE", g_vt_numpunct);

    /* streams */
    g_ti_ostream[0] = vt_class; g_ti_ostream[1] = P64("NSt3__113basic_ostreamIcNS_11char_traitsIcEEEE");
    g_ti_istream[0] = vt_class; g_ti_istream[1] = P64("NSt3__113basic_istreamIcNS_11char_traitsIcEEEE");
    make_stream_vtable(g_vt_ostream, g_ti_ostream, "basic_ostream", 8);
    make_stream_vtable(g_vt_istream, g_ti_istream, "basic_istream", 0x10);
    import_set_address("_ZTVNSt3__113basic_ostreamIcNS_11char_traitsIcEEEE", g_vt_ostream);
    import_set_address("_ZTVNSt3__113basic_istreamIcNS_11char_traitsIcEEEE", g_vt_istream);
    (void)g_classic_impl;
}

#define OSTR "_ZNSt3__113basic_ostreamIcNS_11char_traitsIcEEE"
#define ISTR "_ZNSt3__113basic_istreamIcNS_11char_traitsIcEEE"
/* Replace earlier registrations: use_facet used to trap; the deleting destructor did not free the object. */
static const ImportDef cxxlib_replace[] = {
    {"_ZNKSt3__16locale9use_facetERNS0_2idE", locale_use_facet},
    {OSTR "D0Ev", ostream_d0}, {ISTR "D0Ev", istream_d0}, {OSTR "D1Ev", ostream_d1}, {ISTR "D1Ev", istream_d1},
    {"_ZNSt3__19basic_iosIcNS_11char_traitsIcEEED2Ev", ios_dtor},
    {"_ZNSt3__115basic_streambufIcNS_11char_traitsIcEEED2Ev", sbuf_dtor},
    {OSTR "6sentryD1Ev", sentry_dtor},
};
static const ImportDef cxxlib_fill[] = {
    {"_ZTv0_n24_" "NSt3__113basic_ostreamIcNS_11char_traitsIcEEED1Ev", stream_thunk_dtor},
    {"_ZTv0_n24_" "NSt3__113basic_ostreamIcNS_11char_traitsIcEEED0Ev", stream_thunk_dtor_delete},
    {"_ZTv0_n24_" "NSt3__113basic_istreamIcNS_11char_traitsIcEEED1Ev", stream_thunk_dtor},
    {"_ZTv0_n24_" "NSt3__113basic_istreamIcNS_11char_traitsIcEEED0Ev", stream_thunk_dtor_delete},
};

/* test access (cxxrt_selftest.h) */
uint64_t cxxlib_test_object(int which) { return which == 0 ? P64(g_id_ctype) : which == 1 ? P64(g_id_numpunct) : which == 2 ? P64(&g_vt_ostream[3]) : P64(g_future_category); }

void register_cxxlib_imports(void) {
    register_imports(cxxlib_replace, (int)(sizeof cxxlib_replace / sizeof cxxlib_replace[0]));
    for (int i = 0; i < (int)(sizeof cxxlib_fill / sizeof cxxlib_fill[0]); i++)
        if (!import_lookup(cxxlib_fill[i].name)) register_imports(&cxxlib_fill[i], 1);
}
