/* Host implementations of the Nintendo SDK functions the game imports.
 * Grown one function at a time from what the boot trace asks for. Each entry
 * names the mangled symbol; behaviour follows the public SDK semantics. */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "gfx_options.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define P(n) ((void *)(uintptr_t)c->x[n])

static void keep(Ctx *c) { (void)c; }                   /* returns its first argument (this) unchanged */
static void ret0(Ctx *c) { RET(0); }                    /* nn::Result success, false, or null */


/* ---- nn::os: time. The Switch system tick runs at 19.2 MHz. */
#define TICK_HZ 19200000ULL
/* Test aid WT_TICK_CALLERS=1 (track V session 4, looking for what seeds the game's random numbers): every distinct
 * return address of a caller of the clock functions is printed once. */
void host_clock_caller(const char *what, uint64_t lr) {
    static int on = -1, n;
    static uint64_t seen[512];
    if (on < 0) on = getenv("WT_TICK_CALLERS") != NULL;
    if (!on) return;
    for (int i = 0; i < n && i < 512; i++) if (seen[i] == lr) return;
    if (n < 512) { seen[n++] = lr; printf("clock caller: %s from %llx\n", what, (unsigned long long)lr); }
}
static void os_GetSystemTick(Ctx *c) {
    LARGE_INTEGER now, freq;
    host_clock_caller("GetSystemTick", c->x[30]);
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    RET((uint64_t)((double)now.QuadPart * (double)TICK_HZ / (double)freq.QuadPart));
}
static void os_GetSystemTickFrequency(Ctx *c) { RET(TICK_HZ); }

/* ---- nn::os: thread-local storage slots (single guest thread for now) */
static uint64_t g_tls_values[64];
static uint32_t g_tls_next;
static void os_AllocateTlsSlot(Ctx *c) { wr32(A0, g_tls_next < 63 ? g_tls_next++ : 63); RET(0); }
static void os_GetTlsValue(Ctx *c) { RET(g_tls_values[A0 & 63]); }
static void os_SetTlsValue(Ctx *c) { g_tls_values[A0 & 63] = A1; }

/* ---- nn::os: mutexes. One guest thread runs, so locking always succeeds. */
static void os_InitializeMutex(Ctx *c) { memset(P(0), 0, 0x20); }
static void os_TryLock(Ctx *c) { RET(1); }

/* ---- libc bits that live in the SDK */
static int g_errno;
static void c_errno_location(Ctx *c) { RET(&g_errno); }



/* ---- nn::os: virtual memory, used by the game's own allocator (jemalloc) */
/* cxxrt_guest_region, cxxrt_mem_changed: nncxxrt.c (P3) */      // PR #2 (jfb112697): prototypes now in hostrt.h (cxxrt_mem_changed takes the range)
/* ---- more libc */
static void c_strtol(Ctx *c) {
    char *end = NULL;
    long long v = strtoll(P(0), &end, (int)A2);
    if (A1) wr64(A1, (uint64_t)end);
    RET(v);
}

/* ---- nn::os: current thread (one guest thread for now) */
static uint8_t g_main_thread[0x400];
static void os_GetCurrentThread(Ctx *c) { RET(g_main_thread); }

/* ---- nn::oe: operating environment. Fixed answers: docked, Italian. */
/* 0 handheld, 1 console (docked). Track O: console_mode = handheld in config\graphics.ini (gfx_options.c) answers 0. */
static void oe_GetOperationMode(Ctx *c) {
    static int mode = -1;
    if (mode < 0) { mode = gfx_options()->handheld ? 0 : 1; printf("nn::oe::GetOperationMode: %s (graphics.ini console_mode), first asked from %llx\n", mode ? "docked" : "handheld", (unsigned long long)c->x[30]); }
    RET(mode);
}
static void oe_GetDesiredLanguage(Ctx *c) { RET(0x7469); }           /* LanguageCode "it" */
static void settings_LanguageEquals(Ctx *c) { RET(rd32(A1) == 4); }   /* nn::settings::Language_Italian */

/* ---- nn::fs: nothing is mounted yet; calls succeed so start-up can go on */
static void fs_QueryMountRomCacheSize(Ctx *c) { wr64(A0, 0x10000); RET(0); }


/* True when a printf format has a floating-point conversion (looks only at conversion
 * specifiers, so letters in literal text such as "uTexture%d" are ignored). */
static int format_has_float(const char *fmt) {
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%') continue;
        while (*p && strchr("-+ #0123456789.*hlLqjzt", *p)) p++;
        if (*p && strchr("fFeEgGaA", *p)) return 1;
        if (!*p) break;
    }
    return 0;
}

