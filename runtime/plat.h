/* Host platform layer (Linux port, step 1; macOS on Apple silicon follows Linux, its differences under __APPLE__).
 *
 * Windows: <windows.h> plus thin wrappers that make exactly the calls the runtime made before, so the Windows build
 * does not change. Linux: the same Win32 names for the simple things the runtime uses everywhere (integer types,
 * Interlocked*, SRW locks, QueryPerformanceCounter, Sleep, _aligned_malloc), so those call sites need no edits, and
 * plat_* functions for guest memory, whose reserve/commit model differs. Only what the call sites use is here. */
#ifndef RECOMP_PLAT_H
#define RECOMP_PLAT_H

#include <stddef.h>
#include <stdint.h>

enum { PLAT_NONE, PLAT_RO, PLAT_RW };

#ifdef _WIN32
#include <windows.h>

/* rw memory at exactly addr (NULL when it cannot be mapped there) */
static inline void *plat_map_fixed(uint64_t addr, size_t size) {
    void *p = VirtualAlloc((void *)(uintptr_t)addr, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return p == (void *)(uintptr_t)addr ? p : NULL;
}
static inline void *plat_alloc(size_t size) { return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE); }
static inline void *plat_reserve(size_t size) { return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_NOACCESS); }
static inline int plat_commit(void *p, size_t size) { return VirtualAlloc(p, size, MEM_COMMIT, PAGE_READWRITE) == p; }
static inline int plat_protect(void *p, size_t size, int prot) {
    DWORD old;
    return VirtualProtect(p, size, prot == PLAT_RW ? PAGE_READWRITE : prot == PLAT_RO ? PAGE_READONLY : PAGE_NOACCESS, &old) != 0;
}

#else /* POSIX (Linux, macOS) */
#include <errno.h>
#include <malloc.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <ctype.h>
#include <strings.h>
#include <wchar.h>

/* ---------------------------------------------------------------- Win32 types and constants the runtime uses */
typedef int32_t LONG, BOOL, INT;
typedef int64_t LONG64, LONGLONG, INT64, INT_PTR, LONG_PTR;
typedef uint64_t ULONG64;
typedef uint32_t DWORD, UINT, ULONG, UINT32;
typedef uint64_t DWORD64, ULONGLONG, UINT64, ULONG_PTR, DWORD_PTR, UINT_PTR;
typedef uint16_t WORD;
typedef wchar_t WCHAR;          /* 32-bit here: the runtime writes its wide strings as L"..." literals */
typedef int16_t SHORT;
typedef uint16_t USHORT;
typedef uint8_t BYTE, BOOLEAN;
typedef size_t SIZE_T;
typedef void *HANDLE, *HMODULE, *HINSTANCE, *LPVOID, *PVOID;
typedef const void *LPCVOID;
typedef char *LPSTR;
typedef const char *LPCSTR;
typedef const WCHAR *LPCWSTR;
typedef int32_t HRESULT;
typedef union { int64_t QuadPart; struct { uint32_t LowPart; int32_t HighPart; }; } LARGE_INTEGER;
typedef union { uint64_t QuadPart; struct { uint32_t LowPart, HighPart; }; } ULARGE_INTEGER;
typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct SECURITY_ATTRIBUTES SECURITY_ATTRIBUTES;
typedef DWORD (*LPTHREAD_START_ROUTINE)(LPVOID);
typedef void (*LPFIBER_START_ROUTINE)(LPVOID);
#define WINAPI
#define CALLBACK
#define TRUE 1
#define FALSE 0
#define MAX_PATH 260
#define INFINITE 0xFFFFFFFFu
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define S_OK 0
#define SUCCEEDED(hr) ((HRESULT)(hr) >= 0)
#define FAILED(hr) ((HRESULT)(hr) < 0)
#define WAIT_OBJECT_0 0u
#define WAIT_TIMEOUT 258u
#define WAIT_FAILED 0xFFFFFFFFu
#define CREATE_SUSPENDED 4u
#define STACK_SIZE_PARAM_IS_A_RESERVATION 0x10000u
#define TIMER_ALL_ACCESS 0x1F0003u
#define MemoryBarrier() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* memory: VirtualAlloc and friends over mmap (plat_posix.c); MEM_WRITE_WATCH with GetWriteWatch on userfaultfd */
#define MEM_COMMIT 0x1000u
#define MEM_RESERVE 0x2000u
#define MEM_DECOMMIT 0x4000u
#define MEM_RELEASE 0x8000u
#define MEM_FREE 0x10000u
#define MEM_WRITE_WATCH 0x200000u
#define PAGE_NOACCESS 0x01u
#define PAGE_READONLY 0x02u
#define PAGE_READWRITE 0x04u
#define PAGE_EXECUTE_READ 0x20u
#define PAGE_EXECUTE_READWRITE 0x40u
#define PAGE_GUARD 0x100u
#define WRITE_WATCH_FLAG_RESET 1u
typedef struct { PVOID BaseAddress, AllocationBase; DWORD AllocationProtect; SIZE_T RegionSize; DWORD State, Protect, Type; } MEMORY_BASIC_INFORMATION;
typedef struct { WORD wProcessorArchitecture, wReserved; DWORD dwPageSize; LPVOID lpMinimumApplicationAddress, lpMaximumApplicationAddress;
                 DWORD_PTR dwActiveProcessorMask; DWORD dwNumberOfProcessors, dwProcessorType, dwAllocationGranularity; WORD wProcessorLevel, wProcessorRevision; } SYSTEM_INFO;
