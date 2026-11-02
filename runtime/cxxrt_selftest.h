/* Self-test of the C / C++ runtime layer, included at the end of nncxxrt.c.
 *   WT_CXXRT_SELFTEST=1  tests that need no game state (the image is mapped and relocated, nothing has run)
 * Exit code = number of failed checks. */
uint64_t cxxlib_test_object(int which);

static int st_fail, st_checks;
#define CHECK(cond, ...) do { st_checks++; if (!(cond)) { st_fail++; printf("  FAILED: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

static Ctx *st_ctx(void) {
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    memset(c, 0, sizeof *c);
    uint8_t *stack = VirtualAlloc(NULL, 4 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    c->x[31] = P64(stack) + (4 << 20) - 0x1000;
    c->tpidr = P64(VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return c;
}
static uint64_t st_import(Ctx *c, const char *name, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) {
    ImportFn fn = import_lookup(name);
    if (!fn) { st_fail++; printf("  FAILED: no handler for %s\n", name); return 0; }
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3;
    fn(c);
    return c->x[0];
}


/* ---- sscanf / printf through the AArch64 variadic reader */
static int st_scan(Ctx *c, const char *in, const char *fmt, int n, ...) {
    va_list ap;
    va_start(ap, n);
    uint64_t sp = c->x[31];
    for (int i = 0; i < n; i++) {
        uint64_t p = P64(va_arg(ap, void *));
        if (i < 6) c->x[2 + i] = p; else wr64(sp + 8ull * (uint64_t)(i - 6), p);
    }
    va_end(ap);
    c->x[0] = P64(in); c->x[1] = P64(fmt);
    ImportFn fn = import_lookup("sscanf");
    if (!fn) { st_fail++; printf("  FAILED: sscanf has no handler\n"); return -99; }
    fn(c);
    return (int)(int32_t)c->x[0];
}
static void st_scanf(void) {
    Ctx *c = st_ctx();
    float f[3] = {0}; double d = 0; int i[8] = {0}; char s1[64] = "", s2[64] = "", s3[64] = "", ch = 0; long long ll = 0; unsigned long long ull = 0;
    uint32_t bits; uint64_t bits64, q[2];
    /* formats the game uses (found by scanning the sscanf call sites of main) */
    CHECK(st_scan(c, "1.5,-2.25,3e2", "%f,%f,%f", 3, &f[0], &f[1], &f[2]) == 3 && f[0] == 1.5f && f[1] == -2.25f && f[2] == 300.0f, "%%f,%%f,%%f");
    CHECK(st_scan(c, "0.1, 7", "%f,%f,%f", 3, &f[0], &f[1], &f[2]) == 2 && f[1] == 7.0f, "%%f,%%f,%%f with two fields must return 2");
    CHECK(st_scan(c, "key = value;rest", "%1023[^;\r\n=] =%4999[^;\r\n]", 2, s1, s2) == 2 && !strcmp(s1, "key ") && !strcmp(s2, " value"), "config line: '%s' '%s'", s1, s2);
    CHECK(st_scan(c, "HTTP/1.1 200 OK", "HTTP/1.%d %d", 2, &i[0], &i[1]) == 2 && i[0] == 1 && i[1] == 200, "HTTP/1.%%d %%d");
    CHECK(st_scan(c, "HTTP/1.1 404 Not Found", " HTTP/%1d.%1d%c%3d", 4, &i[0], &i[1], &ch, &i[2]) == 4 && i[0] == 1 && i[1] == 1 && ch == ' ' && i[2] == 404, "status line");
    CHECK(st_scan(c, "HTTP/2 301", " HTTP/2 %d", 1, &i[0]) == 1 && i[0] == 301, "HTTP/2 %%d");
    CHECK(st_scan(c, "RTSP/1.0 200", " RTSP/%d.%d %3d", 3, &i[0], &i[1], &i[2]) == 3 && i[2] == 200, "RTSP");
    CHECK(st_scan(c, "12:34:56 GMT", "%02d:%02d:%02d%n", 4, &i[0], &i[1], &i[2], &i[3]) == 3 && i[0] == 12 && i[2] == 56 && i[3] == 8, "time with %%n (n=%d)", i[3]);
    CHECK(st_scan(c, "12:34", "%02d:%02d%n", 3, &i[0], &i[1], &i[3]) == 2 && i[3] == 5, "hh:mm with %%n");
    CHECK(st_scan(c, "https://example.org/x", "%15[^?&/:]://%c", 2, s1, &ch) == 2 && !strcmp(s1, "https") && ch == 'e', "scheme");
    CHECK(st_scan(c, "http://host.name:80/path?q", "%15[^\n:]://%[^\n/?:]%[^\n]", 3, s1, s2, s3) == 3 && !strcmp(s2, "host.name") && !strcmp(s3, ":80/path?q"), "url split '%s' '%s'", s2, s3);
    CHECK(st_scan(c, "host/path", "%[^\n/?:]%[^\n]", 2, s1, s2) == 2 && !strcmp(s1, "host") && !strcmp(s2, "/path"), "host/path");
    CHECK(st_scan(c, "file:///c/dir", "%15[^\n/:]:%3[/]%[^\n/?#]%[^\n]", 4, s1, s2, s3, s3 + 32) == 4 && !strcmp(s2, "///") && !strcmp(s3, "c"), "slashes '%s' '%s'", s2, s3);
    CHECK(st_scan(c, "[fe80::1]:80", "[%*45[0123456789abcdefABCDEF:.]%c", 1, &ch) == 1 && ch == ']', "bracketed IPv6 with suppression");
    CHECK(st_scan(c, "3.25", "%f", 1, &f[0]) == 1 && f[0] == 3.25f, "%%f");
    /* general conformance */
    CHECK(st_scan(c, "0x1Ag -077 0", "%i%c %i %i", 4, &i[0], &ch, &i[1], &i[2]) == 4 && i[0] == 26 && ch == 'g' && i[1] == -63 && i[2] == 0, "%%i bases: %d %d", i[0], i[1]);
    CHECK(st_scan(c, "ff 0XFF 777", "%x %X %o", 3, &i[0], &i[1], &i[2]) == 3 && i[0] == 255 && i[1] == 255 && i[2] == 511, "%%x %%X %%o");
    i[0] = -1; CHECK(st_scan(c, "300", "%hhd", 1, &i[0]) == 1 && i[0] == (int)0xffffff2c, "%%hhd stores one byte (%08x)", (unsigned)i[0]);
    i[0] = -1; CHECK(st_scan(c, "70000", "%hd", 1, &i[0]) == 1 && i[0] == (int)0xffff1170, "%%hd stores two bytes (%08x)", (unsigned)i[0]);
    CHECK(st_scan(c, "-9223372036854775808 18446744073709551615", "%lld %llu", 2, &ll, &ull) == 2 && ll == (-9223372036854775807ll - 1) && ull == ~0ull, "64-bit integers");
    CHECK(st_scan(c, "1 2 3 4 5 6 7 8", "%d %d %d %d %d %d %d %d", 8, &i[0], &i[1], &i[2], &i[3], &i[4], &i[5], &i[6], &i[7]) == 8 && i[5] == 6 && i[6] == 7 && i[7] == 8, "eight arguments (two on the stack)");
    CHECK(st_scan(c, "", "%d", 1, &i[0]) == -1, "empty input must return EOF");
    CHECK(st_scan(c, "   ", "%d", 1, &i[0]) == -1, "blank input must return EOF");
    CHECK(st_scan(c, "abc", "%d", 1, &i[0]) == 0, "matching failure must return 0");
    CHECK(st_scan(c, "5 x", "%d %d", 2, &i[0], &i[1]) == 1, "second conversion fails -> 1");
    CHECK(st_scan(c, "abcdefgh", "%5s%n", 2, s1, &i[0]) == 1 && !strcmp(s1, "abcde") && i[0] == 5, "%%5s");
    memset(s1, '#', 8); CHECK(st_scan(c, "xyzw", "%3c", 1, s1) == 1 && !memcmp(s1, "xyz#", 4), "%%3c writes no terminator");
    CHECK(st_scan(c, "50% off", "%d%% %s", 2, &i[0], s1) == 2 && i[0] == 50 && !strcmp(s1, "off"), "%%%%");
    CHECK(st_scan(c, "a-c]x", "%[]a-c-]", 1, s1) == 1 && !strcmp(s1, "a-c]"), "set with ] and - ('%s')", s1);
    CHECK(st_scan(c, "12345", "%2d%*2d%d", 2, &i[0], &i[1]) == 2 && i[0] == 12 && i[1] == 5, "widths and suppression");
    /* floating point: correctly rounded, single precision rounded once */
    CHECK(st_scan(c, "0.1", "%f", 1, &f[0]) == 1 && (memcpy(&bits, &f[0], 4), bits) == 0x3dcccccdu, "0.1f = %08x", bits);
    CHECK(st_scan(c, "16777217", "%f", 1, &f[0]) == 1 && f[0] == 16777216.0f, "16777217 -> float");
    CHECK(st_scan(c, "0.1 1e308 4.9e-324", "%lf %le %lg", 3, &d, &bits64, &q[0]) == 3 && (memcpy(&bits64, &d, 8), bits64) == 0x3fb999999999999aull && q[0] == 1, "doubles");
    CHECK(st_scan(c, "0x1.8p1 -inf nan", "%f %f %f", 3, &f[0], &f[1], &f[2]) == 3 && f[0] == 3.0f && f[1] < 0 && f[1] * 0 != 0 && f[2] != f[2], "hex float, inf, nan");
    CHECK(st_scan(c, "1.e5x", "%f%c", 2, &f[0], &ch) == 2 && f[0] == 1e5f && ch == 'x', "1.e5");
    CHECK(st_scan(c, "2.5e+z", "%f%c", 2, &f[0], &ch) == 2 && f[0] == 2.5f && ch == 'e', "an incomplete exponent is not consumed");
    CHECK(st_scan(c, "1.5", "%Lf", 1, q) == 1 && q[1] == 0x3fff800000000000ull && q[0] == 0, "%%Lf stores binary128");

    /* printf family through the same reader */
    char out[256];
    ImportFn sn = import_lookup("snprintf");
    c->x[0] = P64(out); c->x[1] = sizeof out; c->x[2] = P64("%d|%5s|%-4x|%c|%lld|%hhd|%05.1f|%s");
    c->x[3] = (uint64_t)-42; c->x[4] = P64("ab"); c->x[5] = 0xbeef; c->x[6] = 'Z'; c->x[7] = 0x123456789all;
    wr64(c->x[31], 300); wr64(c->x[31] + 8, P64("tail"));
    c->v[0].df[0] = 2.25;
    sn(c);
    CHECK(!strcmp(out, "-42|   ab|beef|Z|78187493530|44|002.2|tail") || !strcmp(out, "-42|   ab|beef|Z|78187493530|44|002.3|tail"), "snprintf: %s", out);
    c->x[0] = P64(out); c->x[1] = sizeof out; c->x[2] = P64("%p %Lf %.3Lg");
    c->x[3] = 0x7100001000ull;
    c->v[0].d[0] = 0; c->v[0].d[1] = 0x3fff800000000000ull;            /* 1.5L */
    c->v[1].d[0] = 0x5555555555555555ull; c->v[1].d[1] = 0x3ffd555555555555ull;   /* 1/3 */
    sn(c);
    CHECK(!strcmp(out, "0x0000007100001000 1.500000 0.333"), "snprintf %%p %%Lf: %s", out);
    c->x[0] = P64(out); c->x[1] = 8; c->x[2] = P64("%s"); c->x[3] = P64("0123456789");
    sn(c);
    CHECK((int)c->x[0] == 10 && !strcmp(out, "0123456"), "snprintf truncation: %d '%s'", (int)c->x[0], out);
}


/* ---- guard variables under contention */
#define ST_GUARDS 400
static uint64_t st_guards[ST_GUARDS];
static volatile LONG st_guard_inits[ST_GUARDS];
static HANDLE st_go;
static DWORD WINAPI st_guard_thread(LPVOID arg) {
    Ctx *c = st_ctx();
    (void)arg;
    WaitForSingleObject(st_go, INFINITE);
    for (int g = 0; g < ST_GUARDS; g++) {
        if (rd8(P64(&st_guards[g]))) continue;                         /* the inline fast path of the compiler */
        if ((int)st_import(c, "__cxa_guard_acquire", P64(&st_guards[g]), 0, 0, 0)) {
            LONG seen = st_guard_inits[g];
            if ((g & 7) == 0) Sleep(0);
            st_guard_inits[g] = seen + 1;                              /* a deliberately racy "constructor" */
            st_import(c, (g % 5 == 4 && seen == 0) ? "__cxa_guard_abort" : "__cxa_guard_release", P64(&st_guards[g]), 0, 0, 0);
        }
    }
    return 0;
}
static void st_guard(void) {
    HANDLE th[12];
    st_go = CreateEventA(NULL, TRUE, FALSE, NULL);
    for (int i = 0; i < 12; i++) th[i] = CreateThread(NULL, 0, st_guard_thread, NULL, 0, NULL);
    SetEvent(st_go);
    WaitForMultipleObjects(12, th, TRUE, INFINITE);
    int bad = 0;
    for (int g = 0; g < ST_GUARDS; g++) {
        /* every fifth guard's first initialisation is aborted, so it must have run exactly twice; the others once */
        int want = g % 5 == 4 ? 2 : 1;
        if (st_guard_inits[g] != want || (st_guards[g] & 0xffff) != 1) bad++;
    }
    CHECK(bad == 0, "%d of %d guard variables were initialised a wrong number of times under 12 threads", bad, ST_GUARDS);
}

/* ---- exception objects and the imported vtables */
static void st_exceptions(void) {
    Ctx *c = st_ctx();
    uint64_t obj[4] = {0}, copy[4] = {0};
    st_import(c, "_ZNSt11logic_errorC2EPKc", P64(obj), P64("index out of range"), 0, 0);
    obj[0] = P64(&g_vt_out_of_range[2]);                               /* what the game's inlined out_of_range constructor stores */
    const char *w = (const char *)(uintptr_t)cxxrt_call(c, rd64(obj[0] + 0x10), P64(obj), 0, 0);
    CHECK(w && !strcmp(w, "index out of range"), "out_of_range::what() through the imported vtable");
    CHECK(rd64(obj[0] - 8) == P64(g_ti_out_of_range) && rd64(obj[0] - 16) == 0, "vtable header (offset-to-top, typeinfo)");
    st_import(c, "_ZNSt11logic_errorC2ERKS_", P64(copy), P64(obj), 0, 0);
    CHECK(copy[1] == obj[1] && rd32(obj[1] - 8) == 1, "logic_error copy shares the reference-counted string");
    cxxrt_call(c, rd64(obj[0]), P64(obj), 0, 0);                       /* virtual ~D1 */
    CHECK(rd32(copy[1] - 8) == 0 && !strcmp((const char *)(uintptr_t)copy[1], "index out of range"), "string survives the first destructor");
    st_import(c, "_ZNSt12out_of_rangeD1Ev", P64(copy), 0, 0, 0);
    CHECK(ti_upcast(P64(g_ti_out_of_range), 0x1000, P64(g_ti_exception)) == 0x1000 && ti_upcast(P64(g_ti_bad_alloc), 0x1000, P64(g_ti_logic_error)) == 0, "typeinfo hierarchy walk");
    uint64_t e = st_import(c, "__cxa_allocate_exception", 24, 0, 0, 0);
    CHECK(e && (e & 15) == 0 && rd64(e) == 0 && rd64(e + 16) == 0, "__cxa_allocate_exception returns zeroed, aligned memory");
}

/* ---- facets, stream vtables */
static void st_facets(void) {
    Ctx *c = st_ctx();
    uint64_t loc = 0;
    uint64_t ct = st_import(c, "_ZNKSt3__16locale9use_facetERNS0_2idE", P64(&loc), cxxlib_test_object(0), 0, 0);
    CHECK(ct && (uint8_t)cxxrt_call(c, rd64(rd64(ct) + 0x38), ct, 'q', 0) == 'q', "ctype<char>::do_widen through the vtable");
    CHECK((uint8_t)cxxrt_call(c, rd64(rd64(ct) + 0x18), ct, 'q', 0) == 'Q' && (uint8_t)cxxrt_call(c, rd64(rd64(ct) + 0x28), ct, 'Q', 0) == 'q', "do_toupper / do_tolower");
    CHECK((uint8_t)cxxrt_call(c, rd64(rd64(ct) + 0x48), ct, 0xe9, '?') == '?', "do_narrow of a non-ASCII character gives the default");
    const uint16_t *tab = (const uint16_t *)(uintptr_t)rd64(ct + 0x10);
    CHECK((tab['A'] & 0x100) && (tab['a'] & 0x200) && (tab['5'] & 0x800) && (tab[' '] & 0x2000) && !(tab['a'] & 0x100), "classic table mask bits");
    uint64_t np = st_import(c, "_ZNKSt3__16locale9use_facetERNS0_2idE", P64(&loc), cxxlib_test_object(1), 0, 0);
    uint8_t str[24];
    CHECK(np && (uint8_t)cxxrt_call(c, rd64(rd64(np) + 0x18), np, 0, 0) == '.' && (uint8_t)cxxrt_call(c, rd64(rd64(np) + 0x20), np, 0, 0) == ',', "numpunct<char> decimal point / thousands separator");
    c->x[8] = P64(str);
    cxxrt_call(c, rd64(rd64(np) + 0x30), np, 0, 0);
    CHECK(str[0] == 8 && !memcmp(str + 1, "true", 5), "numpunct<char>::do_truename returns a short std::string");
    /* a plain std::ostream as the game lays it out: primary vptr at +0, basic_ios part at +8 */
    uint64_t vt = cxxlib_test_object(2), os[32] = {0};
    os[0] = vt; os[1] = vt + 0x28;
    CHECK(rd64(vt - 0x18) == 8 && rd64(vt - 0x10) == 0 && rd64(os[1] - 0x18) == (uint64_t)-8, "basic_ostream vtable: vbase offset 8, vcall offset -8");
    CHECK(cxxrt_call(c, rd64(os[1]), P64(&os[1]), 0, 0) == P64(os), "virtual thunk to ~basic_ostream adjusts this by -8");
    CHECK(P64(os) + rd64(rd64(P64(os)) - 0x18) == P64(&os[1]), "the stream finds its basic_ios through vtable[-3]");
}

/* ---- promise<void> / future<void> */
static uint64_t st_promise[1];

/* ---- __dynamic_cast on the game's own RTTI
 * Typeinfo objects and vtables are found in the relocated image: a typeinfo starts with one of the three imported
 * __cxxabiv1 table addresses; a vtable holds { offset-to-top, typeinfo } before its address point. */
#include "image_layout.h"
#define ST_DATA_START (GUEST_BASE + GUEST_RO_END)          /* main's data segment, where its typeinfo objects and vtables are */
#define ST_DATA_END (GUEST_BASE + FILE_IMAGE)
static uint64_t st_find_vptr(uint64_t ti, int64_t offset_to_top) {
    for (uint64_t a = ST_DATA_START + 8; a < ST_DATA_END - 16; a += 8)
        if (rd64(a) == ti && (int64_t)rd64(a - 8) == offset_to_top && rd64(a + 8) >= GUEST_BASE && rd64(a + 8) < GUEST_BASE + GUEST_RO_END) return a + 8;
    return 0;
}
static void st_dyncast(void) {
    int si_done = 0, vmi_done = 0, n_class = 0, n_si = 0, n_vmi = 0;
    uint64_t other = 0;
    for (uint64_t a = ST_DATA_START; a < ST_DATA_END - 64; a += 8) {
        int kind = rd64(a) == P64(&g_vt_class[2]) ? TI_CLASS : rd64(a) == P64(&g_vt_si[2]) ? TI_SI : rd64(a) == P64(&g_vt_vmi[2]) ? TI_VMI : 0;
        if (!kind || !cxxrt_readable(rd64(a + 8) & 0x7fffffffffffffffull, 4)) continue;
        if (kind == TI_CLASS) { n_class++; if (!other && st_find_vptr(a, 0)) other = a; }
        if (kind == TI_SI) n_si++;
        if (kind == TI_VMI) n_vmi++;
        if (kind == TI_SI && !si_done && other) {
            /* D : B : ... two levels, both with vtables */
            uint64_t d = a, b = rd64(a + 16);
            if (ti_kind(b) != TI_SI) continue;
            uint64_t root = rd64(b + 16), vd = st_find_vptr(d, 0), vb = st_find_vptr(b, 0);
            if (!vd || !vb || ti_same(root, other) || ti_same(b, other)) continue;
            uint64_t obj_d[2] = {vd, 0}, obj_b[2] = {vb, 0};
            printf("  single inheritance: %s : %s : %s (unrelated: %s)\n", cxxrt_type_name(d), cxxrt_type_name(b), cxxrt_type_name(root), cxxrt_type_name(other));
            CHECK(cxxrt_dynamic_cast(P64(obj_d), root, d) == P64(obj_d), "root -> most derived");
            CHECK(cxxrt_dynamic_cast(P64(obj_d), root, b) == P64(obj_d), "root -> middle class");
            CHECK(cxxrt_dynamic_cast(P64(obj_d), b, d) == P64(obj_d), "middle -> most derived");
            CHECK(cxxrt_dynamic_cast(P64(obj_b), root, d) == 0, "an object of the middle class is not the derived class");
            CHECK(cxxrt_dynamic_cast(P64(obj_d), root, other) == 0, "cast to an unrelated class fails");
            si_done = 1;
        }
        if (kind == TI_VMI && !vmi_done) {
            uint32_t count = rd32(a + 20);
            if (count < 2) continue;
            int64_t of0 = (int64_t)rd64(a + 32), of1 = (int64_t)rd64(a + 48);
            uint64_t b0 = rd64(a + 24), b1 = rd64(a + 40);
            if ((of0 & 1) || (of1 & 1) || (of0 >> 8) != 0 || (of1 >> 8) <= 0 || (of1 >> 8) > 0x400 || ti_same(b0, b1)) continue;
            uint64_t off = (uint64_t)(of1 >> 8), v0 = st_find_vptr(a, 0), v1 = st_find_vptr(a, -(int64_t)off);
            if (!v0 || !v1 || ti_kind(b0) == TI_OTHER || ti_kind(b1) == TI_OTHER) continue;
            uint64_t *obj = calloc(1, off + 64);
            obj[0] = v0;
            wr64(P64(obj) + off, v1);
            printf("  multiple inheritance: %s : %s (+0), %s (+0x%llx)\n", cxxrt_type_name(a), cxxrt_type_name(b0), cxxrt_type_name(b1), (unsigned long long)off);
            CHECK(cxxrt_dynamic_cast(P64(obj) + off, b1, a) == P64(obj), "second base -> derived subtracts the base offset");
            CHECK(cxxrt_dynamic_cast(P64(obj), b0, a) == P64(obj), "first base -> derived");
            CHECK(cxxrt_dynamic_cast(P64(obj), b0, b1) == P64(obj) + off, "cross-cast first base -> second base");
            CHECK(cxxrt_dynamic_cast(P64(obj) + off, b1, b0) == P64(obj), "cross-cast second base -> first base");
            if (other && !ti_upcast(a, 0x1000, other)) CHECK(cxxrt_dynamic_cast(P64(obj) + off, b1, other) == 0, "cast to an unrelated class fails");
            free(obj);
            vmi_done = 1;
        }
    }
    printf("  game RTTI seen: %d root classes, %d single-inheritance, %d multiple/virtual-inheritance typeinfo objects\n", n_class, n_si, n_vmi);
    CHECK(si_done && vmi_done, "could not find suitable classes in the game's RTTI (single %d, multiple %d)", si_done, vmi_done);
    CHECK(n_si > 0, "the imported __cxxabiv1 typeinfo vtables are not what the game's typeinfo objects point to");
}


/* cxxrt_readable's record of game-managed memory: which changes of guest memory it survives, and which not. */
static void st_readable(void) {      // PR #2 (jfb112697): 11 checks of the ranged record (skipped under WT_NO_READABLE_CACHE / WT_READABLE_FORGET_ALL)
    const uint64_t pg = 0x1000;
    uint8_t *mem = VirtualAlloc(NULL, 64 * pg, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), *other = VirtualAlloc(NULL, 4 * pg, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    uint64_t m = P64(mem), p = m + 16 * pg + 0x80, q = m + 30 * pg + 0x10;
    if (!mem || !other) { CHECK(0, "VirtualAlloc"); return; }
    cxxrt_guest_region(m, 64 * pg); cxxrt_guest_region(P64(other), 4 * pg);
    LONG64 asked = g_rd_asked, kept = g_rd_kept;
    CHECK(cxxrt_readable(p, 8) && g_rd_asked == asked + 1, "the first question about a run goes to the OS");
    if (g_rd_nocache || g_rd_forget_all) { printf("  (WT_NO_READABLE_CACHE / WT_READABLE_FORGET_ALL is set: the record is not tested)\n"); return; }
    CHECK(cxxrt_readable(p + 0x100, 8) && g_rd_asked == asked + 1, "the next one in the same run is answered from the record");
    cxxrt_mem_changed(P64(other), pg); VirtualFree(other, pg, MEM_DECOMMIT); cxxrt_mem_changed(P64(other), pg);
    CHECK(cxxrt_readable(p, 8) && g_rd_asked == asked + 1 && g_rd_kept == kept + 1, "pages freed in another region: the record stays");
    cxxrt_mem_changed(m + 40 * pg + 0x10, 8 * pg - 0x20); VirtualFree(mem + 40 * pg, 8 * pg, MEM_DECOMMIT); cxxrt_mem_changed(m + 40 * pg + 0x10, 8 * pg - 0x20);
    CHECK(cxxrt_readable(p, 8) && g_rd_asked == asked + 1, "pages freed higher up in the same run: the record stays for what is below them");
    CHECK(!cxxrt_readable(m + 41 * pg, 8) && !cxxrt_readable(m + 40 * pg, 1) && g_rd_asked == asked + 3, "the freed pages are not readable (whole pages, whatever the range that was named)");
    CHECK(cxxrt_readable(m + 40 * pg - 8, 8) && !cxxrt_readable(m + 40 * pg - 8, 9) && g_rd_asked == asked + 4, "the record ends where the freed pages begin");
    cxxrt_mem_changed(m + 20 * pg, 3 * pg); VirtualFree(mem + 20 * pg, 3 * pg, MEM_DECOMMIT); cxxrt_mem_changed(m + 20 * pg, 3 * pg);
    CHECK(cxxrt_readable(q, 8) && g_rd_asked == asked + 4, "pages freed lower down in the same run: the record stays for what is above them");
    CHECK(!cxxrt_readable(m + 21 * pg, 8) && cxxrt_readable(p, 8) && g_rd_asked == asked + 6, "and the part below them is asked about again");
    cxxrt_mem_changed(m + 30 * pg, pg); VirtualFree(mem + 30 * pg, pg, MEM_DECOMMIT); cxxrt_mem_changed(m + 30 * pg, pg);
    CHECK(!cxxrt_readable(q, 8) && g_rd_asked == asked + 7, "the page that was asked about is freed: not readable");
    for (int i = 0; i <= RD_CHANGES; i++) cxxrt_mem_changed(P64(other), pg);
    CHECK(cxxrt_readable(p, 8) && g_rd_asked == asked + 8, "more changes than are kept: the record is forgotten and the OS is asked");
    cxxrt_mem_changed(m, 0); VirtualFree(mem, 0, MEM_RELEASE); cxxrt_mem_changed(m, 0);
    CHECK(!cxxrt_readable(p, 8) && g_rd_asked == asked + 9, "the region is released: not readable");
    VirtualFree(other, 0, MEM_RELEASE);
}

static DWORD WINAPI st_main(LPVOID arg) {
    (void)arg;
    printf("\n=== C/C++ runtime self-test\n");
    printf("sscanf / printf\n"); st_scanf();
    printf("guard variables\n"); st_guard();
    printf("record of readable game-managed memory\n"); st_readable();      // PR #2 (jfb112697)
    printf("exception classes and vtables\n"); st_exceptions();
    printf("locale facets and stream vtables\n"); st_facets();
    printf("__dynamic_cast on the game's RTTI\n"); st_dyncast();
    printf("=== self-test: %d checks, %d failed\n", st_checks, st_fail);
    fflush(stdout);
    return 0;
}
static void cxxrt_selftest(void) {
    HANDLE t = CreateThread(NULL, (SIZE_T)1 << 30, st_main, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    WaitForSingleObject(t, INFINITE);
    ExitProcess((UINT)st_fail);
}
