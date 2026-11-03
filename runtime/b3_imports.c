/* [witchtime] Imports of Bayonetta 3's main (nnSdk 13.3.2) that B2recomp's runtime had no handler for, reached at boot.
 * Registered by boot.c after r2_imports.c (later registrations replace earlier ones). Each one says what it models;
 * the "no-op" ones are services whose answer cannot change what a PC build does (notifications, recording, mouse). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include "runtime.h"
#include "hostrt.h"
#include "image_layout.h"

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define RET(v) (c->x[0] = (uint64_t)(v))

static void nothing(Ctx *c) { (void)c; }
static void ret0(Ctx *c) { RET(0); }
static void ret2(Ctx *c) { RET(2); }

/* pthread_once(pthread_once_t *, void (*)(void)): musl's once word, 0 = not run, 1 = running, 2 = done.
 * shortcut: waiters spin on Sleep(0) (as cxx_call_once below); fine for one-time init, upgrade if a profile shows it */
static void b3_pthread_once(Ctx *c) {
    volatile LONG *once = (volatile LONG *)(uintptr_t)A0;
    uint64_t fn = A1;
    if (InterlockedCompareExchange(once, 1, 0) == 0) {
        call_guest(c, fn);
        InterlockedExchange(once, 2);
    } else {
        while (*once != 2) Sleep(0);
    }
    RET(0);
}

/* nn::os::SdkMutexType: one 32-bit word in guest memory, 0 free, 1 held. Not recursive (the SDK's isn't).
 * shortcut: spins with Sleep(0) instead of the kernel's ArbitrateLock wait queue; upgrade to a futex if it shows in a profile */
static void sdkmutex_init(Ctx *c) { wr32(A0, 0); }
static void sdkmutex_lock(Ctx *c) {
    volatile LONG *w = (volatile LONG *)(uintptr_t)A0;
    while (InterlockedCompareExchange(w, 1, 0) != 0) Sleep(0);
}
static void sdkmutex_unlock(Ctx *c) { InterlockedExchange((volatile LONG *)(uintptr_t)A0, 0); }

/* nn::os::ReaderWriterLockType: the guest object's first 8 bytes point at host state (made by Initialize, or on first use
 * of a zero-filled object). nn::os semantics: the writer may take the read and write locks again (counted), readers wait
 * while another thread writes, a writer waits for no readers and no other writer. Thread identity = the guest thread
 * pointer (one per nn::os thread). shortcut: no writer preference (a stream of readers can starve a writer); upgrade if
 * a hang shows a writer waiting. */
typedef struct { SRWLOCK m; CONDITION_VARIABLE cv; int readers, writes, owner_reads; uint64_t owner; } RwLock;
static RwLock *rw_get(uint64_t obj) {
    RwLock *r = (RwLock *)(uintptr_t)rd64(obj);
    if (r) return r;
    RwLock *n = calloc(1, sizeof *n);
    InitializeSRWLock(&n->m);
    InitializeConditionVariable(&n->cv);
    if (InterlockedCompareExchange64((volatile LONG64 *)(uintptr_t)obj, (LONG64)(uintptr_t)n, 0) != 0) { free(n); return (RwLock *)(uintptr_t)rd64(obj); }
    return n;
}
static void rw_init(Ctx *c) { wr64(A0, 0); rw_get(A0); }
static void rw_fini(Ctx *c) { RwLock *r = (RwLock *)(uintptr_t)rd64(A0); wr64(A0, 0); if (r) free(r); }
static void rw_read(Ctx *c) {
    RwLock *r = rw_get(A0);
    AcquireSRWLockExclusive(&r->m);
    if (r->writes && r->owner == c->tpidr) r->owner_reads++;
    else { while (r->writes) SleepConditionVariableSRW(&r->cv, &r->m, INFINITE, 0); r->readers++; }
    ReleaseSRWLockExclusive(&r->m);
}
static void rw_read_release(Ctx *c) {
    RwLock *r = rw_get(A0);
    AcquireSRWLockExclusive(&r->m);
    if (r->writes && r->owner == c->tpidr && r->owner_reads) r->owner_reads--;
    else if (r->readers) r->readers--;
    WakeAllConditionVariable(&r->cv);
    ReleaseSRWLockExclusive(&r->m);
}
static void rw_write(Ctx *c) {
    RwLock *r = rw_get(A0);
    AcquireSRWLockExclusive(&r->m);
    if (!(r->writes && r->owner == c->tpidr)) {
        while (r->writes || r->readers) SleepConditionVariableSRW(&r->cv, &r->m, INFINITE, 0);
        r->owner = c->tpidr;
    }
    r->writes++;
    ReleaseSRWLockExclusive(&r->m);
}
static void rw_write_release(Ctx *c) {
    RwLock *r = rw_get(A0);
    AcquireSRWLockExclusive(&r->m);
    if (r->writes && r->owner == c->tpidr && --r->writes == 0) { r->owner = 0; r->readers += r->owner_reads; r->owner_reads = 0; }
    WakeAllConditionVariable(&r->cv);
    ReleaseSRWLockExclusive(&r->m);
}