LPVOID VirtualAlloc(LPVOID addr, SIZE_T size, DWORD type, DWORD prot);
BOOL VirtualFree(LPVOID addr, SIZE_T size, DWORD type);
BOOL VirtualProtect(LPVOID addr, SIZE_T size, DWORD prot, DWORD *old);
SIZE_T VirtualQuery(LPCVOID addr, MEMORY_BASIC_INFORMATION *mi, SIZE_T len);
BOOL IsBadReadPtr(const void *p, UINT_PTR n);
UINT GetWriteWatch(DWORD flags, PVOID base, SIZE_T size, PVOID *addrs, ULONG_PTR *count, ULONG *gran);
UINT ResetWriteWatch(LPVOID base, SIZE_T size);
void GetSystemInfo(SYSTEM_INFO *si);

/* handles: threads, events, semaphores and timers are one waitable object (plat_posix.c) */
HANDLE CreateThread(SECURITY_ATTRIBUTES *sa, SIZE_T stack, LPTHREAD_START_ROUTINE fn, LPVOID arg, DWORD flags, DWORD *id);
HANDLE GetCurrentThread(void);
DWORD GetCurrentThreadId(void);
DWORD GetThreadId(HANDLE h);
DWORD ResumeThread(HANDLE h);
DWORD SuspendThread(HANDLE h);
BOOL SetThreadPriority(HANDLE h, int prio);
int GetThreadPriority(HANDLE h);
BOOL GetExitCodeThread(HANDLE h, DWORD *code);
HANDLE CreateEventA(SECURITY_ATTRIBUTES *sa, BOOL manual, BOOL initial, LPCSTR name);
BOOL SetEvent(HANDLE h);
BOOL ResetEvent(HANDLE h);
HANDLE CreateSemaphoreA(SECURITY_ATTRIBUTES *sa, LONG initial, LONG max, LPCSTR name);
BOOL ReleaseSemaphore(HANDLE h, LONG n, LONG *prev);
HANDLE CreateWaitableTimerExW(SECURITY_ATTRIBUTES *sa, LPCWSTR name, DWORD flags, DWORD access);
BOOL SetWaitableTimer(HANDLE h, const LARGE_INTEGER *due, LONG period, void *fn, LPVOID arg, BOOL resume);
DWORD WaitForSingleObject(HANDLE h, DWORD ms);
DWORD WaitForMultipleObjects(DWORD n, const HANDLE *h, BOOL all, DWORD ms);
BOOL CloseHandle(HANDLE h);
#define THREAD_PRIORITY_LOWEST (-2)
#define THREAD_PRIORITY_BELOW_NORMAL (-1)
#define THREAD_PRIORITY_NORMAL 0
#define THREAD_PRIORITY_ABOVE_NORMAL 1
#define THREAD_PRIORITY_HIGHEST 2
#define THREAD_PRIORITY_TIME_CRITICAL 15
#define SetThreadPriorityBoost(h, off) TRUE
#define SetThreadAffinityMask(h, m) ((DWORD_PTR)1)   /* shortcut: affinity not applied, port pthread_setaffinity_np if pinning matters */
#define GetCurrentProcessId() ((DWORD)getpid())
#ifdef __APPLE__
#define GetCurrentProcessorNumber() ((DWORD)0)   /* macOS does not say which core a thread is on */
#else
#define GetCurrentProcessorNumber() ((DWORD)sched_getcpu())
#endif
static inline uint64_t GetTickCount64(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
#define GetTickCount() ((DWORD)GetTickCount64())
void GetSystemTimeAsFileTime(FILETIME *ft);

/* critical sections (recursive, as on Windows) and condition variables */
typedef pthread_mutex_t CRITICAL_SECTION;
typedef pthread_cond_t CONDITION_VARIABLE;
#define CONDITION_VARIABLE_INIT PTHREAD_COND_INITIALIZER
void InitializeCriticalSection(CRITICAL_SECTION *cs);
#define InitializeCriticalSectionAndSpinCount(cs, n) (InitializeCriticalSection(cs), TRUE)
#define EnterCriticalSection(cs) pthread_mutex_lock(cs)
#define LeaveCriticalSection(cs) pthread_mutex_unlock(cs)
#define TryEnterCriticalSection(cs) (pthread_mutex_trylock(cs) == 0)
#define DeleteCriticalSection(cs) pthread_mutex_destroy(cs)
#define InitializeConditionVariable(cv) pthread_cond_init((cv), NULL)
#define WakeConditionVariable(cv) pthread_cond_signal(cv)
#define WakeAllConditionVariable(cv) pthread_cond_broadcast(cv)
BOOL SleepConditionVariableCS(CONDITION_VARIABLE *cv, CRITICAL_SECTION *cs, DWORD ms);
typedef struct { volatile int done; } INIT_ONCE;
#define INIT_ONCE_STATIC_INIT {0}
typedef INIT_ONCE *PINIT_ONCE;
BOOL InitOnceExecuteOnce(INIT_ONCE *once, BOOL (*fn)(INIT_ONCE *, PVOID, PVOID *), PVOID param, PVOID *ctx);

/* fiber-local storage (destructor at thread exit) and fibers (ucontext) */
#define FLS_OUT_OF_INDEXES 0xFFFFFFFFu
DWORD FlsAlloc(void (*dtor)(PVOID));
#define FlsSetValue(k, v) (pthread_setspecific((pthread_key_t)(k), (v)) == 0)
#define FlsGetValue(k) pthread_getspecific((pthread_key_t)(k))
LPVOID ConvertThreadToFiber(LPVOID arg);
LPVOID CreateFiberEx(SIZE_T commit, SIZE_T reserve, DWORD flags, LPFIBER_START_ROUTINE fn, LPVOID arg);
void SwitchToFiber(LPVOID fiber);
void DeleteFiber(LPVOID fiber);

/* modules and process */
HMODULE LoadLibraryA(LPCSTR name);
#define LoadLibraryExA(name, file, flags) LoadLibraryA(name)
#define LOAD_WITH_ALTERED_SEARCH_PATH 8u
void *GetProcAddress(HMODULE m, LPCSTR name);
HMODULE GetModuleHandleA(LPCSTR name);
DWORD GetModuleFileNameA(HMODULE m, LPSTR out, DWORD n);
DWORD GetLastError(void);
void RaiseException(DWORD code, DWORD flags, DWORD n, const ULONG_PTR *args);

/* files and directories (paths with / or \, made POSIX in plat_posix.c) */
#define FILE_ATTRIBUTE_DIRECTORY 0x10u
#define MOVEFILE_REPLACE_EXISTING 1u
typedef enum { GetFileExInfoStandard } GET_FILEEX_INFO_LEVELS;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow; } WIN32_FILE_ATTRIBUTE_DATA;
BOOL CreateDirectoryA(LPCSTR path, SECURITY_ATTRIBUTES *sa);
BOOL RemoveDirectoryA(LPCSTR path);
BOOL DeleteFileA(LPCSTR path);
BOOL MoveFileExA(LPCSTR from, LPCSTR to, DWORD flags);
BOOL GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, void *out);
DWORD GetTempPathA(DWORD n, LPSTR out);
char *_fullpath(char *out, const char *path, size_t n);

