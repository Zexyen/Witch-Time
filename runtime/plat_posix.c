/* Linux build only: the Win32 functions plat.h declares (threads and waitable handles, memory regions, fibers, modules,
 * files). Behaviour follows what the runtime's call sites rely on, not all of Win32; the limits are marked "shortcut:". */
#ifndef _WIN32
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "plat.h"
#include "posix/win32_absent.h"
#include "plat_hash.h"                        /* [witchtime] SHA-1 / MD5 of our own: no libcrypto */
#include <dirent.h>
#include <locale.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <linux/fs.h>
#include <linux/userfaultfd.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#ifndef PAGEMAP_SCAN                          /* [witchtime] Linux 6.7's uapi, for older kernel headers (the release builds
                                                 on Ubuntu 22.04); whether the kernel has it is asked at run time */
#define PAGEMAP_SCAN _IOWR('f', 16, struct pm_scan_arg)
#define PAGE_IS_WRITTEN (1 << 1)
#define PM_SCAN_WP_MATCHING (1 << 0)
#define PM_SCAN_CHECK_WPASYNC (1 << 1)
struct page_region { __u64 start, end, categories; };
struct pm_scan_arg { __u64 size, flags, start, end, walk_end, vec, vec_len, max_pages, category_inverted, category_mask,
                     category_anyof_mask, return_mask; };
#endif
#ifndef UFFD_FEATURE_WP_UNPOPULATED
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif

/* [witchtime] the start of this module: the program, or libwitchtime.so when the launcher runs the game in its process */
extern const char __ehdr_start __attribute__((visibility("hidden")));

static int64_t mono_ns(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec; }
static struct timespec ns_to_ts(int64_t ns) { struct timespec ts = { ns / 1000000000, ns % 1000000000 }; return ts; }
static struct timespec realtime_after(DWORD ms) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t ns = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec + (int64_t)ms * 1000000;
    return ns_to_ts(ns);
}

/* ---------------------------------------------------------------- waitable handles */
enum { H_THREAD = 1, H_EVENT, H_SEM, H_TIMER, H_FILE, H_FIND, H_MAP };
typedef struct PH {
    int kind, refs;
    pthread_mutex_t m;
    pthread_cond_t cv;                 /* CLOCK_MONOTONIC */
    int signaled, manual, suspended;
    LONG count, max;
    int64_t due;                       /* timer: monotonic ns, 0 = not set */
    LPTHREAD_START_ROUTINE fn;
    void *arg;
    DWORD code;
    volatile pid_t tid;
    int fd;                            /* file, mapping: the descriptor */
    DIR *dir;                          /* find: the folder being listed and the name pattern */
    char *dir_path, *pattern;
} PH;
#define PSEUDO_THREAD ((HANDLE)(intptr_t)-2)

static PH *ph_new(int kind, int refs) {
    PH *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    h->kind = kind; h->refs = refs;
    pthread_mutex_init(&h->m, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&h->cv, &ca);
    pthread_condattr_destroy(&ca);
    return h;
}
static void ph_release(PH *h) {
    if (__atomic_sub_fetch(&h->refs, 1, __ATOMIC_ACQ_REL)) return;
    pthread_mutex_destroy(&h->m); pthread_cond_destroy(&h->cv); free(h);
}

static void *thread_entry(void *p) {
    PH *h = p;
    h->tid = (pid_t)syscall(SYS_gettid);
    pthread_mutex_lock(&h->m);
    while (h->suspended) pthread_cond_wait(&h->cv, &h->m);
    pthread_mutex_unlock(&h->m);
    DWORD code = h->fn(h->arg);
    pthread_mutex_lock(&h->m);
    h->code = code; h->signaled = 1;
    pthread_cond_broadcast(&h->cv);
    pthread_mutex_unlock(&h->m);
    ph_release(h);
    return NULL;
}
HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *id) {
    (void)sa;
    PH *h = ph_new(H_THREAD, 2);       /* one reference for the handle, one for the running thread */
    if (!h) return NULL;
    h->fn = fn; h->arg = arg; h->suspended = (flags & CREATE_SUSPENDED) != 0;
    pthread_attr_t at;
    pthread_attr_init(&at);
    pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, stack ? stack : (SIZE_T)8 << 20);   /* Windows' default is 1 MB; the guest runs on its own stacks */
    pthread_t th;
    int e = pthread_create(&th, &at, thread_entry, h);
    pthread_attr_destroy(&at);
    if (e) { free(h); errno = e; return NULL; }
    if (id) { while (!h->tid) sched_yield(); *id = (DWORD)h->tid; }
    return h;
}
HANDLE GetCurrentThread(void) { return PSEUDO_THREAD; }
DWORD GetCurrentThreadId(void) { return (DWORD)syscall(SYS_gettid); }
DWORD GetThreadId(HANDLE h) {
    if (h == PSEUDO_THREAD) return GetCurrentThreadId();
    PH *p = h;
    if (!p || p->kind != H_THREAD) return 0;
    while (!p->tid && !p->suspended) sched_yield();   /* just created: its id is a moment away */
    return (DWORD)p->tid;
}
DWORD ResumeThread(HANDLE h) {
    PH *p = h;
    if (!p || h == PSEUDO_THREAD || p->kind != H_THREAD) return (DWORD)-1;
    pthread_mutex_lock(&p->m);
    DWORD was = (DWORD)p->suspended;
    p->suspended = 0;
    pthread_cond_broadcast(&p->cv);
    pthread_mutex_unlock(&p->m);
    return was;
}
/* shortcut: a running thread cannot be suspended (the samplers and watchdogs that do it skip the thread); port with signals if they matter */
DWORD SuspendThread(HANDLE h) { (void)h; errno = ENOTSUP; return (DWORD)-1; }
/* shortcut: priorities are not applied (needs CAP_SYS_NICE or rtkit for anything above normal) */
BOOL SetThreadPriority(HANDLE h, int prio) { (void)h; (void)prio; return TRUE; }
int GetThreadPriority(HANDLE h) { (void)h; return THREAD_PRIORITY_NORMAL; }
BOOL GetExitCodeThread(HANDLE h, DWORD *code) {
    PH *p = h;
    pthread_mutex_lock(&p->m);
    *code = p->signaled ? p->code : 259 /* STILL_ACTIVE */;
    pthread_mutex_unlock(&p->m);
    return TRUE;
}