/* nn::util::pmr: NewDeleteResource() is a MemoryResource whose do_allocate / do_deallocate go to the game's own
 * aligned_alloc / free (main exports both, image_layout.h), as operator new / delete do in the SDK.
 * vtable (Itanium, as nn::MemoryResource): [0] ~dtor, [1] deleting dtor, [2] do_allocate(this, bytes, align),
 * [3] do_deallocate(this, p, bytes, align), [4] do_is_equal(this, other). */
static uint64_t g_nd_vtable[5];
static uint64_t g_nd_object[1];                      /* { vtable } */
static uint64_t g_default_resource;
static void nd_allocate(Ctx *c) {
    uint64_t bytes = A1, align = A2;
    c->x[0] = align < 16 ? 16 : align;
    c->x[1] = bytes ? bytes : 1;
    call_guest(c, GUEST_BASE + GAME_ALIGNED_ALLOC_OFF);
}
static void nd_deallocate(Ctx *c) { c->x[0] = A1; call_guest(c, GUEST_BASE + GAME_FREE_OFF); }
static void nd_is_equal(Ctx *c) { RET(A0 == A1); }
static uint64_t new_delete_resource(void) {
    if (!g_nd_vtable[0]) {
        g_nd_vtable[0] = hostfn_address("pmr::NewDeleteResource::~", nothing);
        g_nd_vtable[1] = hostfn_address("pmr::NewDeleteResource::~ (deleting)", nothing);
        g_nd_vtable[2] = hostfn_address("pmr::NewDeleteResource::do_allocate", nd_allocate);
        g_nd_vtable[3] = hostfn_address("pmr::NewDeleteResource::do_deallocate", nd_deallocate);
        g_nd_vtable[4] = hostfn_address("pmr::NewDeleteResource::do_is_equal", nd_is_equal);
        g_nd_object[0] = (uint64_t)(uintptr_t)g_nd_vtable;
    }
    return (uint64_t)(uintptr_t)g_nd_object;
}
static void pmr_NewDeleteResource(Ctx *c) { RET(new_delete_resource()); }
static void pmr_GetDefaultResource(Ctx *c) { RET(g_default_resource ? g_default_resource : new_delete_resource()); }
static void pmr_SetDefaultResource(Ctx *c) {         /* returns the previous one; null sets the new/delete resource back */
    uint64_t old = g_default_resource ? g_default_resource : new_delete_resource();
    g_default_resource = A0;
    RET(old);
}

/* nn::fs::Get/SetCurrentThreadFsContext: the context only decides whether an fs error aborts the program; the host
 * never aborts on one, so one shared context object is handed out and Set is remembered per thread.
 * shortcut: the context's contents are never read; model them if the game inspects what Get returns */
static uint64_t g_fs_context[4];
static _Thread_local uint64_t t_fs_context;
static void fs_GetCurrentThreadFsContext(Ctx *c) { RET(t_fs_context ? t_fs_context : (uint64_t)(uintptr_t)g_fs_context); }
static void fs_SetCurrentThreadFsContext(Ctx *c) { t_fs_context = A0; }

/* nn::oe::GetPerformanceMode: 1 (boost) docked, 0 (normal) handheld: the same answer as GetOperationMode. */
static void oe_GetPerformanceMode(Ctx *c) {
    ImportFn op = import_lookup("_ZN2nn2oe16GetOperationModeEv");
    if (op) op(c); else RET(1);
}
/* nn::oe::GetPerformanceConfiguration(PerformanceMode): program 1 (the picture book) asks for the clock set of a mode.
 * shortcut: one value for both modes, 0x00020003 (the docked default set; NOT VERIFIED against a console); model it per
 * mode if the program draws differently by the answer */