#define INVALID_FILE_ATTRIBUTES 0xFFFFFFFFu
#define FILE_ATTRIBUTE_READONLY 0x1u
#define FILE_ATTRIBUTE_NORMAL 0x80u
#define MOVEFILE_WRITE_THROUGH 8u
#define GENERIC_READ 0x80000000u
#define GENERIC_WRITE 0x40000000u
#define FILE_SHARE_READ 1u
#define FILE_SHARE_WRITE 2u
#define FILE_SHARE_DELETE 4u
#define CREATE_NEW 1u
#define CREATE_ALWAYS 2u
#define OPEN_EXISTING 3u
#define OPEN_ALWAYS 4u
#define TRUNCATE_EXISTING 5u
#define FILE_BEGIN 0u
#define FILE_CURRENT 1u
#define FILE_END 2u
#define FILE_FLAG_SEQUENTIAL_SCAN 0x08000000u
#define FILE_FLAG_WRITE_THROUGH 0x80000000u
#define ERROR_SUCCESS 0u
#define ERROR_FILE_NOT_FOUND 2u
#define ERROR_PATH_NOT_FOUND 3u
#define ERROR_ACCESS_DENIED 5u
#define ERROR_ALREADY_EXISTS 183u
#define ERROR_FILE_EXISTS 80u
#define ERROR_NO_MORE_FILES 18u
#define ERROR_TIMEOUT 1460u
#define SYNCHRONIZE 0x00100000u
#define CP_ACP 0u
#define CP_UTF8 65001u
typedef struct { ULONG_PTR Internal, InternalHigh; DWORD Offset, OffsetHigh; HANDLE hEvent; } OVERLAPPED;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
                 char cFileName[MAX_PATH]; char cAlternateFileName[14]; } WIN32_FIND_DATAA;
