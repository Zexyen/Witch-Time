/* runtime/plat_posix.c, the write watch by page faults (ww = 3): what GetWriteWatch answers; a block opens without touching
 * uncommitted pages; a write to one stays a crash; a read from a file into a held page; and two threads against the reset
 * of the render thread: no crash when a thread loses the race for a page twice in a row, and no write lost.
 *   clang -O2 -Wall -I runtime -I runtime/posix tests/plat_ww_test.c runtime/plat_posix.c -lm -ldl -pthread -o build/plat_ww_test */
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "plat.h"
#include "posix/win32_absent.h"

static void on_segv(int sig, siginfo_t *si, void *uc) {
    (void)sig; (void)uc;
    if (!plat_ww_fault((uintptr_t)si->si_addr)) _exit(3);
}
static ULONG_PTR written(void *p, size_t n, int reset) {
    static PVOID pages[4096];
    ULONG_PTR count = n >> 12; ULONG gran;
    if (GetWriteWatch(reset ? WRITE_WATCH_FLAG_RESET : 0, p, n, pages, &count, &gran)) return ~(ULONG_PTR)0;
    return count;
}
static volatile int g_stop;
static volatile uint64_t *g_word;
static void *writer(void *arg) { (void)arg; while (!g_stop) __atomic_add_fetch(g_word, 1, __ATOMIC_RELAXED); return NULL; }

#define CHECK(c) do { if (!(c)) { printf("plat_ww_test: FAILED line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void) {
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    setenv("WT_NO_UFFD", "1", 1);
    size_t size = 1 << 20;
    volatile uint8_t *p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_WRITE_WATCH, PAGE_NOACCESS);
    CHECK(p);
    CHECK(VirtualAlloc((void *)p, 62 * 4096, MEM_COMMIT, PAGE_READWRITE));
    CHECK(written((void *)p, size, 1) == 0);
    p[5 * 4096] = 1;                               /* its 64 KB block opens */
    CHECK(p[5 * 4096] == 1 && written((void *)p, size, 0) == 16);
    CHECK(written((void *)p, size, 1) == 16 && written((void *)p, size, 1) == 0);
    CHECK(p[5 * 4096] == 1 && written((void *)p, size, 1) == 0);      /* a read is not a write */
    p[61 * 4096] = 2;                              /* the last block has 14 committed pages */
    CHECK(written((void *)p, size, 1) == 14);
    pid_t child = fork();
    if (!child) { p[62 * 4096] = 3; _exit(0); }    /* not committed: the block did not open it */
    int st; waitpid(child, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 3);

    char path[] = "/tmp/plat_ww_testXXXXXX"; int fd = mkstemp(path); uint8_t data[9000];
    memset(data, 0x5a, sizeof data);
    CHECK(write(fd, data, sizeof data) == (ssize_t)sizeof data); close(fd);
    HANDLE h = CreateFileA(path, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL); DWORD got = 0;
    CHECK(ReadFile(h, (void *)(p + 0x10000), sizeof data, &got, NULL) && got == sizeof data);      /* a system call into held pages */
    CloseHandle(h); unlink(path);
    CHECK(p[0x10000 + 8999] == 0x5a && written((void *)p, size, 1) >= 3);

    CHECK(VirtualFree((void *)p, 16 * 4096, MEM_DECOMMIT));
    CHECK(VirtualAlloc((void *)p, 16 * 4096, MEM_COMMIT, PAGE_READWRITE));
    CHECK(written((void *)p, size, 1) == 0);

    g_word = (volatile uint64_t *)(p + 0x30000);
    pthread_t t[4];
    for (int i = 0; i < 4; i++) pthread_create(&t[i], NULL, writer, NULL);
    for (int i = 0; i < 200000; i++) {             /* a write between a question and the next is in the next answer */
        uint64_t before = *g_word;
        for (volatile int d = 0; d < i % 64; d++) { }
        uint64_t after = *g_word;
        ULONG_PTR n = written((void *)p, size, 1);
        CHECK(after == before || n);
    }
    g_stop = 1;
    for (int i = 0; i < 4; i++) pthread_join(t[i], NULL);
    puts("plat_ww_test: ok");
    return 0;
}