/* ---- nn::util */
static void util_ReferSymbol(Ctx *c) { (void)c; }                                      /* returns its argument */

/* ---- C formatting: integer/pointer arguments arrive in x2..x7 (variadic AArch64 ABI).
 * Formats with floating-point conversions would need v0..v7 and are refused loudly. */
static void c_sprintf(Ctx *c) {
    const char *fmt = (const char *)(uintptr_t)A1;
    if (format_has_float(fmt)) guest_trap("sprintf with a floating-point conversion is not supported", c->x[30] - 4);
    int n = snprintf((char *)(uintptr_t)A0, 4096, fmt, (unsigned long long)A2, (unsigned long long)A3,
                     (unsigned long long)A4, (unsigned long long)c->x[5], (unsigned long long)c->x[6], (unsigned long long)c->x[7]);
    RET((int64_t)n);
}
static void c_snprintf(Ctx *c) {
    const char *fmt = (const char *)(uintptr_t)A2;
    if (format_has_float(fmt)) guest_trap("snprintf with a floating-point conversion is not supported", c->x[30] - 4);
    int n = snprintf((char *)(uintptr_t)A0, (size_t)A1, fmt, (unsigned long long)A3, (unsigned long long)A4,
                     (unsigned long long)c->x[5], (unsigned long long)c->x[6], (unsigned long long)c->x[7]);
    RET((int64_t)n);
}

/* ---- nn::lmem frame heap: bump allocation over a caller-supplied region. The HeapHead lives at
 * the start of the region and is opaque to the game (it only passes it back). A negative alignment
 * would allocate from the tail; the game uses positive alignments only. */
typedef struct { uint64_t start, head, tail, end; } FrameHeap;

/* ---- nn::mem::NumberLineAllocator: hands out integer ids from registered ranges (the game uses
 * it for texture/sampler descriptor slots). The game owns bytes +0x40.. of the object; the SDK
 * part (+0..+0x3f) holds a host pointer to a bitmap and the covered range. */
typedef struct { uint8_t *bits; int32_t begin, end; } NumberLine;


/* ---- nn::bcat delivery cache, client side as in sdk.elf; the bcat system module is not in the dump.
 * [sdk] 0xc1dd0 MountDeliveryCacheStorage -> 0xc5380 ShimLibraryGlobal::MountDeliveryCacheStorage: a storage
 *   service already held (global +0x10) gives 0x87a (module 122, description 4); otherwise the Result of
 *   CreateDeliveryCacheStorageService, and the service is kept. [doc] suyu bcat/service_creator.cpp: the module
 *   answers success with the application's delivery cache directory, which is empty when nothing was ever
 *   delivered (a console that was never online).
 * [sdk] 0xc54c0 Unmount: aborts when no storage is held or a file / directory is still open, else releases it.
 * [sdk] 0xc5770 Enumerate: aborts when no storage is held; asks the service (command 10) for the names.
 *   [doc] suyu delivery_cache_storage_service.cpp: count = directories present = 0.
 * [sdk] 0xc2110 RequestSyncDeliveryCache(progress): aborts when the storage is mounted; when
 *   nn::nifm::IsAnyInternetRequestAccepted(GetClientId()) is false it returns 0x107a (module 122, description 8)
 *   WITHOUT asking the bcat module. No network request is ever accepted here (nifm: no network).
 * [sdk] 0xc2260 the form with a directory name: a name of 32 or more characters, or one that
 *   DirectoryName::IsValid [0xc1be0] refuses (1..31 characters of A-Z a-z 0-9 _ -), gives 0x27a (description 1)
 *   before the mounted / network checks. */