typedef struct { DWORD dwFileAttributes; FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime; DWORD nFileSizeHigh, nFileSizeLow, dwReserved0, dwReserved1;
                 WCHAR cFileName[MAX_PATH]; WCHAR cAlternateFileName[14]; } WIN32_FIND_DATAW;
HANDLE CreateFileA(LPCSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition, DWORD flags, HANDLE tmpl);
HANDLE CreateFileW(LPCWSTR path, DWORD access, DWORD share, SECURITY_ATTRIBUTES *sa, DWORD disposition, DWORD flags, HANDLE tmpl);
BOOL ReadFile(HANDLE h, LPVOID buf, DWORD n, DWORD *got, OVERLAPPED *ov);
BOOL WriteFile(HANDLE h, LPCVOID buf, DWORD n, DWORD *put, OVERLAPPED *ov);
BOOL SetFilePointerEx(HANDLE h, LARGE_INTEGER to, LARGE_INTEGER *pos, DWORD how);
BOOL SetEndOfFile(HANDLE h);
BOOL GetFileSizeEx(HANDLE h, LARGE_INTEGER *size);
BOOL FlushFileBuffers(HANDLE h);
DWORD GetFileAttributesA(LPCSTR path);
BOOL SetFileAttributesA(LPCSTR path, DWORD attr);
#define MoveFileA(from, to) MoveFileExA((from), (to), 0)
HANDLE FindFirstFileA(LPCSTR pattern, WIN32_FIND_DATAA *fd);
BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
HANDLE FindFirstFileW(LPCWSTR pattern, WIN32_FIND_DATAW *fd);
BOOL FindNextFileW(HANDLE h, WIN32_FIND_DATAW *fd);
BOOL FindClose(HANDLE h);
int MultiByteToWideChar(UINT cp, DWORD flags, LPCSTR in, int n, WCHAR *out, int cap);
int WideCharToMultiByte(UINT cp, DWORD flags, LPCWSTR in, int n, LPSTR out, int cap, LPCSTR dflt, BOOL *used);
/* file mappings, read-only (nvn_vk_store.c) */
#define PAGE_READONLY_MAP PAGE_READONLY
#define FILE_MAP_READ 4u
HANDLE CreateFileMappingA(HANDLE file, SECURITY_ATTRIBUTES *sa, DWORD prot, DWORD hi, DWORD lo, LPCSTR name);
LPVOID MapViewOfFile(HANDLE map, DWORD access, DWORD off_hi, DWORD off_lo, SIZE_T n);
BOOL UnmapViewOfFile(LPCVOID p);
int _get_osfhandle_fd(HANDLE h);   /* the descriptor behind a file handle */