HANDLE CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, LPCSTR name) {
    (void)sa; (void)name;
    PH *h = ph_new(H_EVENT, 1);
    if (h) { h->manual = manual; h->signaled = initial; }
    return h;
}
BOOL SetEvent(HANDLE h) {
    PH *p = h;
    pthread_mutex_lock(&p->m); p->signaled = 1; pthread_cond_broadcast(&p->cv); pthread_mutex_unlock(&p->m);
    return TRUE;
}
BOOL ResetEvent(HANDLE h) { PH *p = h; pthread_mutex_lock(&p->m); p->signaled = 0; pthread_mutex_unlock(&p->m); return TRUE; }
HANDLE CreateSemaphoreA(SECURITY_ATTRIBUTES *sa, LONG initial, LONG max, LPCSTR name) {
    (void)sa; (void)name;
    PH *h = ph_new(H_SEM, 1);
    if (h) { h->count = initial; h->max = max; }
    return h;
}
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *prev) {
    PH *p = h;
    pthread_mutex_lock(&p->m);
    if (prev) *prev = p->count;
    int ok = n > 0 && p->count <= p->max - n;
    if (ok) { p->count += n; pthread_cond_broadcast(&p->cv); }
    pthread_mutex_unlock(&p->m);
    return ok;
}
HANDLE CreateWaitableTimerExW(SECURITY_ATTRIBUTES *sa, LPCWSTR name, DWORD flags, DWORD access) {
    (void)sa; (void)name; (void)access;
    PH *h = ph_new(H_TIMER, 1);
    if (h) h->manual = (flags & 1) != 0;       /* CREATE_WAITABLE_TIMER_MANUAL_RESET */
    return h;
}
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, LPVOID arg, BOOL resume) {
    (void)fn; (void)arg; (void)resume;
    PH *p = h;
    if (period) return FALSE;                   /* not used by the runtime */
    int64_t ns;
    if (due->QuadPart <= 0) ns = mono_ns() - due->QuadPart * 100;   /* negative: relative, in 100 ns */
    else {                                      /* positive: absolute FILETIME (100 ns since 1601) */
        FILETIME ft; GetSystemTimeAsFileTime(&ft);
        int64_t now = (int64_t)(((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
        ns = mono_ns() + (due->QuadPart - now) * 100;
    }
    pthread_mutex_lock(&p->m); p->due = ns; p->signaled = 0; pthread_cond_broadcast(&p->cv); pthread_mutex_unlock(&p->m);
    return TRUE;
}

static int ph_ready(PH *p) {
    if (p->kind == H_SEM) return p->count > 0;
    if (p->kind == H_TIMER && p->due && mono_ns() >= p->due) { p->signaled = 1; p->due = 0; }
    return p->signaled;
}
static void ph_consume(PH *p) {
    if (p->kind == H_SEM) p->count--;
    else if ((p->kind == H_EVENT || p->kind == H_TIMER) && !p->manual) p->signaled = 0;
}
DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    if (!h || h == INVALID_HANDLE_VALUE || h == PSEUDO_THREAD) return WAIT_FAILED;
    PH *p = h;
    int64_t deadline = ms == INFINITE ? INT64_MAX : mono_ns() + (int64_t)ms * 1000000;
    pthread_mutex_lock(&p->m);
    while (!ph_ready(p)) {
        int64_t until = deadline;
        if (p->kind == H_TIMER && p->due && p->due < until) until = p->due;
        if (until == INT64_MAX) pthread_cond_wait(&p->cv, &p->m);
        else {
            if (mono_ns() >= deadline) { pthread_mutex_unlock(&p->m); return WAIT_TIMEOUT; }
            struct timespec ts = ns_to_ts(until);
            pthread_cond_timedwait(&p->cv, &p->m, &ts);
        }
    }
    ph_consume(p);
    pthread_mutex_unlock(&p->m);
    return WAIT_OBJECT_0;
}
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms) {
    int64_t deadline = ms == INFINITE ? INT64_MAX : mono_ns() + (int64_t)ms * 1000000;
    if (all) {   /* shortcut: waits one after the other, so objects are not taken atomically (the runtime waits for threads only) */
        for (DWORD i = 0; i < n; i++) {
            int64_t left = deadline == INT64_MAX ? -1 : (deadline - mono_ns()) / 1000000;
            if (WaitForSingleObject(h[i], left < 0 ? (deadline == INT64_MAX ? INFINITE : 0) : (DWORD)left) != WAIT_OBJECT_0) return WAIT_TIMEOUT;
        }
        return WAIT_OBJECT_0;
    }
    for (;;) {   /* shortcut: any-of polls every millisecond */
        for (DWORD i = 0; i < n; i++) if (WaitForSingleObject(h[i], 0) == WAIT_OBJECT_0) return WAIT_OBJECT_0 + i;
        if (mono_ns() >= deadline) return WAIT_TIMEOUT;
        Sleep(1);
    }
}
BOOL CloseHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE || h == PSEUDO_THREAD) return FALSE;
    PH *p = h;
    if (p->kind == H_FILE || p->kind == H_MAP) close(p->fd);
    ph_release(h);
    return TRUE;
}

void GetSystemTimeAsFileTime(FILETIME *ft) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 10000000 + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ull;   /* 1601 -> 1970 */
    ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32);
}

/* ---------------------------------------------------------------- critical sections, condition variables, once */
void InitializeCriticalSection(CRITICAL_SECTION *cs) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(cs, &a);
    pthread_mutexattr_destroy(&a);
}
static BOOL cv_wait(CONDITION_VARIABLE *cv, pthread_mutex_t *m, DWORD ms) {
    if (ms == INFINITE) return pthread_cond_wait(cv, m) == 0;
    struct timespec ts = realtime_after(ms);   /* CONDITION_VARIABLE_INIT gives the default clock: CLOCK_REALTIME */
    if (pthread_cond_timedwait(cv, m, &ts) == ETIMEDOUT) { errno = ETIMEDOUT; return FALSE; }
    return TRUE;
}
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms) { return cv_wait(cv, cs, ms); }
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags) { (void)flags; return cv_wait(cv, l, ms); }
static pthread_mutex_t g_once_lock = PTHREAD_MUTEX_INITIALIZER;
BOOL InitOnceExecuteOnce(INIT_ONCE *once, BOOL (*fn)(INIT_ONCE *, PVOID, PVOID *), PVOID param, PVOID *ctx) {
    if (__atomic_load_n(&once->done, __ATOMIC_ACQUIRE)) return TRUE;
    pthread_mutex_lock(&g_once_lock);   /* one lock for every once: they run rarely and briefly */
    BOOL ok = TRUE;
    if (!once->done) { ok = fn(once, param, ctx); if (ok) __atomic_store_n(&once->done, 1, __ATOMIC_RELEASE); }
    pthread_mutex_unlock(&g_once_lock);
    return ok;
}