#define BCAT_INVALID_ARGUMENT 0x27au
#define BCAT_ALREADY_MOUNTED 0x87au
#define BCAT_INTERNET_REQUEST_NOT_ACCEPTED 0x107au
volatile LONG g_bcat_mounted;                             /* also read by nnsvc_c5a.inc (DeliveryCacheDirectory / File Open) */
static void bcat_Mount(Ctx *c) {
    if (InterlockedCompareExchange(&g_bcat_mounted, 1, 0) != 0) { RET(BCAT_ALREADY_MOUNTED); return; }
    trace_printf("     nn::bcat::MountDeliveryCacheStorage -> mounted (empty delivery cache)\n");
    RET(0);
}
static void bcat_Unmount(Ctx *c) {
    if (InterlockedCompareExchange(&g_bcat_mounted, 0, 1) != 1)
        guest_trap("nn::bcat::UnmountDeliveryCacheStorage without a mounted storage (the SDK aborts, sdk 0xc5514)", c->x[30] - 4);
}
static void bcat_Enumerate(Ctx *c) {                         /* (int *outCount, DirectoryName *names, int max) */
    if (!g_bcat_mounted)
        guest_trap("nn::bcat::EnumerateDeliveryCacheDirectory without a mounted storage (the SDK aborts, sdk 0xc5800)", c->x[30] - 4);
    wr32(A0, 0);
    RET(0);
}
int bcat_dirname_valid(const char *s) {              /* [sdk] 0xc1be0 on the 32-byte copy */
    int n = 0;
    for (; n < 32 && s[n]; n++) {
        unsigned char ch = (unsigned char)s[n];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '_' || ch == '-')) return 0;
    }
    return n != 0 && n != 32;
}
static uint32_t bcat_sync_common(Ctx *c) {
    if (g_bcat_mounted)
        guest_trap("nn::bcat::RequestSyncDeliveryCache while the delivery cache storage is mounted (the SDK aborts, sdk 0xc2218)", c->x[30] - 4);
    return BCAT_INTERNET_REQUEST_NOT_ACCEPTED;
}
static void bcat_RequestSync(Ctx *c) { RET(bcat_sync_common(c)); }               /* (DeliveryCacheProgress *) */
static void bcat_RequestSyncDir(Ctx *c) {                                         /* (DeliveryCacheProgress *, const char *name) */
    const char *name = (const char *)(uintptr_t)A1;
    if (strnlen(name, 32) >= 32 || !bcat_dirname_valid(name)) { RET(BCAT_INVALID_ARGUMENT); return; }
    RET(bcat_sync_common(c));
}
int bcat_selftest(Ctx *c) {
    static const struct { const char *name; int valid; } t[] = {
        {"", 0}, {"a", 1}, {"spirits_event-01", 1}, {"with space", 0}, {"dot.name", 0},
        {"abcdefghijklmnopqrstuvwxyz01234", 1}, {"abcdefghijklmnopqrstuvwxyz012345", 0},
    };
    int ok = 1;
    for (int i = 0; i < (int)(sizeof t / sizeof t[0]); i++)
        if ((strnlen(t[i].name, 32) < 32 && bcat_dirname_valid(t[i].name)) != t[i].valid) ok = 0;
    printf("  bcat: directory name rule of sdk 0xc1be0 (7 names): %s\n", ok ? "ok" : "FAILED");
    {
        int32_t count = 7;
        int ok2 = 1;
        uint64_t lr = c->x[30];
        bcat_RequestSync(c); ok2 &= (uint32_t)c->x[0] == BCAT_INTERNET_REQUEST_NOT_ACCEPTED;
        c->x[1] = (uint64_t)(uintptr_t)"bad name"; bcat_RequestSyncDir(c); ok2 &= (uint32_t)c->x[0] == BCAT_INVALID_ARGUMENT;
        c->x[1] = (uint64_t)(uintptr_t)"good_name"; bcat_RequestSyncDir(c); ok2 &= (uint32_t)c->x[0] == BCAT_INTERNET_REQUEST_NOT_ACCEPTED;
        bcat_Mount(c); ok2 &= (uint32_t)c->x[0] == 0;
        bcat_Mount(c); ok2 &= (uint32_t)c->x[0] == BCAT_ALREADY_MOUNTED;
        c->x[0] = (uint64_t)(uintptr_t)&count; c->x[1] = 0; c->x[2] = 100; bcat_Enumerate(c); ok2 &= (uint32_t)c->x[0] == 0 && count == 0;
        bcat_Unmount(c); ok2 &= g_bcat_mounted == 0;
        bcat_Mount(c); ok2 &= (uint32_t)c->x[0] == 0;
        bcat_Unmount(c);
        c->x[30] = lr;
        printf("  bcat: sync refused 0x107a / 0x27a, mount, second mount 0x87a, 0 directories, unmount, mount again: %s\n", ok2 ? "ok" : "FAILED");
        ok &= ok2;
    }
    return !ok;
}

/* ---- nn::friends: no friends, no network */
/* [sdk 0x1d9ed0] GetFriendList(int *outCount, Friend *out, const Uid &, offset, max, filter): detail::CheckUserStatus(uid)
 * [0x1e0350] first, its Result returned with the count untouched; then ONE request to the friend service (IFriendService slot
 * +0x38), a failure returned with the count untouched, success writes the count. The service (not in the dump) answers
 * success with 0 friends (suyu friend/friend.cpp GetFriendList, command 10101). */
uint32_t fr_check_user(Ctx *c, uint64_t uid);               /* nnnet.c */

/* ---- nn::mem::StandardAllocator: backed by the host heap. The region the game hands to
 * Initialize is left untouched; its size is only remembered for GetTotalFreeSize. */
