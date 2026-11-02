/* Entry point of the SDK families added on 2026-10-08, and their host-side self-test.
 *
 *   sdklift_rt.c / sdklift_gen.c   code lifted out of the real SDK module (pure functions, exact behaviour)
 *   nnmii.c      nn::mii Database (service side driven by lifted SDK code), AppendMii
 *   nnnet.c      nn::ssl C API, nn::socket leftovers, nn::bcat, nn::friends, nn::account, nn::ldn, nn::ec accessor, glslc
 *   nnfsdir.c    nn::fs directory functions
 *
 * WT_SDK_SELFTEST=1: run the tests below right after registration and exit (exit code = failures).
 * WT_DUMP_UNREG2=<file>: names of SDK imports (nn..., glslc...) still without a handler after this
 * file's registrations. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "sdklift.h"

void register_nnmii_imports(void);
void register_nnfsdir_imports(void);
int nnmii_selftest(Ctx *c);
int nnfsdir_selftest(Ctx *c);
int nnfs_selftest(Ctx *c);
int bcat_selftest(Ctx *c);
int nnhid_selftest(Ctx *c);
int nvn_sdk_selftest(Ctx *c);
int nntime_selftest(Ctx *c);
void nnnet_register_final(void);

#define P(v) ((uint64_t)(uintptr_t)(v))

static ImportFn handler(const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) { printf("  no handler registered for %s\n", name); report_and_exit(3); }
    return f;
}



/* WT_DUMP_UNREG2 */
static void dump_unregistered(void) {
    const char *path = getenv("WT_DUMP_UNREG2");
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < dyn_sym_count; i++) {
        const char *n = dyn_syms[i].name;
        if (dyn_syms[i].defined || !n[0] || import_lookup(n)) continue;
        if (strstr(n, "lua2cpp") || strstr(n, "L2C")) continue;
        if (strncmp(n, "_ZN2nn", 6) && strncmp(n, "_ZNK2nn", 7) && strncmp(n, "nn", 2) && strncmp(n, "glslc", 5)) continue;
        int is_data = 0;
        for (int k = 0; k < sdk_data_count; k++) if (strcmp(sdk_data[k].name, n) == 0) is_data = 1;
        if (!is_data) fprintf(f, "%s\n", n);
    }
    fclose(f);
}

/* ---- C library / libm / small SDK classes that run as lifted sdk.elf code (sdklift_install.h): each is called
 * through the handler the game gets and compared with an independent reference (the host C library, a closed
 * formula, a published test vector). Math: musl and the Microsoft CRT are not bit-identical, so the test only
 * demands agreement within 4 ulp and prints how many results differ in the last bits (that number is what the
 * game no longer sees with the lifted code). */