/* ---------------------------------------------------------------- fibers */
DWORD FlsAlloc(void (*dtor)(PVOID)) { pthread_key_t k; return pthread_key_create(&k, dtor) ? FLS_OUT_OF_INDEXES : (DWORD)k; }
typedef struct Fiber { ucontext_t uc; LPFIBER_START_ROUTINE fn; void *arg, *stack; size_t stack_size; } Fiber;
static __thread Fiber *t_fiber;
static void fiber_entry(void) { t_fiber->fn(t_fiber->arg); _exit(70); }   /* a fiber function never returns (Windows ends the thread) */
LPVOID ConvertThreadToFiber(LPVOID arg) {
    Fiber *f = calloc(1, sizeof *f);
    if (f) { f->arg = arg; t_fiber = f; }
    return f;
}
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID arg) {
    (void)commit; (void)flags;
    Fiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->stack_size = reserve ? reserve : (SIZE_T)1 << 20;
    f->stack = mmap(NULL, f->stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (f->stack == MAP_FAILED) { free(f); return NULL; }
    f->fn = fn; f->arg = arg;
    getcontext(&f->uc);
    f->uc.uc_stack.ss_sp = f->stack;
    f->uc.uc_stack.ss_size = f->stack_size;
    f->uc.uc_link = NULL;
    makecontext(&f->uc, fiber_entry, 0);
    return f;
}
void SwitchToFiber(LPVOID fiber) {
    Fiber *from = t_fiber, *to = fiber;
    t_fiber = to;
    swapcontext(&from->uc, &to->uc);
}
void DeleteFiber(LPVOID fiber) {
    Fiber *f = fiber;
    if (f->stack) munmap(f->stack, f->stack_size);
    free(f);
}

/* ---------------------------------------------------------------- memory */
typedef struct { uintptr_t base; size_t size; int ww; } Region;   /* ww: 0 no write watch, 1 tracked by the kernel, 2 untracked (every page written) */
static Region *g_regions;
static size_t g_nregions, g_cap;
static pthread_mutex_t g_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t page_up(size_t n) { return (n + 4095) & ~(size_t)4095; }
static int to_prot(DWORD p) {
    switch (p & 0xff) {
    case PAGE_READONLY: return PROT_READ;
    case PAGE_READWRITE: return PROT_READ | PROT_WRITE;
    case PAGE_EXECUTE_READ: return PROT_READ | PROT_EXEC;
    case PAGE_EXECUTE_READWRITE: return PROT_READ | PROT_WRITE | PROT_EXEC;
    default: return PROT_NONE;
    }
}
/* Write watch (MEM_WRITE_WATCH, GetWriteWatch): userfaultfd write-protection in async mode, where the kernel itself lifts
 * the protection of a page at its first write (also a write from a system call or a driver) and PAGEMAP_SCAN lists the
 * pages written and protects them again in one call (Linux 6.7+; Wine emulates write watches the same way). Without it
 * (an older kernel, userfaultfd not allowed) a watched region answers "every page written": always correct, only slower. */
static int g_uffd = -1, g_pagemap = -1, g_ww_state;   /* g_ww_state: 0 not tried, 1 on, 2 not available */
static int ww_available(void) {
    pthread_mutex_lock(&g_reg_lock);
    if (!g_ww_state) {
        struct uffdio_api api = { .api = UFFD_API, .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED };
        g_uffd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
        if (g_uffd >= 0 && ioctl(g_uffd, UFFDIO_API, &api) == 0) g_pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
        g_ww_state = g_pagemap >= 0 ? 1 : 2;
        if (g_ww_state == 2) {
            printf("plat: write watch NOT tracked (userfaultfd async write-protect / PAGEMAP_SCAN: %s): every watched page counts as written\n", strerror(errno));
            if (g_uffd >= 0) close(g_uffd);
        } else printf("plat: write watch tracked by the kernel (userfaultfd async write-protect, PAGEMAP_SCAN)\n");
    }
    int on = g_ww_state == 1;
    pthread_mutex_unlock(&g_reg_lock);
    return on;
}
static int ww_protect(uintptr_t a, size_t n) {         /* from now on [a, a+n) counts as not written */
    struct uffdio_writeprotect wp = { .range = { a, n }, .mode = UFFDIO_WRITEPROTECT_MODE_WP };
    return ioctl(g_uffd, UFFDIO_WRITEPROTECT, &wp) == 0;
}
static int ww_of(uintptr_t a, size_t n) {              /* the write watch of the region holding [a, a+n), 0 = none */
    int ww = 0;
    pthread_mutex_lock(&g_reg_lock);
    for (size_t i = 0; i < g_nregions && !ww; i++)
        if (a >= g_regions[i].base && a + n <= g_regions[i].base + g_regions[i].size) ww = g_regions[i].ww;
    pthread_mutex_unlock(&g_reg_lock);
    return ww;
}
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot) {
    size = page_up(size);
    if (!(type & MEM_RESERVE)) {               /* commit inside a reservation */
        uintptr_t a = (uintptr_t)addr & ~(uintptr_t)4095;
        size_t n = page_up((uintptr_t)addr + size - a);
        if (mprotect((void *)a, n, to_prot(prot))) return NULL;
        if (ww_of(a, n) == 1) ww_protect(a, n);  /* newly committed pages are not written (as on Windows) */
        return addr;
    }
    int flags = MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | (addr ? MAP_FIXED_NOREPLACE : 0);
    void *p = mmap(addr, size, (type & MEM_COMMIT) ? to_prot(prot) : PROT_NONE, flags, -1, 0);
    if (p == MAP_FAILED || (addr && p != addr)) { if (p != MAP_FAILED) munmap(p, size); return NULL; }
    int ww = 0;
    if (type & MEM_WRITE_WATCH) {
        struct uffdio_register reg = { .range = { (uintptr_t)p, size }, .mode = UFFDIO_REGISTER_MODE_WP };
        ww = ww_available() && ioctl(g_uffd, UFFDIO_REGISTER, &reg) == 0 && ww_protect((uintptr_t)p, size) ? 1 : 2;
    }
    pthread_mutex_lock(&g_reg_lock);
    if (g_nregions == g_cap) { g_cap = g_cap ? g_cap * 2 : 256; g_regions = realloc(g_regions, g_cap * sizeof *g_regions); }
    g_regions[g_nregions++] = (Region){ (uintptr_t)p, size, ww };
    pthread_mutex_unlock(&g_reg_lock);
    return p;
}
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type) {
    if (type & MEM_DECOMMIT) {                 /* back to reserved: contents dropped, as on Windows */
        uintptr_t a = (uintptr_t)addr & ~(uintptr_t)4095;
        size_t n = page_up((uintptr_t)addr + size - a);
        madvise((void *)a, n, MADV_DONTNEED);
        return mprotect((void *)a, n, PROT_NONE) == 0;
    }
    pthread_mutex_lock(&g_reg_lock);
    for (size_t i = 0; i < g_nregions; i++)
        if (g_regions[i].base == (uintptr_t)addr) {
            Region r = g_regions[i];
            g_regions[i] = g_regions[--g_nregions];
            pthread_mutex_unlock(&g_reg_lock);
            return munmap(addr, r.size) == 0;
        }
    pthread_mutex_unlock(&g_reg_lock);
    errno = EINVAL;
    return FALSE;                              /* not the base of a VirtualAlloc region, as on Windows */
}
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD *old) {
    MEMORY_BASIC_INFORMATION mi;
    if (old) *old = VirtualQuery(addr, &mi, sizeof mi) ? mi.Protect : PAGE_NOACCESS;
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)4095;
    return mprotect((void *)a, page_up((uintptr_t)addr + size - a), to_prot(prot)) == 0;
}
/* shortcut: reads /proc/self/maps on every call (fine for the runtime's diagnostics; cache it if a hot path calls it) */
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mi, SIZE_T len) {
    if (len < sizeof *mi) return 0;
    memset(mi, 0, sizeof *mi);
    uintptr_t a = (uintptr_t)addr, page = a & ~(uintptr_t)4095;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[512], perm[8];
    unsigned long lo, hi, prev_hi = 0;
    int found = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%lx-%lx %4s", &lo, &hi, perm) != 3) continue;
        if (a < lo) { mi->BaseAddress = (void *)page; mi->RegionSize = lo - page; mi->State = MEM_FREE; mi->Protect = PAGE_NOACCESS; found = 1; break; }
        if (a < hi) {
            int r = perm[0] == 'r', w = perm[1] == 'w', x = perm[2] == 'x';
            mi->BaseAddress = (void *)page; mi->AllocationBase = (void *)lo; mi->RegionSize = hi - page;
            mi->Protect = !r ? PAGE_NOACCESS : x ? (w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ) : w ? PAGE_READWRITE : PAGE_READONLY;
            mi->AllocationProtect = mi->Protect;
            mi->State = r ? MEM_COMMIT : MEM_RESERVE;   /* PROT_NONE is how a reservation looks here */
            mi->Type = 0x20000;                         /* MEM_PRIVATE */
            found = 1;
            break;
        }
        prev_hi = hi;
    }
    fclose(f);
    (void)prev_hi;
    if (!found) { mi->BaseAddress = (void *)page; mi->State = MEM_FREE; mi->Protect = PAGE_NOACCESS; mi->RegionSize = 4096; }
    return sizeof *mi;
}
BOOL IsBadReadPtr(const void *p, UINT_PTR n) {
    if (!p) return TRUE;
    MEMORY_BASIC_INFORMATION mi;
    for (uintptr_t a = (uintptr_t)p, end = a + (n ? n : 1); a < end; a = (uintptr_t)mi.BaseAddress + mi.RegionSize)
        if (!VirtualQuery((void *)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || mi.Protect == PAGE_NOACCESS) return TRUE;
    return FALSE;
}
/* The pages of [base, base+size) written since the last reset, in address order, at most *count of them; with
 * WRITE_WATCH_FLAG_RESET the ones listed count as not written again. Fails (non-zero) for memory that is not watched. */
UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addrs, ULONG_PTR *count, ULONG *gran) {
    uintptr_t start = (uintptr_t)base & ~(uintptr_t)4095, end = page_up((uintptr_t)base + size);
    int ww = size ? ww_of(start, end - start) : 0;
    if (!ww) return 1;
    *gran = 4096;
    ULONG_PTR cap = *count, n = 0;
    if (ww == 1) {
        struct page_region vec[256];
        while (start < end && n < cap) {
            struct pm_scan_arg a = { .size = sizeof a, .flags = PM_SCAN_CHECK_WPASYNC | ((flags & WRITE_WATCH_FLAG_RESET) ? PM_SCAN_WP_MATCHING : 0),
                                     .start = start, .end = end, .vec = (uintptr_t)vec, .vec_len = 256, .max_pages = cap - n,
                                     .category_mask = PAGE_IS_WRITTEN, .return_mask = PAGE_IS_WRITTEN };
            long k = ioctl(g_pagemap, PAGEMAP_SCAN, &a);
            if (k < 0) { ww = 2; n = 0; break; }      /* cannot ask: answer as if untracked (everything written) */
            for (long i = 0; i < k; i++)
                for (uint64_t pg = vec[i].start; pg < vec[i].end && n < cap; pg += 4096) addrs[n++] = (void *)(uintptr_t)pg;
            if (a.walk_end <= start) break;
            start = a.walk_end;                       /* the scan stopped early (vec full or max_pages): go on from there */
        }
        if (ww == 1) { *count = n; return 0; }
        start = (uintptr_t)base & ~(uintptr_t)4095;
    }
    for (uintptr_t pg = start; pg < end && n < cap; pg += 4096) addrs[n++] = (void *)pg;   /* untracked: every page */
    *count = n;
    return 0;
}
UINT ResetWriteWatch(LPVOID base, SIZE_T size) {
    uintptr_t a = (uintptr_t)base & ~(uintptr_t)4095, n = page_up((uintptr_t)base + size) - a;
    int ww = size ? ww_of(a, n) : 0;
    if (!ww) return 1;
    return ww == 1 && !ww_protect(a, n);
}
void GetSystemInfo(SYSTEM_INFO *si) {
    memset(si, 0, sizeof *si);
    si->wProcessorArchitecture = 9;            /* PROCESSOR_ARCHITECTURE_AMD64 */
    si->dwPageSize = 4096;
    si->dwAllocationGranularity = 65536;
    si->lpMinimumApplicationAddress = (void *)0x10000;
    si->lpMaximumApplicationAddress = (void *)0x7ffffffeffffull;
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    si->dwNumberOfProcessors = n > 0 ? (DWORD)n : 1;
    si->dwActiveProcessorMask = si->dwNumberOfProcessors >= 64 ? ~(DWORD_PTR)0 : ((DWORD_PTR)1 << si->dwNumberOfProcessors) - 1;
}

