/* [witchtime] Linux sampling profiler of every thread. TEST ONLY; nothing runs without the variable.
 *   WT_LPROF=<seconds from start>,<seconds sampled>[,<output file>]   (default output: lprof.txt in the working folder)
 * A thread of its own sends a real-time signal to each thread of the process in turn (about 4,000 samples a second in all); the
 * handler stores the interrupted PC and a short host call chain (backtrace(); it ends where unwind tables end, e.g. in
 * lifted code). The file lists per sample: thread id, the thread's guest name or "-", the PCs innermost first; then the
 * presented frame ("@<frame>", last), and the mappings (/proc/self/maps), so scripts/lprof_report.py can turn addresses into function names (nm of witch-time-game and
 * game.so; lifted functions are f_<guest address>). Included by boot.c (Linux only), like prof15.c. */
#include <execinfo.h>
#include <signal.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <ucontext.h>
#include <sched.h>
#include <time.h>

#define LP_SIG (SIGRTMIN + 2)                    /* not SIGPROF: SDL3 threads (and the threads they start) block SIGPROF / SIGALRM */
#define LP_DEPTH 12
#define LP_MAX (1 << 22)                        /* 4M samples (about 420 MB): 150 s of 50 threads */
typedef struct { int tid, n, frame; void *pc[LP_DEPTH]; } LpSample;
extern volatile LONG64 g_prof15_frame;            /* display.c: presented frames */
static LpSample *g_lp;
static volatile long g_lp_n;
static volatile int g_lp_ack;

static void lp_signal(int sig, siginfo_t *si, void *ucv) {
    (void)sig; (void)si;
    long i = __atomic_fetch_add(&g_lp_n, 1, __ATOMIC_RELAXED);
    if (i < LP_MAX) {
        LpSample *s = &g_lp[i];
        void *tmp[LP_DEPTH + 3];
        int k = backtrace(tmp, LP_DEPTH + 3), j = 0;
        s->tid = (int)syscall(SYS_gettid); s->frame = (int)g_prof15_frame;
        s->pc[j++] = (void *)((ucontext_t *)ucv)->uc_mcontext.gregs[REG_RIP];
        for (int m = 2; m < k && j < LP_DEPTH; m++) s->pc[j++] = tmp[m];      /* 0 = this handler, 1 = the signal return trampoline */
        s->n = j;
    }
    __atomic_store_n(&g_lp_ack, 1, __ATOMIC_RELEASE);
}

static DWORD WINAPI lp_thread(LPVOID arg) {
    const char *spec = (const char *)arg;
    double start = 0, secs = 0;
    char out[512] = "lprof.txt";
    if (sscanf(spec, "%lf,%lf", &start, &secs) != 2 || secs <= 0) { printf("lprof: WT_LPROF=<start s>,<seconds>[,<file>]\n"); return 0; }
    const char *c2 = strchr(strchr(spec, ',') + 1, ',');
    if (c2) snprintf(out, sizeof out, "%s", c2 + 1);
    g_lp = calloc(LP_MAX, sizeof *g_lp);
    struct sigaction sa = { .sa_sigaction = lp_signal, .sa_flags = SA_SIGINFO | SA_RESTART };
    sigemptyset(&sa.sa_mask);
    sigaction(LP_SIG, &sa, NULL);
    Sleep((DWORD)(start * 1000));
    long self = (long)syscall(SYS_gettid);
    printf("lprof: sampling every thread for %.0f s\n", secs); fflush(stdout);
    ULONGLONG t0 = GetTickCount64(), rounds = 0;
    int tids[512], miss[512], nt = 0;
    while (GetTickCount64() - t0 < (ULONGLONG)(secs * 1000) && g_lp_n < LP_MAX) {
        if (rounds++ % 64 == 0) {           /* the thread list, now and then */
            DIR *d = opendir("/proc/self/task");
            struct dirent *e;
            nt = 0;
            while (d && (e = readdir(d)) && nt < 512) { long t = atol(e->d_name); if (t > 0 && t != self) { miss[nt] = 0; tids[nt++] = (int)t; } }
            if (d) closedir(d);
        }
        for (int i = 0; i < nt; i++) {
            if (miss[i] >= 3) continue;     /* a thread that blocks the signal (driver threads): left out */
            g_lp_ack = 0;
            if (syscall(SYS_tgkill, (long)getpid(), (long)tids[i], LP_SIG) != 0) { miss[i] = 3; continue; }
            struct timespec a, b;
            clock_gettime(CLOCK_MONOTONIC, &a);
            for (;;) {
                if (__atomic_load_n(&g_lp_ack, __ATOMIC_ACQUIRE)) { miss[i] = 0; break; }
                clock_gettime(CLOCK_MONOTONIC, &b);
                if ((b.tv_sec - a.tv_sec) * 1000000000L + (b.tv_nsec - a.tv_nsec) > 300000) { miss[i]++; break; }
                sched_yield();
            }
        }
        usleep(2000);                       /* about 500 rounds a second, each thread once a round */
    }
    long n = g_lp_n < LP_MAX ? g_lp_n : LP_MAX;
    FILE *f = fopen(out, "w");
    if (!f) { printf("lprof: cannot write %s\n", out); return 0; }
    const char *guest_thread_name_of(DWORD tid);
    fprintf(f, "# lprof %.1f s, %ld samples, %llu rounds\n", secs, n, (unsigned long long)rounds);
    for (long i = 0; i < n; i++) {
        const char *nm = guest_thread_name_of((DWORD)g_lp[i].tid);
        char nb[64]; snprintf(nb, sizeof nb, "%s", nm && *nm ? nm : "-");
        for (char *q = nb; *q; q++) if (*q == ' ') *q = '_';
        fprintf(f, "S %d %s", g_lp[i].tid, nb);
        for (int j = 0; j < g_lp[i].n; j++) fprintf(f, " %llx", (unsigned long long)(uintptr_t)g_lp[i].pc[j]);
        fprintf(f, " @%d\n", g_lp[i].frame);
    }
    FILE *m = fopen("/proc/self/maps", "r");
    char line[600];
    while (m && fgets(line, sizeof line, m)) fprintf(f, "M %s", line);
    if (m) fclose(m);
    fclose(f);
    printf("lprof: %ld samples written to %s\n", n, out); fflush(stdout);
    return 0;
}
void lprof_start(void) {
    const char *spec = getenv("WT_LPROF");
    if (spec && *spec) CloseHandle(CreateThread(NULL, 0, lp_thread, (LPVOID)spec, 0, NULL));
}