static void oe_GetPerformanceConfiguration(Ctx *c) { RET(0x00020003u); }

/* [witchtime] the program switch (issue 115): the picture book on Bayonetta 3's shelf runs program 1 of the application
 * (the Bayonetta Origins prologue), which runs program 0 again at its end. nn::oe::ExecuteProgram(index, param, size)
 * ends this program on the console and starts the other one, which gets param from TryPopLaunchParameter. Here it
 * writes the switch file (<userdata>/program_switch.bin, or WT_PROGRAM_SWITCH) and ends the process with exit code
 * PROGRAM_SWITCH_EXIT; the launcher then starts program <index> (launcher/src/game_host.cpp). The file: "WTPS", the
 * program index (u32), the size (u32), the parameter. The program started reads it once and deletes it. */
#define PROGRAM_SWITCH_EXIT 86
#define PROGRAM_SWITCH_MAX 0x10000
static void switch_path(char *p, size_t n) {
    const char *e = getenv("WT_PROGRAM_SWITCH"), *u = getenv("WT_USERDATA");
    if (e && e[0]) snprintf(p, n, "%s", e);
    else snprintf(p, n, "%s/program_switch.bin", u && u[0] ? u : "../userdata");
}
static uint8_t g_launch[PROGRAM_SWITCH_MAX];
static int g_launch_size = -1;                           /* -1: this start did not come from a program switch */
static volatile LONG g_launch_read;
/* boot.c, before the game runs: the parameter a program switch left for this program (WT_PROGRAM = its index, 0 if
 * unset), taken out of the file so a later plain start does not see it */
void program_switch_take(void) {
    char p[1024];
    switch_path(p, sizeof p);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    uint8_t h[12];
    const char *pe = getenv("WT_PROGRAM");
    uint32_t want = pe ? (uint32_t)atoi(pe) : 0, index, size;
    if (fread(h, 1, 12, f) == 12 && !memcmp(h, "WTPS", 4) && (memcpy(&index, h + 4, 4), memcpy(&size, h + 8, 4), index == want)
        && size <= PROGRAM_SWITCH_MAX && fread(g_launch, 1, size, f) == size) {
        g_launch_size = (int)size;
        printf("program switch: started as program %u with a %u-byte launch parameter\n", index, size);
    } else printf("program switch: %s is not for this program (%u), ignored\n", p, want);
    fclose(f);
    remove(p);
}
int program_switched_in(void) { return g_launch_size >= 0; }   /* nnfs.c PopOpenUsers: the previous program's user */
/* bool TryPopLaunchParameter(size_t *out_size, void *buf, size_t buf_size): once, as the console's queue */
static void oe_TryPopLaunchParameter(Ctx *c) {
    if (g_launch_size < 0 || InterlockedExchange(&g_launch_read, 1)) { RET(0); return; }
    size_t n = (size_t)g_launch_size < A2 ? (size_t)g_launch_size : (size_t)A2;
    memcpy((void *)(uintptr_t)A1, g_launch, n);
    wr64(A0, (uint64_t)g_launch_size);
    RET(1);
}
static void oe_ExecuteProgram(Ctx *c) {
    char p[1024];
    uint32_t index = (uint32_t)A0, size = (uint32_t)(A2 < PROGRAM_SWITCH_MAX ? A2 : PROGRAM_SWITCH_MAX);
    switch_path(p, sizeof p);
    FILE *f = fopen(p, "wb");
    int ok = f && fwrite("WTPS", 1, 4, f) == 4 && fwrite(&index, 4, 1, f) == 1 && fwrite(&size, 4, 1, f) == 1
             && fwrite((const void *)(uintptr_t)A1, 1, size, f) == size;
    if (f && fclose(f)) ok = 0;
    printf("\nprogram switch: ExecuteProgram(%u, %u bytes) %s %s\n", index, size, ok ? "written to" : "COULD NOT BE WRITTEN to", p);
    report_and_exit(ok ? PROGRAM_SWITCH_EXIT : 5);
}

/* nn::hid::GetMouseState(MouseState *): no mouse on the console side (the PC mouse drives the camera through nnpad);
 * samplingNumber, x, y, deltas, wheel, buttons, attributes all 0 = not connected. */