/* time */
typedef struct { WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds; } SYSTEMTIME;
void GetLocalTime(SYSTEMTIME *st);
void GetSystemTime(SYSTEMTIME *st);
typedef int64_t __time64_t;
#define _time64(t) ((__time64_t)time(t))
#define _mkgmtime64(tm) ((__time64_t)timegm(tm))
#define _gmtime64_s(out, t) (gmtime_r((const time_t *)(t), (out)) ? 0 : 22)
#define _localtime64_s(out, t) (localtime_r((const time_t *)(t), (out)) ? 0 : 22)

/* diagnostics the runtime uses on Windows only (profilers, watchdogs, crash walks): here they fail the way Windows
 * calls are allowed to, and the code that uses them already reports nothing then */
typedef struct { DWORD ContextFlags; DWORD64 Dr0, Dr1, Dr2, Dr3, Dr6, Dr7, Rax, Rcx, Rdx, Rbx, Rsp, Rbp, Rsi, Rdi, Rip; } CONTEXT;
#define CONTEXT_CONTROL 1u
#define CONTEXT_INTEGER 2u
#define CONTEXT_FULL 7u
#define CONTEXT_DEBUG_REGISTERS 0x10u
typedef struct { DWORD BeginAddress, EndAddress, UnwindData; } RUNTIME_FUNCTION;
#define UNW_FLAG_NHANDLER 0u
typedef struct { DWORD ExceptionCode, ExceptionFlags; void *ExceptionRecord; PVOID ExceptionAddress; DWORD NumberParameters; ULONG_PTR ExceptionInformation[15]; } EXCEPTION_RECORD;
typedef struct { EXCEPTION_RECORD *ExceptionRecord; CONTEXT *ContextRecord; } EXCEPTION_POINTERS;
#define EXCEPTION_ACCESS_VIOLATION 0xC0000005u
#define EXCEPTION_SINGLE_STEP 0x80000004u
#define EXCEPTION_EXECUTE_HANDLER 1
#define EXCEPTION_CONTINUE_SEARCH 0
#define EXCEPTION_CONTINUE_EXECUTION (-1)
#define GetThreadContext(h, c) FALSE
#define SetThreadContext(h, c) FALSE
#define RtlCaptureContext(c) memset((c), 0, sizeof *(c))
#define RtlLookupFunctionEntry(pc, base, t) ((RUNTIME_FUNCTION *)NULL)
#define RtlVirtualUnwind(...) ((void *)NULL)
#define AddVectoredExceptionHandler(first, fn) ((PVOID)NULL)
#define RemoveVectoredExceptionHandler(h) 0
#define SetUnhandledExceptionFilter(fn) ((void *)NULL)
#define THREAD_GET_CONTEXT 8u
#define THREAD_SET_CONTEXT 0x10u
#define THREAD_SUSPEND_RESUME 2u
#define THREAD_QUERY_INFORMATION 0x40u
#define OpenThread(access, inherit, id) ((HANDLE)NULL)
typedef struct { DWORD dwSize, cntUsage, th32ThreadID, th32OwnerProcessID; LONG tpBasePri, tpDeltaPri; DWORD dwFlags; } THREADENTRY32;
typedef struct { DWORD dwSize, th32ModuleID, th32ProcessID, GlblcntUsage, ProccntUsage; BYTE *modBaseAddr; DWORD modBaseSize; HMODULE hModule;
                 char szModule[256], szExePath[MAX_PATH]; } MODULEENTRY32;
#define TH32CS_SNAPTHREAD 4u
#define TH32CS_SNAPMODULE 8u
#define CreateToolhelp32Snapshot(flags, pid) INVALID_HANDLE_VALUE
#define Thread32First(h, te) FALSE
#define Thread32Next(h, te) FALSE
#define Module32First(h, me) FALSE
#define Module32Next(h, me) FALSE
#define ReadProcessMemory(p, a, b, n, got) FALSE
#define GetCurrentProcess() ((HANDLE)(intptr_t)-1)
#define OpenProcess(access, inherit, pid) ((HANDLE)NULL)
#define DUPLICATE_SAME_ACCESS 2u
#define DuplicateHandle(...) FALSE
typedef struct { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
                 QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage; } PROCESS_MEMORY_COUNTERS;