static uint64_t g_std_alloc_size = 0x10000000;
static void stdalloc_Initialize(Ctx *c) { g_std_alloc_size = A2; }
static void stdalloc_Allocate(Ctx *c) { RET(_aligned_malloc(A1 ? (size_t)A1 : 1, 16)); }
static void stdalloc_AllocateAligned(Ctx *c) { RET(_aligned_malloc(A1 ? (size_t)A1 : 1, A2 < 16 ? 16 : (size_t)A2)); }
static void stdalloc_Free(Ctx *c) { _aligned_free((void *)(uintptr_t)A1); }
static void stdalloc_Reallocate(Ctx *c) { RET(_aligned_realloc((void *)(uintptr_t)A1, A2 ? (size_t)A2 : 1, 16)); }



static const ImportDef sdk_imports[] = {
    {"_ZN2nn7friends12UserPresenceC1Ev", keep},
    {"_ZN2nn7friends12UserPresence10InitializeERKNS_7account3UidE", ret0},
    {"_ZN2nn3mem17StandardAllocator10InitializeEPvm", stdalloc_Initialize},
    {"_ZN2nn3mem17StandardAllocator10InitializeEPvmb", stdalloc_Initialize},     /* [Recomps#1] SDK 3.6.0 form (+ bool thread cache): same host heap */
    {"_ZN2nn3mem17StandardAllocator8FinalizeEv", keep},
    {"_ZN2nn3mem17StandardAllocator8AllocateEm", stdalloc_Allocate},
    {"_ZN2nn3mem17StandardAllocator8AllocateEmm", stdalloc_AllocateAligned},
    {"_ZN2nn3mem17StandardAllocator4FreeEPv", stdalloc_Free},
    {"_ZN2nn3mem17StandardAllocator10ReallocateEPvm", stdalloc_Reallocate},
    {"_ZN2nn4util11ReferSymbolEPKv", util_ReferSymbol},
    {"sprintf", c_sprintf}, {"snprintf", c_snprintf},
    {"_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi", keep},
    {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", keep},
    {"_ZN2nn2oe16DisableRecordingEv", keep},
    {"_ZN2nn2oe16GetOperationModeEv", oe_GetOperationMode},
    {"_ZN2nn2oe18GetDesiredLanguageEv", oe_GetDesiredLanguage},
    {"_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE", settings_LanguageEquals},
    {"_ZN2nn2fs22QueryMountRomCacheSizeEPm", fs_QueryMountRomCacheSize},
    {"_ZN2nn2fs8MountRomEPKcPvm", ret0},
    {"_ZN2nn4time13IsInitializedEv", ret0},
    {"_ZN2nn4time10InitializeEv", ret0},
    {"_ZN2nn2os16GetCurrentThreadEv", os_GetCurrentThread},
    {"_ZN2nn2os20GetCurrentCoreNumberEv", ret0},
    {"strtol", c_strtol}, {"strtoll", c_strtol},
    {"_ZN2nn3mem17StandardAllocatorC1Ev", keep},
    {"_ZN2nn2os13GetSystemTickEv", os_GetSystemTick},
    {"_ZN2nn2os22GetSystemTickFrequencyEv", os_GetSystemTickFrequency},
    {"_ZN2nn2os18SdkAllocateTlsSlotEPNS0_7TlsSlotEPFvmE", os_AllocateTlsSlot},
    {"_ZN2nn2os15AllocateTlsSlotEPNS0_7TlsSlotEPFvmE", os_AllocateTlsSlot},
    {"_ZN2nn2os11GetTlsValueENS0_7TlsSlotE", os_GetTlsValue},
    {"_ZN2nn2os11SetTlsValueENS0_7TlsSlotEm", os_SetTlsValue},
    {"_ZN2nn2os15InitializeMutexEPNS0_9MutexTypeEbi", os_InitializeMutex},
    {"_ZN2nn2os13FinalizeMutexEPNS0_9MutexTypeE", keep},
    {"_ZN2nn2os9LockMutexEPNS0_9MutexTypeE", keep},
    {"_ZN2nn2os11UnlockMutexEPNS0_9MutexTypeE", keep},
    {"_ZN2nn2os12TryLockMutexEPNS0_9MutexTypeE", os_TryLock},
    {"pthread_mutex_lock", ret0}, {"pthread_mutex_unlock", ret0},
    {"pthread_mutex_init", ret0}, {"pthread_mutex_destroy", ret0},
    {"__errno_location", c_errno_location},
    {"_ZNSt3__15mutex4lockEv", keep},
    {"_ZNSt3__15mutex6unlockEv", keep},
    {"_ZNSt3__15mutexD1Ev", keep},
};

void register_sdk_imports(void) {
    register_imports(sdk_imports, (int)(sizeof sdk_imports / sizeof sdk_imports[0]));
}