#include <math.h>
static float callf1(Ctx *c, ImportFn f, float a) { memset(&c->v[0], 0, 16); c->v[0].f[0] = a; f(c); return c->v[0].f[0]; }
static float callf2(Ctx *c, ImportFn f, float a, float b) { memset(&c->v[0], 0, 32); c->v[0].f[0] = a; c->v[1].f[0] = b; f(c); return c->v[0].f[0]; }
static double calld1(Ctx *c, ImportFn f, double a) { memset(&c->v[0], 0, 16); c->v[0].df[0] = a; f(c); return c->v[0].df[0]; }
static double calld2(Ctx *c, ImportFn f, double a, double b) { memset(&c->v[0], 0, 32); c->v[0].df[0] = a; c->v[1].df[0] = b; f(c); return c->v[0].df[0]; }
static long long ulp_f(float a, float b) {
    int32_t x, y;
    if (a != a && b != b) return 0;
    if (a != a || b != b) return 1 << 30;
    memcpy(&x, &a, 4); memcpy(&y, &b, 4);
    if (x < 0) x = (int32_t)0x80000000 - x;
    if (y < 0) y = (int32_t)0x80000000 - y;
    return llabs((long long)x - (long long)y);
}
static long long ulp_d(double a, double b) {
    int64_t x, y;
    if (a != a && b != b) return 0;
    if (a != a || b != b) return 1 << 30;
    memcpy(&x, &a, 8); memcpy(&y, &b, 8);
    if (x < 0) x = (int64_t)0x8000000000000000ull - x;
    if (y < 0) y = (int64_t)0x8000000000000000ull - y;
    long long d = (long long)(x - y);
    return d < 0 ? -d : d;
}
static void selftest_cmp_int(Ctx *c) { int a = (int)rd32(c->x[0]), b = (int)rd32(c->x[1]); c->x[0] = (uint64_t)(int64_t)((a > b) - (a < b)); }
static int libc_selftest(Ctx *c) {
    int bad = 0;
    static const struct { const char *name; float (*ref)(float); float lo, hi; } f1[] = {
        {"sinf", sinf, -20, 20}, {"cosf", cosf, -20, 20}, {"tanf", tanf, -1.5f, 1.5f}, {"asinf", asinf, -1, 1}, {"acosf", acosf, -1, 1},
        {"atanf", atanf, -50, 50}, {"expf", expf, -20, 20}, {"exp2f", exp2f, -30, 30}, {"logf", logf, 1e-6f, 1e6f}, {"log2f", log2f, 1e-6f, 1e6f},
        {"log10f", log10f, 1e-6f, 1e6f}, {"sqrtf", sqrtf, 0, 1e6f},
    };
    static const struct { const char *name; float (*ref)(float, float); float lo, hi; } f2[] = {
        {"atan2f", atan2f, -10, 10}, {"powf", powf, 0.01f, 12}, {"fmodf", fmodf, -100, 100},
    };
    static const struct { const char *name; double (*ref)(double); double lo, hi; } d1[] = { {"sin", sin, -20, 20}, {"exp2", exp2, -30, 30} };
    static const struct { const char *name; double (*ref)(double, double); double lo, hi; } d2[] = { {"pow", pow, 0.01, 12}, {"fmod", fmod, -100, 100} };
    enum { N = 4000 };
    for (unsigned k = 0; k < sizeof f1 / sizeof f1[0]; k++) {
        ImportFn f = handler(f1[k].name);
        long long worst = 0; int differ = 0;
        for (int i = 0; i <= N; i++) {
            float x = f1[k].lo + (f1[k].hi - f1[k].lo) * (float)i / N;
            long long u = ulp_f(callf1(c, f, x), f1[k].ref(x));
            if (u) differ++;
            if (u > worst) worst = u;
        }
        printf("  libm: %-7s %d of %d results differ from the host CRT in the last bits (largest %lld ulp): %s\n", f1[k].name, differ, N + 1, worst, worst <= 4 ? "ok" : "FAILED");
        bad += worst > 4;
    }
    for (unsigned k = 0; k < sizeof f2 / sizeof f2[0]; k++) {
        ImportFn f = handler(f2[k].name);
        long long worst = 0; int differ = 0, n = 0;
        for (int i = 0; i <= 60; i++) for (int j = 0; j <= 60; j++, n++) {
            float x = f2[k].lo + (f2[k].hi - f2[k].lo) * (float)i / 60, y = f2[k].lo + (f2[k].hi - f2[k].lo) * (float)j / 61 + 0.0131f;
            if (!strcmp(f2[k].name, "powf")) y = (y - 6) * 1.5f;
            long long u = ulp_f(callf2(c, f, x, y), f2[k].ref(x, y));
            if (u) differ++;
            if (u > worst) worst = u;
        }
        printf("  libm: %-7s %d of %d results differ from the host CRT in the last bits (largest %lld ulp): %s\n", f2[k].name, differ, n, worst, worst <= 4 ? "ok" : "FAILED");
        bad += worst > 4;
    }
    for (unsigned k = 0; k < sizeof d1 / sizeof d1[0]; k++) {
        ImportFn f = handler(d1[k].name);
        long long worst = 0; int differ = 0;
        for (int i = 0; i <= N; i++) {
            double x = d1[k].lo + (d1[k].hi - d1[k].lo) * (double)i / N;
            long long u = ulp_d(calld1(c, f, x), d1[k].ref(x));
            if (u) differ++;
            if (u > worst) worst = u;
        }
        printf("  libm: %-7s %d of %d results differ from the host CRT in the last bits (largest %lld ulp): %s\n", d1[k].name, differ, N + 1, worst, worst <= 4 ? "ok" : "FAILED");
        bad += worst > 4;
    }
    for (unsigned k = 0; k < sizeof d2 / sizeof d2[0]; k++) {
        ImportFn f = handler(d2[k].name);
        long long worst = 0; int differ = 0, n = 0;
        for (int i = 0; i <= 60; i++) for (int j = 0; j <= 60; j++, n++) {
            double x = d2[k].lo + (d2[k].hi - d2[k].lo) * (double)i / 60, y = d2[k].lo + (d2[k].hi - d2[k].lo) * (double)j / 61 + 0.0131;
            if (!strcmp(d2[k].name, "pow")) y = (y - 6) * 1.5;
            long long u = ulp_d(calld2(c, f, x, y), d2[k].ref(x, y));
            if (u) differ++;
            if (u > worst) worst = u;
        }
        printf("  libm: %-7s %d of %d results differ from the host CRT in the last bits (largest %lld ulp): %s\n", d2[k].name, differ, n, worst, worst <= 4 ? "ok" : "FAILED");
        bad += worst > 4;
    }
    {   /* strings and memory against the host C library */
        static char a[64] = "the quick brown fox", b[64], m[64];
        int ok = 1;
        c->x[0] = P(a); handler("strlen")(c); ok &= c->x[0] == strlen(a);
        c->x[0] = P(b); c->x[1] = P(a); handler("strcpy")(c); ok &= !strcmp(a, b);
        c->x[0] = P(a); c->x[1] = P("the quick brown fo"); handler("strcmp")(c); ok &= (int)c->x[0] > 0;
        c->x[0] = P(a); c->x[1] = P("brown"); handler("strstr")(c); ok &= c->x[0] == P(a + 10);
        c->x[0] = P(a); c->x[1] = 'q'; handler("strchr")(c); ok &= c->x[0] == P(a + 4);
        for (int i = 0; i < 64; i++) m[i] = (char)i;
        c->x[0] = P(m + 3); c->x[1] = P(m); c->x[2] = 40; handler("memmove")(c);
        for (int i = 0; i < 40; i++) ok &= m[3 + i] == (char)i;
        c->x[0] = P(m); c->x[1] = 0x5a; c->x[2] = 64; handler("memset")(c);
        for (int i = 0; i < 64; i++) ok &= m[i] == 0x5a;
        c->x[0] = P(b); c->x[1] = P(a); c->x[2] = 20; handler("memcmp")(c); ok &= (int)c->x[0] == 0;
        c->x[0] = P("  -1234xyz"); handler("atoi")(c); ok &= (int)c->x[0] == -1234;
        c->x[0] = P("-0x1f"); c->x[1] = 0; c->x[2] = 0; handler("strtol")(c); ok &= (int64_t)c->x[0] == -31;
        c->x[0] = P("3.25e2"); handler("atof")(c); ok &= c->v[0].df[0] == 325.0;
        c->x[0] = 'Q'; handler("tolower")(c); ok &= (int)c->x[0] == 'q';
        printf("  libc: strlen strcpy strcmp strstr strchr memmove memset memcmp atoi strtol atof tolower: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
    }
    {   /* snprintf with integer, string and floating-point arguments (AArch64 variadic call: x3.., v0..) */
        char out[128], ref[128];
        memset(&c->v[0], 0, 32);
        c->x[0] = P(out); c->x[1] = sizeof out; c->x[2] = P("%d|%5.2f|%s|%x|%lld|%e|%g"); c->x[3] = (uint64_t)(int64_t)-42; c->x[4] = P("str"); c->x[5] = 0xbeef;
        c->x[6] = (uint64_t)-1234567890123ll; c->v[0].df[0] = 3.14159; c->v[1].df[0] = 12345.678; c->v[2].df[0] = 0.0001;
        handler("snprintf")(c);
        snprintf(ref, sizeof ref, "%d|%5.2f|%s|%x|%lld|%e|%g", -42, 3.14159, "str", 0xbeef, -1234567890123ll, 12345.678, 0.0001);
        int ok = !strcmp(out, ref) && (int)c->x[0] == (int)strlen(ref);
        printf("  libc: snprintf \"%s\": %s\n", out, ok ? "ok" : "FAILED");
        bad += !ok;
    }
    {   /* qsort with a guest callback, errno location, tick conversion */
        int v[9] = {5, -3, 9, 0, 7, 7, -8, 2, 1}, ok = 1;
        c->x[0] = P(v); c->x[1] = 9; c->x[2] = 4; c->x[3] = hostfn_address("audit self-test comparator", selftest_cmp_int);
        handler("qsort")(c);
        for (int i = 1; i < 9; i++) ok &= v[i - 1] <= v[i];
        printf("  libc: qsort through a guest callback: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
        handler("__errno_location")(c);
        ok = c->x[0] == c->tpidr + 0x1d8;
        printf("  libc: errno lives in the thread's own block (thread pointer + 0x1d8): %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
        c->x[0] = 19200000ull * 3; handler("_ZN2nn2os17ConvertToTimeSpanENS0_4TickE")(c); ok = c->x[0] == 3000000000ull;
        c->x[0] = 0x7fffffffffull; handler("_ZN2nn2os17ConvertToTimeSpanENS0_4TickE")(c); ok &= c->x[0] == (uint64_t)(((unsigned __int128)0x7fffffffffull * 1000000000ull) / 19200000ull);
        printf("  nn::os: tick -> time span conversions are exact integers: %s\n", ok ? "ok" : "FAILED");
        bad += !ok;
    }
    return bad;
}

/* ---------------------------------------------------------------- under the lifted nn::mem::StandardAllocator
 * (sdklift_roots.txt `cut` lines). The allocator itself (nlib nmalloc, 86 functions) is the SDK's code; these are the
 * functions below it that end in kernel calls or in musl's per-thread data.
 *   pthread_once            musl: control word 0 = not run, 1 = running, 2 = done; the routine runs exactly once
 *   pthread_key_*           on the host's nn::os TLS slots (the console's musl does the same through __nnmusl_AllocateTlsSlot)
 *   nn::os::AllocateMemoryBlock / FreeMemoryBlock   [sdk 0x3326b0 / 0x3326f0] pages of the memory heap; failure = 0x1003
 *                           ([sdk 0x3355d8] os OutOfMemory). Host: committed pages from Windows.
 *   nn::os::SetMemoryPermission   [sdk 0x339730] accepts 0 (none), 1 (read), 3 (read-write) -> VirtualProtect
 *   nn::os::SetMemoryAttribute    cache attribute of a mapping (svc 3): no counterpart for ordinary host pages
 *   nn::os::IsVirtualAddressMemoryEnabled   [sdk 0x336af0] kernel GetInfo(16): true for a 64-bit application
 * The game gives its StandardAllocator a buffer (trace: Initialize(this, buffer, 0x100000)), so the
 * memory-block functions are only reached by the self-test's buffer-less allocator. */
static void pt_once(Ctx *c) {
    volatile LONG *ctl = (volatile LONG *)(uintptr_t)c->x[0];
    uint64_t fn = c->x[1];
    for (;;) {
        LONG v = InterlockedCompareExchange(ctl, 1, 0);
        if (v == 2) break;
        if (v == 0) { call_guest(c, fn); InterlockedExchange(ctl, 2); break; }
        SwitchToThread();
    }
    c->x[0] = 0;
}
static void pt_key_create(Ctx *c) { uint64_t key = c->x[0]; handler("_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE")(c); (void)key; c->x[0] = 0; }
static void pt_getspecific(Ctx *c) { c->x[0] &= 0xffffffffu; handler("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE")(c); }
static void pt_setspecific(Ctx *c) { c->x[0] &= 0xffffffffu; handler("_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm")(c); c->x[0] = 0; }
static void os_AllocateMemoryBlock(Ctx *c) {
    void *p = VirtualAlloc(NULL, (SIZE_T)c->x[1], MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (p) wr64(c->x[0], P(p));
    c->x[0] = p ? 0 : 0x1003;
}
static void os_FreeMemoryBlock(Ctx *c) { VirtualFree((void *)(uintptr_t)c->x[0], 0, MEM_RELEASE); }
static void os_SetMemoryPermission(Ctx *c) {
    DWORD old, prot = (uint32_t)c->x[2] == 3 ? PAGE_READWRITE : (uint32_t)c->x[2] == 1 ? PAGE_READONLY : PAGE_NOACCESS;
    if ((uint32_t)c->x[2] != 0 && (uint32_t)c->x[2] != 1 && (uint32_t)c->x[2] != 3) guest_trap("nn::os::SetMemoryPermission: invalid permission", c->x[30] - 4);
    cxxrt_mem_changed(c->x[0], c->x[1]);      /* nncxxrt.c: remembered readable checks of this range are forgotten (P3) */      // PR #2 (jfb112697): passes the range; prototype from hostrt.h (via sdklift.h)
    if (!VirtualProtect((void *)(uintptr_t)c->x[0], (SIZE_T)c->x[1], prot, &old)) guest_trap("nn::os::SetMemoryPermission: VirtualProtect failed", c->x[0]);
    cxxrt_mem_changed(c->x[0], c->x[1]);      // PR #2 (jfb112697): and after it, as the frees do: a check that ran in between saw the old state
}
static void os_SetMemoryAttribute(Ctx *c) { (void)c; }
static void os_FreeAddressRegion2(Ctx *c) {      // PR #2 (review): report the release before and after, as os_FreeAddressRegion in nnsdk.c does
    uint64_t base = c->x[0];
    cxxrt_mem_changed(base, 0); VirtualFree((void *)(uintptr_t)base, 0, MEM_RELEASE); cxxrt_mem_changed(base, 0);
    c->x[0] = 0;
}
static void os_IsVammEnabled(Ctx *c) { c->x[0] = 1; }
static void install_mem_overrides(void) {
    sdk_host_override("pthread_once", pt_once);
    sdk_host_override("pthread_key_create", pt_key_create);
    sdk_host_override("pthread_getspecific", pt_getspecific);
    sdk_host_override("pthread_setspecific", pt_setspecific);
    sdk_host_override("_ZN2nn2os19AllocateMemoryBlockEPmm", os_AllocateMemoryBlock);
    sdk_host_override("_ZN2nn2os15FreeMemoryBlockEmm", os_FreeMemoryBlock);
    sdk_host_override("_ZN2nn2os19SetMemoryPermissionEmmNS0_16MemoryPermissionE", os_SetMemoryPermission);
    sdk_host_override("_ZN2nn2os18SetMemoryAttributeEmmNS0_15MemoryAttributeE", os_SetMemoryAttribute);
    sdk_host_override("_ZN2nn2os17FreeAddressRegionEm", os_FreeAddressRegion2);
    sdk_host_override("_ZN2nn2os29IsVirtualAddressMemoryEnabledEv", os_IsVammEnabled);
}


static void selftest(void) {
    Ctx *c = _aligned_malloc(sizeof *c, 16);
    uint8_t *stack = VirtualAlloc(NULL, 8 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    memset(c, 0, sizeof *c);
    c->x[31] = P(stack + (8 << 20) - 0x1000);
    c->tpidr = P(VirtualAlloc(NULL, 0x10000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));   /* the thread's own block: lifted libc keeps errno there */
    int bad = 0;
    printf("SDK self-test (lifted sdk.elf code + offline service answers)\n");
    if (!sdk_image_ready()) { printf("  sdk.elf is not loaded: nothing can be tested\n"); fflush(stdout); ExitProcess(99); }
    int missing = sdk_missing_host_callees(stdout);
    printf("  sdklift: %d functions lifted, %d host callees, %d without a handler\n", sdk_fn_count, sdk_host_name_count, missing);
    bad += missing;
    if (getenv("RECOMPS_NVN_SELFTEST")) {                 /* [Recomps#1] only the nvn_sdk.c checks */
        bad += nvn_sdk_selftest(c);
        printf("RECOMPS_NVN_SELFTEST: %d failures\n", bad);
        fflush(stdout);
        ExitProcess((UINT)bad);
    }
    bad += libc_selftest(c);
    /* [Recomps#1] nn::mii removed: our game imports none of it */
    bad += nnfsdir_selftest(c);
    bad += nnfs_selftest(c);
    bad += bcat_selftest(c);
    bad += nnhid_selftest(c);
    bad += nvn_sdk_selftest(c);
    bad += nntime_selftest(c);
    { int close5_lead_selftest(Ctx *c); bad += close5_lead_selftest(c); }  /* nnmisc.c (close5): pctl answers, image protection */
    printf("SDK self-test: %d failure(s)\n", bad);
    fflush(stdout);
    ExitProcess((UINT)bad);
}

/* Imports whose host re-implementation is replaced by the real code lifted from sdk.elf (plan\FAKE_AUDIT.md, part 2;
 * list generated by tools_audit\lift_select.py). Debug aids: WT_LIFT_INSTALL=0 keeps every host handler;
 * WT_LIFT_SKIP=<text>[,<text>...] keeps the host handler of the names containing one of the texts. */
#include "sdklift_install.h"
static void install_lifted(void) {
    int n = (int)(sizeof sdk_install_names / sizeof sdk_install_names[0]), ok = 0, skipped = 0;
    const char *off = getenv("WT_LIFT_INSTALL"), *skip = getenv("WT_LIFT_SKIP");
    if (!sdk_image_ready() || (off && atoi(off) == 0)) return;
    for (int i = 0; i < n; i++) {
        int skip_this = 0;
        if (!sdk_install_names[i][0]) continue;          /* [witchtime] the placeholder of an empty list */
        if (skip && skip[0]) {
            char buf[1024];
            snprintf(buf, sizeof buf, "%s", skip);
            for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) if (strstr(sdk_install_names[i], t)) skip_this = 1;
        }
        if (skip_this) { skipped++; continue; }
        if (sdk_install(sdk_install_names[i])) ok++;
        else printf("sdklift: %s is in sdklift_install.h but was not lifted or is not an import\n", sdk_install_names[i]);
    }
    printf("sdklift: %d host re-implementations replaced by the real SDK code (%d kept by WT_LIFT_SKIP)\n", ok, skipped);
}

void register_sdk2_imports(void) {
    register_sdklift_imports();
    /* [Recomps#1] register_nnmii_imports(): nn::mii removed, our game imports none of it */
    register_nnfsdir_imports();
    install_mem_overrides();
    install_lifted();
    nnnet_register_final();
    dump_unregistered();
    if (getenv("WT_SDK_SELFTEST")) selftest();
}