#define GetProcessMemoryInfo(p, pmc, cb) (memset((pmc), 0, (cb)), FALSE)
#define GetThreadTimes(...) FALSE
#define GetProcessTimes(...) FALSE
#define GetSystemTimes(...) FALSE
#define QueryThreadCycleTime(h, c) (*(c) = 0, FALSE)
BOOL GetProcessAffinityMask(HANDLE p, DWORD_PTR *proc, DWORD_PTR *sys);
BOOLEAN SystemFunction036(PVOID buf, ULONG n);   /* RtlGenRandom */
#define NTAPI
#define IsBadWritePtr(p, n) IsBadReadPtr((p), (n))
extern int __argc;
extern char **__argv;

#define GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT 2u
#define GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS 4u
BOOL GetModuleHandleExA(DWORD flags, LPCSTR name_or_addr, HMODULE *out);
/* shortcut: a named mutex is one of this process only (the save commit lock; one game runs at a time) */
HANDLE CreateMutexA(SECURITY_ATTRIBUTES *sa, BOOL owned, LPCSTR name);
BOOL ReleaseMutex(HANDLE h);
BOOL CopyFileA(LPCSTR from, LPCSTR to, BOOL fail_if_exists);
BOOL GetUserNameW(WCHAR *out, DWORD *n);
#define _fileno fileno
HANDLE plat_fd_handle(int fd);   /* a file handle for a descriptor the caller keeps (not closed by anyone) */
#define _get_osfhandle(fd) ((intptr_t)plat_fd_handle(fd))
/* the OpenGL back end (WGL) is Windows only; Linux renders with Vulkan */
#define wglGetProcAddress(name) ((void *)NULL)

/* paths as on Windows: \ is a separator, and a name that does not exist as written is looked up case-insensitively,
 * one component at a time (RomFS, mods and the runtime's own names are written in any case) */
const char *plat_path(const char *in, char *out, size_t n);
FILE *plat_fopen(const char *path, const char *mode);
#define fopen plat_fopen

/* C runtime names */
#define _stricmp strcasecmp
#define _strnicmp strncasecmp
#define _strdup strdup
#define _fseeki64 fseeko
#define _ftelli64 ftello
#define _putenv(s) putenv(strdup(s))
#define _putenv_s(k, v) setenv((k), (v), 1)
static inline char *_strlwr(char *s) { for (char *p = s; *p; p++) *p = (char)tolower((unsigned char)*p); return s; }

#ifdef __APPLE__
/* macOS: no MAP_FIXED_NOREPLACE (mmap takes an address as a hint it need not follow): plat_map_at (plat_posix.c) asks
 * Mach for exactly that address, which fails when something is there. No MAP_NORESERVE or MAP_STACK either (memory is
 * not accounted before it is touched anyway). The host's pages are 16 KB where the guest's are 4 KB: plat_mprotect
 * changes the host pages a 4 KB range can be given. */