/* ---------------------------------------------------------------- modules and process */
HMODULE LoadLibraryA(LPCSTR name) {
    static const char *const map[][2] = {      /* the Windows DLLs the runtime opens by name, and their Linux libraries */
        {"vulkan-1.dll", "libvulkan.so.1"}, {"wt_glslang.dll", "libwt_glslang.so"}, {"SDL3.dll", "libSDL3.so.0"}, {"libopus-0.dll", "libopus.so.0"},
    };
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (!strcasecmp(name, map[i][0])) {
            void *h = dlopen(map[i][1], RTLD_NOW);
            if (!h && strchr(map[i][1], '/') == NULL) {   /* beside the program, like a DLL */
                char path[MAX_PATH + 64];
                DWORD n = GetModuleFileNameA(NULL, path, MAX_PATH);
                char *slash = n ? strrchr(path, '/') : NULL;
                if (slash) { snprintf(slash + 1, sizeof path - (size_t)(slash + 1 - path), "%s", map[i][1]); h = dlopen(path, RTLD_NOW); }
            }
            return h;
        }
    size_t n = strlen(name);
    if (n > 4 && !strcasecmp(name + n - 4, ".dll")) return NULL;   /* a Windows system DLL (gdi32, dwmapi, ...): not here */
    return dlopen(name, RTLD_NOW);
}
void *GetProcAddress(HMODULE m, LPCSTR name) { return dlsym(!m || m == (HMODULE)&__ehdr_start ? RTLD_DEFAULT : m, name); }
HMODULE GetModuleHandleA(LPCSTR name) { return name ? NULL : (HMODULE)&__ehdr_start; }
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n) {
    (void)m;   /* the runtime asks for its own program only */
    ssize_t len = readlink("/proc/self/exe", out, n ? n - 1 : 0);
    if (len < 0) { if (n) out[0] = 0; return 0; }
    out[len] = 0;
    return (DWORD)len;
}
/* [witchtime] ExitProcess (plat.h). A host that runs the game in its own process (launcher/, through libwitchtime.so) sets
 * the hook to see the exit first: it goes back to its own screens instead of ending with the game. */