static void hid_GetMouseState(Ctx *c) { memset((void *)(uintptr_t)A0, 0, 40); }
/* [witchtime] program 1 (the picture book). GetTouchScreenStates<1>(TouchScreenState<1> *, int count): one state with no
 * touch (samplingNumber 0, count 0, the touch record zero; 0x38 bytes) = nobody touches the screen. */
static void hid_GetTouchScreenStates1(Ctx *c) { if ((int)A1 < 1) { RET(0); return; } memset((void *)(uintptr_t)A0, 0, 0x38); RET(1); }
/* GetVibrationDeviceInfo(VibrationDeviceInfo *, handle): {type 1 = linear resonant actuator, position} as a Pro
 * Controller's two motors; the position is the handle's device index (byte 3: 0 left, 1 right) */
static void hid_GetVibrationDeviceInfo(Ctx *c) { wr32(A0, 1); wr32(A0 + 4, rd32(A1) >> 24 & 1); }
/* clock(): the time since the start in microseconds (the SDK's musl: CLOCKS_PER_SEC 1000000). shortcut: wall time, not
 * CPU time; program 1 only measures intervals with it */
static void c_clock(Ctx *c) {
    static LARGE_INTEGER f, t0;
    LARGE_INTEGER t;
    if (!f.QuadPart) { QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); }
    QueryPerformanceCounter(&t);
    RET((uint64_t)((t.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart));
}

/* libc++ threads (Nintendo's libc++ on nn::os). __libcpp_thread_get_current_id: a value that is the same on every call
 * from one thread and differs between threads, never 0 (0 is "no thread"; protobuf's init-recursion check compares it). */
static void cxx_thread_get_current_id(Ctx *c) { RET((uint64_t)GetCurrentThreadId() + 1); }
/* std::__call_once(volatile unsigned long &flag, void *arg, void (*fn)(void *)): libc++'s flag, 0 not run, 1 running,
 * ~0 done. A throwing fn would leave 1 here (libc++ resets it to 0); B3's callers do not throw. */
static void cxx_call_once(Ctx *c) {
    volatile LONG64 *flag = (volatile LONG64 *)(uintptr_t)A0;
    uint64_t arg = A1, fn = A2;
    if (InterlockedCompareExchange64(flag, 1, 0) == 0) {
        c->x[0] = arg;
        call_guest(c, fn);
        InterlockedExchange64(flag, -1);
    } else {
        while (*flag != -1) Sleep(0);
    }
}
/* std::this_thread::sleep_for(const chrono::nanoseconds &) */
static void cxx_sleep_for(Ctx *c) {
    int64_t ns = (int64_t)rd64(A0);
    if (ns > 0) Sleep((DWORD)((ns + 999999) / 1000000));
    else Sleep(0);
}

/* __nnmusl_init_dso: main's DT_INIT (musl _init) hands nnmusl the module's layout, linker symbols in x0..x7 and on the
 * stack, all of them relocated (a plain number such as an alignment arrives as image base + number). Read from B3's
 * main (docs/RE_NOTES.md, "TLS"): x0 = image base, x2 = .tdata start, x3 = .tdata end, x4 = base + TLS alignment,
 * x7 = end of the TLS block (.tbss end). The block sits at thread pointer + 16 rounded up to the alignment (AArch64
 * TLS variant 1, 16-byte TCB): main reads tpidr_el0 + 16 .. + 0x68 with local-exec offsets. Answer 1: _init then
 * skips its own init_array loop (boot.c runs the constructors). */
static uint64_t g_tls_image, g_tls_image_size, g_tls_size, g_tls_off;
void tls_init_block(uint64_t tp) {                      /* os_threads.c: every new thread block */
    if (!g_tls_size || !tp) return;
    memcpy((void *)(uintptr_t)(tp + g_tls_off), (const void *)(uintptr_t)g_tls_image, g_tls_image_size);
    memset((void *)(uintptr_t)(tp + g_tls_off + g_tls_image_size), 0, g_tls_size - g_tls_image_size);
}
static void musl_init_dso(Ctx *c) {
    uint64_t align = c->x[4] - c->x[0];
    g_tls_image = c->x[2];
    g_tls_image_size = c->x[3] - c->x[2];
    g_tls_size = c->x[7] - c->x[2];
    g_tls_off = (16 + align - 1) & ~(align - 1);
    if (!align || align & (align - 1) || c->x[3] < c->x[2] || c->x[7] < c->x[3] || g_tls_off + g_tls_size > 0x1d8)
        guest_trap("__nnmusl_init_dso: unexpected TLS layout", c->x[2]);   /* below the thread block's errno / ThreadType */
    printf("nnmusl: main's TLS block: %llu bytes (%llu initialised) at thread pointer + %llu (main thread block %llx)\n", (unsigned long long)g_tls_size,
           (unsigned long long)g_tls_image_size, (unsigned long long)g_tls_off, (unsigned long long)c->tpidr);
    tls_init_block(c->tpidr);
    RET(1);
}