#define MAP_NORESERVE 0
#define MAP_STACK 0
int plat_mprotect(void *p, size_t size, int prot);
void *plat_map_at(uint64_t addr, size_t size, int prot);   /* NULL when it cannot be mapped there */
static inline void *plat_map_fixed(uint64_t addr, size_t size) { return plat_map_at(addr, size, PROT_READ | PROT_WRITE); }
#else
#define plat_mprotect mprotect
static inline void *plat_map_fixed(uint64_t addr, size_t size) {
    void *p = mmap((void *)(uintptr_t)addr, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    return p == (void *)(uintptr_t)addr ? p : NULL;   /* kernels before 4.17 ignore the flag and may map elsewhere */
}
#endif
static inline void *plat_alloc(size_t size) {
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static inline void *plat_reserve(size_t size) {
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}
static inline int plat_commit(void *p, size_t size) { return plat_mprotect(p, size, PROT_READ | PROT_WRITE) == 0; }
static inline int plat_protect(void *p, size_t size, int prot) {
    return plat_mprotect(p, size, prot == PLAT_RW ? PROT_READ | PROT_WRITE : prot == PLAT_RO ? PROT_READ : PROT_NONE) == 0;
}

/* Interlocked*: full barriers, return values as on Windows (the new value, or the old one for Exchange/CompareExchange) */
#define InterlockedIncrement(p)              __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedIncrement64(p)            __atomic_add_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement(p)              __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedDecrement64(p)            __atomic_sub_fetch((p), 1, __ATOMIC_SEQ_CST)
#define InterlockedAdd(p, v)                 __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedAdd64(p, v)               __atomic_add_fetch((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd(p, v)         __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangeAdd64(p, v)       __atomic_fetch_add((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange(p, v)            __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchange64(p, v)          __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedExchangePointer(p, v)     __atomic_exchange_n((p), (v), __ATOMIC_SEQ_CST)
#define InterlockedOr8(p, v)                 __atomic_fetch_or((p), (v), __ATOMIC_SEQ_CST)
#define plat_cas_(p, x, c) __extension__({ __typeof__(*(p) + 0) e_ = (c);   /* +0 drops volatile */ __atomic_compare_exchange_n((p), &e_, (x), 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); e_; })
#define InterlockedCompareExchange(p, x, c)        plat_cas_((p), (x), (c))
#define InterlockedCompareExchange64(p, x, c)      plat_cas_((p), (x), (c))
#define InterlockedCompareExchangePointer(p, x, c) plat_cas_((p), (x), (c))
#if defined(__aarch64__)
#define YieldProcessor() __asm__ volatile("yield")
#else
#define YieldProcessor() __builtin_ia32_pause()
#endif

/* SRW locks: a mutex, because SleepConditionVariableSRW waits on them (a pthread condvar needs a mutex); shared
 * acquisitions are exclusive, which is stricter and so still correct */
typedef pthread_mutex_t SRWLOCK;
#define SRWLOCK_INIT PTHREAD_MUTEX_INITIALIZER
#define InitializeSRWLock(l)           pthread_mutex_init((l), NULL)
#define AcquireSRWLockExclusive(l)     pthread_mutex_lock(l)
#define ReleaseSRWLockExclusive(l)     pthread_mutex_unlock(l)
#define TryAcquireSRWLockExclusive(l)  (pthread_mutex_trylock(l) == 0)
#define AcquireSRWLockShared(l)        pthread_mutex_lock(l)
#define ReleaseSRWLockShared(l)        pthread_mutex_unlock(l)
BOOL SleepConditionVariableSRW(CONDITION_VARIABLE *cv, SRWLOCK *l, DWORD ms, ULONG flags);

/* time: QueryPerformanceCounter in nanoseconds */
static inline int QueryPerformanceCounter(LARGE_INTEGER *t) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    t->QuadPart = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
    return 1;
}
static inline int QueryPerformanceFrequency(LARGE_INTEGER *f) { f->QuadPart = 1000000000; return 1; }
static inline void Sleep(DWORD ms) {
    if (!ms) { sched_yield(); return; }   /* Sleep(0) gives up the time slice */
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000 };
    while (nanosleep(&ts, &ts) && errno == EINTR) {}
}
#define SwitchToThread() (sched_yield(), 1)
/* like ExitProcess: other threads are not unwound, no atexit. [witchtime] plat_exit_hook (plat_posix.c) runs first when
 * a host has set it: the Witch Time launcher, which runs the game in its own process and window (launcher/) */
__attribute__((noreturn)) void plat_exit_process(int code);
#define ExitProcess(code) plat_exit_process((int)(code))

/* _aligned_*: glibc's free and malloc_usable_size accept memory from aligned_alloc / posix_memalign */
static inline void *_aligned_malloc(size_t n, size_t align) { void *p; return posix_memalign(&p, align, n) ? NULL : p; }
static inline void _aligned_free(void *p) { free(p); }
static inline size_t _aligned_msize(void *p, size_t align, size_t off) { (void)align; (void)off; return malloc_usable_size(p); }
static inline void *_aligned_realloc(void *p, size_t n, size_t align) {
    if (!p) return _aligned_malloc(n, align);
    void *q = _aligned_malloc(n, align);
    if (!q) return NULL;   /* like realloc: the old block stays valid */
    size_t old = malloc_usable_size(p);
    memcpy(q, p, old < n ? old : n);
    free(p);
    return q;
}
#endif

#endif