void (*plat_exit_hook)(int code);
void plat_exit_process(int code) {
    fflush(NULL);
    if (plat_exit_hook) plat_exit_hook(code);
    _exit(code);
}
/* the runtime raises only before report_and_exit, for a debugger to stop at: nothing to do without one */
void RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args) { (void)code; (void)flags; (void)n; (void)args; }

/* ---------------------------------------------------------------- files: Windows paths (plat_path) */
#undef fopen
const char *plat_path(const char *in, char *out, size_t n) {
    snprintf(out, n, "%s", in);
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
    if (!out[0] || access(out, F_OK) == 0) return out;
    /* not there as written: walk it, and replace each component that is missing by the one that matches without case */
    char done[4096];
    size_t len = 0;
    const char *p = out;
    if (*p == '/') { done[len++] = '/'; while (*p == '/') p++; }
    done[len] = 0;
    while (*p) {
        const char *end = strchr(p, '/');
        size_t k = end ? (size_t)(end - p) : strlen(p);
        char comp[256];
        snprintf(comp, sizeof comp, "%.*s", (int)k, p);
        char probe[4096];
        snprintf(probe, sizeof probe, "%s%s%s", done, len && done[len - 1] != '/' ? "/" : "", comp);
        if (access(probe, F_OK) != 0 && strcmp(comp, ".") && strcmp(comp, "..")) {
            DIR *d = opendir(len ? done : ".");
            struct dirent *e;
            while (d && (e = readdir(d))) if (!strcasecmp(e->d_name, comp)) { snprintf(comp, sizeof comp, "%s", e->d_name); break; }
            if (d) closedir(d);
            snprintf(probe, sizeof probe, "%s%s%s", done, len && done[len - 1] != '/' ? "/" : "", comp);
        }
        snprintf(done, sizeof done, "%s", probe);
        len = strlen(done);
        p += k;
        while (*p == '/') p++;
    }
    snprintf(out, n, "%s", done);   /* a path that still does not exist stays as written, with the parts that matched fixed */
    return out;
}
FILE *plat_fopen(const char *path, const char *mode) {
    char p[4096];
    return fopen(plat_path(path, p, sizeof p), mode);
}
#define PATH(var, in) char var##_buf[4096]; const char *var = plat_path((in), var##_buf, sizeof var##_buf)
BOOL CreateDirectoryA(LPCSTR path, SECURITY_ATTRIBUTES *sa) { (void)sa; PATH(p, path); return mkdir(p, 0755) == 0; }
BOOL RemoveDirectoryA(LPCSTR path) { PATH(p, path); return rmdir(p) == 0; }
BOOL DeleteFileA(LPCSTR path) { PATH(p, path); return unlink(p) == 0; }
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags) {
    PATH(a, from); PATH(b, to);
    if (!(flags & MOVEFILE_REPLACE_EXISTING) && access(b, F_OK) == 0) { errno = EEXIST; return FALSE; }
    return rename(a, b) == 0;
}
static void ft_from(FILETIME *ft, struct timespec ts) {
    uint64_t t = (uint64_t)ts.tv_sec * 10000000 + (uint64_t)ts.tv_nsec / 100 + 116444736000000000ull;
    ft->dwLowDateTime = (DWORD)t; ft->dwHighDateTime = (DWORD)(t >> 32);
}
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, void *out) {
    (void)level;
    PATH(p, path);
    struct stat st;
    if (stat(p, &st)) return FALSE;
    WIN32_FILE_ATTRIBUTE_DATA *d = out;
    memset(d, 0, sizeof *d);
    d->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : 0x80 /* NORMAL */;
    ft_from(&d->ftLastWriteTime, st.st_mtim); ft_from(&d->ftLastAccessTime, st.st_atim); ft_from(&d->ftCreationTime, st.st_ctim);
    d->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); d->nFileSizeLow = (DWORD)st.st_size;
    return TRUE;
}
DWORD GetTempPathA(DWORD n, LPSTR out) {
    const char *t = getenv("TMPDIR");
    return (DWORD)snprintf(out, n, "%s/", t && *t ? t : "/tmp");
}
char *_fullpath(char *out, const char *path, size_t n) {
    PATH(p, path);
    if (p[0] == '/') snprintf(out, n, "%s", p);
    else { char cwd[4096]; if (!getcwd(cwd, sizeof cwd)) return NULL; snprintf(out, n, "%s/%s", cwd, p); }
    return out;
}

/* ---------------------------------------------------------------- file handles */
static DWORD err_from_errno(int e) {
    switch (e) {
    case ENOENT: return ERROR_FILE_NOT_FOUND;
    case ENOTDIR: return ERROR_PATH_NOT_FOUND;
    case EACCES: case EPERM: return ERROR_ACCESS_DENIED;
    case EEXIST: return ERROR_ALREADY_EXISTS;
    default: return (DWORD)e;
    }
}
static __thread DWORD t_last_error;
#undef GetLastError
DWORD GetLastError(void) { return t_last_error ? t_last_error : err_from_errno(errno); }
static void fail_errno(void) { t_last_error = err_from_errno(errno); }
static void ok_error(void) { t_last_error = 0; errno = 0; }