static const ImportDef b3_imports[] = {
    {"__nnmusl_init_dso", musl_init_dso},
    {"_ZNSt3__130__libcpp_thread_get_current_idEv", cxx_thread_get_current_id},
    {"_ZNSt3__111__call_onceERVmPvPFvS2_E", cxx_call_once},
    {"_ZNSt3__111this_thread9sleep_forERKNS_6chrono8durationIxNS_5ratioILl1ELl1000000000EEEEE", cxx_sleep_for},
    {"pthread_once", b3_pthread_once},
    {"_ZN2nn2os12SdkMutexType10InitializeEv", sdkmutex_init},
    {"_ZN2nn2os12SdkMutexType4LockEv", sdkmutex_lock},
    {"_ZN2nn2os12SdkMutexType6UnlockEv", sdkmutex_unlock},
    {"_ZN2nn2os26InitializeReaderWriterLockEPNS0_20ReaderWriterLockTypeE", rw_init},
    {"_ZN2nn2os24FinalizeReaderWriterLockEPNS0_20ReaderWriterLockTypeE", rw_fini},
    {"_ZN2nn2os15AcquireReadLockEPNS0_20ReaderWriterLockTypeE", rw_read},
    {"_ZN2nn2os15ReleaseReadLockEPNS0_20ReaderWriterLockTypeE", rw_read_release},
    {"_ZN2nn2os16AcquireWriteLockEPNS0_20ReaderWriterLockTypeE", rw_write},
    {"_ZN2nn2os16ReleaseWriteLockEPNS0_20ReaderWriterLockTypeE", rw_write_release},
    {"_ZN2nn4util3pmr17NewDeleteResourceEv", pmr_NewDeleteResource},
    {"_ZN2nn4util3pmr18GetDefaultResourceEv", pmr_GetDefaultResource},
    {"_ZN2nn4util3pmr18SetDefaultResourceEPNS_14MemoryResourceE", pmr_SetDefaultResource},
    {"_ZN2nn2fs25GetCurrentThreadFsContextEv", fs_GetCurrentThreadFsContext},
    {"_ZN2nn2fs25SetCurrentThreadFsContextEPKNS0_9FsContextE", fs_SetCurrentThreadFsContext},
    {"_ZN2nn2oe18GetPerformanceModeEv", oe_GetPerformanceMode},
    {"_ZN2nn2oe27GetPerformanceConfigurationENS0_15PerformanceModeE", oe_GetPerformanceConfiguration},
    {"_ZN2nn2oe14ExecuteProgramEiPKvm", oe_ExecuteProgram}, {"_ZN2nn2oe21TryPopLaunchParameterEPmPvm", oe_TryPopLaunchParameter},
    {"_ZN2nn2oe28SetResumeNotificationEnabledEb", nothing},
    {"_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", nothing},
    {"_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", nothing},
    {"_ZN2nn2oe13MuteRecordingEv", nothing},
    {"_ZN2nn2oe15UnmuteRecordingEv", nothing},
    {"_ZN2nn3hid29SetNpadHandheldActivationModeENS0_26NpadHandheldActivationModeE", nothing},
    {"_ZN2nn3hid15InitializeMouseEv", nothing},
    {"_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", ret2},          /* the host output is opened as stereo */
    {"_ZN2nn3hid13GetMouseStateEPNS0_10MouseStateE", hid_GetMouseState},
    {"_ZN2nn3hid20GetTouchScreenStatesILm1EEEiPNS0_16TouchScreenStateIXT_EEEi", hid_GetTouchScreenStates1},
    {"_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", hid_GetVibrationDeviceInfo},
    {"clock", c_clock},
    {"_ZN2nn3hid21ShowControllerSupportERKNS0_20ControllerSupportArgE", ret0},   /* the applet: "controllers are fine" */
};

void register_b3_imports(void) {
    register_imports(b3_imports, (int)(sizeof b3_imports / sizeof b3_imports[0]));
}