HANDLE CreateFileA(LPCSTR path, DWORD acc, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    (void)share; (void)sa; (void)tmpl;
    PATH(p, path);
    int o = (acc & GENERIC_WRITE) ? ((acc & GENERIC_READ) ? O_RDWR : O_WRONLY) : O_RDONLY;
    switch (disposition) {
    case CREATE_NEW: o |= O_CREAT | O_EXCL; break;
    case CREATE_ALWAYS: o |= O_CREAT | O_TRUNC; break;
    case OPEN_ALWAYS: o |= O_CREAT; break;
    case TRUNCATE_EXISTING: o |= O_TRUNC; break;
    default: break;                            /* OPEN_EXISTING */
    }
    if (flags & FILE_FLAG_WRITE_THROUGH) o |= O_DSYNC;
    int existed = disposition == CREATE_ALWAYS || disposition == OPEN_ALWAYS ? access(p, F_OK) == 0 : 0;
    int fd = open(p, o | O_CLOEXEC, 0644);
    if (fd < 0) { fail_errno(); return INVALID_HANDLE_VALUE; }
    struct stat st;
    if (!fstat(fd, &st) && S_ISDIR(st.st_mode)) { close(fd); errno = EISDIR; fail_errno(); return INVALID_HANDLE_VALUE; }
    PH *h = ph_new(H_FILE, 1);
    if (!h) { close(fd); return INVALID_HANDLE_VALUE; }
    h->fd = fd;
    if (existed) t_last_error = ERROR_ALREADY_EXISTS; else ok_error();   /* CREATE_ALWAYS / OPEN_ALWAYS say whether it was there */
    return h;
}
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    char p[4096];
    if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, p, sizeof p, NULL, NULL)) return INVALID_HANDLE_VALUE;
    return CreateFileA(p, access, share, sa, disposition, flags, tmpl);
}
static int file_fd(HANDLE h) { PH *p = h; return h && h != INVALID_HANDLE_VALUE && (p->kind == H_FILE || p->kind == H_MAP) ? p->fd : -1; }
int _get_osfhandle_fd(HANDLE h) { return file_fd(h); }
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *got, OVERLAPPED *ov) {
    int fd = file_fd(h);
    if (got) *got = 0;
    size_t done = 0;
    off_t off = ov ? (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset) : 0;
    while (done < n) {   /* like ReadFile on a disk file: everything up to the end, short only at the end */
        ssize_t r = ov ? pread(fd, (uint8_t *)buf + done, n - done, off + (off_t)done) : read(fd, (uint8_t *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; fail_errno(); if (got) *got = (DWORD)done; return FALSE; }
        if (r == 0) break;
        done += (size_t)r;
    }
    if (got) *got = (DWORD)done;
    if (ov) ov->InternalHigh = done;
    return TRUE;
}
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *put, OVERLAPPED *ov) {
    int fd = file_fd(h);
    if (put) *put = 0;
    size_t done = 0;
    off_t off = ov ? (off_t)(((uint64_t)ov->OffsetHigh << 32) | ov->Offset) : 0;
    while (done < n) {
        ssize_t r = ov ? pwrite(fd, (const uint8_t *)buf + done, n - done, off + (off_t)done) : write(fd, (const uint8_t *)buf + done, n - done);
        if (r < 0) { if (errno == EINTR) continue; fail_errno(); if (put) *put = (DWORD)done; return FALSE; }
        done += (size_t)r;
    }
    if (put) *put = (DWORD)done;
    return TRUE;
}
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *pos, DWORD how) {
    off_t r = lseek(file_fd(h), (off_t)to.QuadPart, how == FILE_END ? SEEK_END : how == FILE_CURRENT ? SEEK_CUR : SEEK_SET);
    if (r < 0) { fail_errno(); return FALSE; }
    if (pos) pos->QuadPart = r;
    return TRUE;
}
BOOL SetEndOfFile(HANDLE h) {
    int fd = file_fd(h);
    off_t at = lseek(fd, 0, SEEK_CUR);
    if (at < 0 || ftruncate(fd, at)) { fail_errno(); return FALSE; }
    return TRUE;
}
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size) {
    struct stat st;
    if (fstat(file_fd(h), &st)) { fail_errno(); return FALSE; }
    size->QuadPart = st.st_size;
    return TRUE;
}
BOOL FlushFileBuffers(HANDLE h) { return fsync(file_fd(h)) == 0; }
DWORD GetFileAttributesA(LPCSTR path) {
    PATH(p, path);
    struct stat st;
    if (stat(p, &st)) { fail_errno(); return INVALID_FILE_ATTRIBUTES; }
    return S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : (st.st_mode & S_IWUSR) ? FILE_ATTRIBUTE_NORMAL : FILE_ATTRIBUTE_READONLY;
}
BOOL SetFileAttributesA(LPCSTR path, DWORD attr) {
    PATH(p, path);
    struct stat st;
    if (stat(p, &st)) { fail_errno(); return FALSE; }
    mode_t m = (attr & FILE_ATTRIBUTE_READONLY) ? st.st_mode & ~(mode_t)0222 : st.st_mode | S_IWUSR;
    return chmod(p, m) == 0;
}

/* ---------------------------------------------------------------- directory listing: FindFirstFile over opendir */
static int find_next(PH *h, WIN32_FIND_DATAA *fd) {
    struct dirent *e;
    while ((e = readdir(h->dir))) {
        if (fnmatch(h->pattern, e->d_name, FNM_CASEFOLD)) continue;   /* Windows names match case-insensitively */
        char full[4096];
        snprintf(full, sizeof full, "%s/%s", h->dir_path, e->d_name);
        struct stat st;
        if (stat(full, &st)) continue;         /* a dangling link */
        memset(fd, 0, sizeof *fd);
        fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        ft_from(&fd->ftLastWriteTime, st.st_mtim); ft_from(&fd->ftLastAccessTime, st.st_atim); ft_from(&fd->ftCreationTime, st.st_ctim);
        fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32); fd->nFileSizeLow = (DWORD)st.st_size;
        snprintf(fd->cFileName, sizeof fd->cFileName, "%s", e->d_name);
        return 1;
    }
    errno = 0; t_last_error = ERROR_NO_MORE_FILES;
    return 0;
}
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd) {
    PATH(p, pattern);
    const char *slash = strrchr(p, '/');
    char dir[4096];
    snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - p) : 1, slash ? p : ".");
    if (!dir[0]) snprintf(dir, sizeof dir, "/");
    PH *h = ph_new(H_FIND, 1);
    if (!h) return INVALID_HANDLE_VALUE;
    h->dir = opendir(dir);
    if (!h->dir) { fail_errno(); ph_release(h); return INVALID_HANDLE_VALUE; }
    h->dir_path = strdup(dir);
    h->pattern = strdup(slash ? slash + 1 : p);
    if (!strcmp(h->pattern, "*.*")) h->pattern[1] = 0;   /* "*.*" is everything on Windows, names without a dot too */
    if (!find_next(h, fd)) { FindClose(h); t_last_error = ERROR_FILE_NOT_FOUND; return INVALID_HANDLE_VALUE; }
    return h;
}
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd) { return find_next(h, fd); }
static void find_w(const WIN32_FIND_DATAA *a, WIN32_FIND_DATAW *w) {
    memset(w, 0, sizeof *w);
    w->dwFileAttributes = a->dwFileAttributes;
    w->ftCreationTime = a->ftCreationTime; w->ftLastAccessTime = a->ftLastAccessTime; w->ftLastWriteTime = a->ftLastWriteTime;
    w->nFileSizeHigh = a->nFileSizeHigh; w->nFileSizeLow = a->nFileSizeLow;
    MultiByteToWideChar(CP_UTF8, 0, a->cFileName, -1, w->cFileName, MAX_PATH);
}
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd) {
    char p[4096];
    WIN32_FIND_DATAA a;
    if (!WideCharToMultiByte(CP_UTF8, 0, pattern, -1, p, sizeof p, NULL, NULL)) return INVALID_HANDLE_VALUE;
    HANDLE h = FindFirstFileA(p, &a);
    if (h != INVALID_HANDLE_VALUE) find_w(&a, fd);
    return h;
}
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd) {
    WIN32_FIND_DATAA a;
    if (!find_next(h, &a)) return FALSE;
    find_w(&a, fd);
    return TRUE;
}
BOOL FindClose(HANDLE h) {
    PH *p = h;
    if (!p || h == INVALID_HANDLE_VALUE || p->kind != H_FIND) return FALSE;
    closedir(p->dir); free(p->dir_path); free(p->pattern);
    ph_release(p);
    return TRUE;
}

/* ---------------------------------------------------------------- text: every code page is UTF-8 here, wide is UTF-32 */
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR in, int n, WCHAR *out, int cap) {
    (void)cp; (void)flags;
    size_t len = n < 0 ? strlen(in) + 1 : (size_t)n;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    int count = 0;
    for (size_t i = 0; i < len;) {
        wchar_t wc;
        size_t k = in[i] ? mbrtowc(&wc, in + i, len - i, &st) : (wc = 0, 1);
        if (k == (size_t)-1 || k == (size_t)-2) { wc = 0xFFFD; k = 1; memset(&st, 0, sizeof st); }
        if (cap) { if (count >= cap) return 0; out[count] = wc; }
        count++;
        i += k;
    }
    return count;
}
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR in, int n, LPSTR out, int cap, LPCSTR dflt, BOOL *used) {
    (void)cp; (void)flags; (void)dflt;
    if (used) *used = FALSE;
    size_t len = n < 0 ? wcslen(in) + 1 : (size_t)n;
    mbstate_t st;
    memset(&st, 0, sizeof st);
    int count = 0;
    for (size_t i = 0; i < len; i++) {
        char buf[8];
        size_t k = in[i] ? wcrtomb(buf, in[i], &st) : (buf[0] = 0, 1);
        if (k == (size_t)-1) { buf[0] = '?'; k = 1; if (used) *used = TRUE; }
        if (cap) { if (count + (int)k > cap) return 0; memcpy(out + count, buf, k); }
        count += (int)k;
    }
    return count;
}
__attribute__((constructor)) static void utf8_locale(void) { if (!setlocale(LC_CTYPE, "C.UTF-8")) setlocale(LC_CTYPE, ""); }

/* ---------------------------------------------------------------- file mappings (read-only) */
HANDLE CreateFileMappingA(HANDLE file, SECURITY_ATTRIBUTES *sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name) {
    (void)sa; (void)prot; (void)hi; (void)lo; (void)name;
    int fd = dup(file_fd(file));
    if (fd < 0) { fail_errno(); return NULL; }
    PH *h = ph_new(H_MAP, 1);
    if (!h) { close(fd); return NULL; }
    h->fd = fd;
    return h;
}
typedef struct { void *base; size_t len; } View;
static View g_views[256];
static pthread_mutex_t g_view_lock = PTHREAD_MUTEX_INITIALIZER;
LPVOID MapViewOfFile(HANDLE map, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T n) {
    (void)access;
    uint64_t off = ((uint64_t)off_hi << 32) | off_lo, at = off & ~(uint64_t)4095;
    if (!n) { struct stat st; if (fstat(file_fd(map), &st)) return NULL; n = (SIZE_T)((uint64_t)st.st_size - off); }
    size_t len = (size_t)(off - at) + n;
    uint8_t *p = mmap(NULL, len, PROT_READ, MAP_SHARED, file_fd(map), (off_t)at);   /* Windows needs 64 KB-aligned offsets; here any works */
    if (p == MAP_FAILED) { fail_errno(); return NULL; }
    pthread_mutex_lock(&g_view_lock);
    for (int i = 0; i < 256; i++) if (!g_views[i].base) { g_views[i] = (View){ p, len }; break; }   /* shortcut: 256 views at once */
    pthread_mutex_unlock(&g_view_lock);
    return p + (off - at);
}
BOOL UnmapViewOfFile(LPCVOID v) {
    uintptr_t a = (uintptr_t)v & ~(uintptr_t)4095;
    pthread_mutex_lock(&g_view_lock);
    for (int i = 0; i < 256; i++)
        if (g_views[i].base && (uintptr_t)g_views[i].base <= a && a < (uintptr_t)g_views[i].base + g_views[i].len) {
            View w = g_views[i];
            g_views[i].base = NULL;
            pthread_mutex_unlock(&g_view_lock);
            return munmap(w.base, w.len) == 0;
        }
    pthread_mutex_unlock(&g_view_lock);
    return FALSE;
}

/* ---------------------------------------------------------------- time, process */
static void to_systemtime(const struct tm *t, long ns, SYSTEMTIME *st) {
    st->wYear = (WORD)(t->tm_year + 1900); st->wMonth = (WORD)(t->tm_mon + 1); st->wDayOfWeek = (WORD)t->tm_wday; st->wDay = (WORD)t->tm_mday;
    st->wHour = (WORD)t->tm_hour; st->wMinute = (WORD)t->tm_min; st->wSecond = (WORD)t->tm_sec; st->wMilliseconds = (WORD)(ns / 1000000);
}
void GetLocalTime(SYSTEMTIME *st) { struct timespec ts; struct tm t; clock_gettime(CLOCK_REALTIME, &ts); localtime_r(&ts.tv_sec, &t); to_systemtime(&t, ts.tv_nsec, st); }
void GetSystemTime(SYSTEMTIME *st) { struct timespec ts; struct tm t; clock_gettime(CLOCK_REALTIME, &ts); gmtime_r(&ts.tv_sec, &t); to_systemtime(&t, ts.tv_nsec, st); }
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys) {
    (void)p;
    cpu_set_t set;
    *proc = *sys = 0;
    if (sched_getaffinity(0, sizeof set, &set)) return FALSE;
    for (int i = 0; i < 64; i++) if (CPU_ISSET(i, &set)) *proc |= (DWORD_PTR)1 << i;
    *sys = *proc;
    return TRUE;
}
BOOLEAN SystemFunction036(PVOID buf, ULONG n) {
    for (ULONG done = 0; done < n;) {
        ssize_t r = getrandom((uint8_t *)buf + done, n - done, 0);
        if (r < 0) { if (errno == EINTR) continue; return FALSE; }
        done += (ULONG)r;
    }
    return TRUE;
}
/* __argc / __argv as the MSVC and mingw C runtimes have them: glibc hands every constructor the program's arguments */
int __argc;
char **__argv;
__attribute__((constructor)) static void save_args(int argc, char **argv) { __argc = argc; __argv = argv; }

/* ---------------------------------------------------------------- more handles and files */
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name_or_addr, HMODULE *out) {
    *out = NULL;
    if (!(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS)) { *out = GetModuleHandleA(name_or_addr); return *out != NULL; }
    Dl_info di;
    if (!dladdr(name_or_addr, &di) || !di.dli_fname) return FALSE;
    void *h = dlopen(di.dli_fname, RTLD_NOW | RTLD_NOLOAD);
    if (h) dlclose(h);                         /* UNCHANGED_REFCOUNT: the module stays loaded by whoever loaded it */
    *out = h;
    return h != NULL;
}
HANDLE CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL owned, LPCSTR name) {
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static struct { char name[128]; PH *h; } named[16];
    PH *h = NULL;
    pthread_mutex_lock(&lock);
    for (int i = 0; name && i < 16 && !h; i++)
        if (named[i].h && !strcmp(named[i].name, name)) { h = named[i].h; __atomic_add_fetch(&h->refs, 1, __ATOMIC_ACQ_REL); }
    if (!h) {
        h = (PH *)CreateSemaphoreA(sa, 1, 1, NULL);   /* a mutex nobody takes twice from one thread: a semaphore of 1 */
        for (int i = 0; name && h && i < 16; i++) if (!named[i].h) { snprintf(named[i].name, sizeof named[i].name, "%s", name); named[i].h = h; h->refs++; break; }
    }
    pthread_mutex_unlock(&lock);
    if (h && owned) WaitForSingleObject(h, INFINITE);
    return h;
}
BOOL ReleaseMutex(HANDLE h) { return ReleaseSemaphore(h, 1, NULL); }
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists) {
    PATH(a, from); PATH(b, to);
    int in = open(a, O_RDONLY | O_CLOEXEC);
    if (in < 0) { fail_errno(); return FALSE; }
    int out = open(b, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | (fail_if_exists ? O_EXCL : 0), 0644);
    if (out < 0) { fail_errno(); close(in); return FALSE; }
    char buf[1 << 16];
    ssize_t n;
    int ok = 1;
    while (ok && (n = read(in, buf, sizeof buf)) != 0) {
        if (n < 0) { if (errno == EINTR) continue; ok = 0; break; }
        for (ssize_t done = 0; ok && done < n;) { ssize_t w = write(out, buf + done, (size_t)(n - done)); if (w < 0 && errno != EINTR) ok = 0; else if (w > 0) done += w; }
    }
    if (!ok) fail_errno();
    close(in);
    if (close(out)) ok = 0;
    if (!ok) unlink(b);                        /* no half copy left behind */
    return ok;
}
BOOL GetUserNameW(WCHAR *out, DWORD *n) {
    const char *u = getenv("USER");
    if (!u || !*u) u = getlogin();
    if (!u || !*u) return FALSE;
    int k = MultiByteToWideChar(CP_UTF8, 0, u, -1, out, (int)*n);
    if (!k) return FALSE;
    *n = (DWORD)k;
    return TRUE;
}
HANDLE plat_fd_handle(int fd) {
    static __thread PH h;                      /* never released: kind H_FILE without an owner */
    h.kind = H_FILE; h.refs = 1 << 30; h.fd = fd;
    return &h;
}

/* ---------------------------------------------------------------- CryptoAPI subset: SHA-1 / MD5 (plat_hash.h), random (getrandom) */
BOOL CryptAcquireContextA(HCRYPTPROV *p, LPCSTR c, LPCSTR prov, DWORD type, DWORD flags) { (void)c; (void)prov; (void)type; (void)flags; *p = 1; return TRUE; }
BOOL CryptReleaseContext(HCRYPTPROV p, DWORD flags) { (void)p; (void)flags; return TRUE; }
BOOL CryptGenRandom(HCRYPTPROV p, DWORD n, BYTE *out) { (void)p; return SystemFunction036(out, n); }
BOOL CryptCreateHash(HCRYPTPROV p, UINT alg, HCRYPTPROV key, DWORD flags, HCRYPTHASH *h) {
    (void)p; (void)key; (void)flags;
    PlatHash *ph = alg == CALG_SHA1 || alg == CALG_MD5 ? malloc(sizeof *ph) : NULL;
    if (!ph) { *h = 0; return FALSE; }
    ph_init(ph, alg == CALG_SHA1);
    *h = (HCRYPTHASH)ph;
    return TRUE;
}
BOOL CryptHashData(HCRYPTHASH h, const BYTE *data, DWORD n, DWORD flags) { (void)flags; ph_update((PlatHash *)h, data, n); return TRUE; }
BOOL CryptGetHashParam(HCRYPTHASH h, DWORD param, BYTE *out, DWORD *n, DWORD flags) {
    (void)flags;
    PlatHash *ph = (PlatHash *)h;
    unsigned size = ph->sha1 ? 20 : 16;
    if (param != HP_HASHVAL || *n < size) { *n = size; return FALSE; }
    *n = ph_final(ph, out);                    /* CryptoAPI may be asked again: ph_final leaves the state as it is */
    return TRUE;
}
BOOL CryptDestroyHash(HCRYPTHASH h) { free((PlatHash *)h); return TRUE; }
#endif
