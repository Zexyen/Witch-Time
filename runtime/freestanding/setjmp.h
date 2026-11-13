/* Freestanding setjmp.h for the code compiled on the player's PC (README.md): glibc's x86-64 jmp_buf (200 bytes; the
 * prebuilt sdklift_rt.c sees the same SdkJmpRec through glibc's own header) and setjmp; on Windows mingw-w64's (256 bytes),
 * on macOS the system's (192 bytes). */
#ifndef WT_FREESTANDING_SETJMP_H
#define WT_FREESTANDING_SETJMP_H
#ifdef _WIN32
/* Windows: mingw-w64's x86-64 jmp_buf (256 bytes) and its setjmp, the UCRT's _setjmp with the frame to unwind to, as the
 * prebuilt sdklift_rt.c sees them through mingw-w64's own header */
typedef struct __attribute__ ((__aligned__ (16))) _SETJMP_FLOAT128 {
    long long LowPart;
    long long HighPart;
} SETJMP_FLOAT128;
typedef SETJMP_FLOAT128 jmp_buf[16];
__declspec(dllimport) int __attribute__ ((__nothrow__,__returns_twice__)) _setjmp(jmp_buf _Buf, void *_Frame);
#if __has_builtin(__builtin_sponentry)
#define setjmp(BUF) _setjmp((BUF), __builtin_sponentry())
#else
#define setjmp(BUF) _setjmp((BUF), __builtin_frame_address(0))
#endif
#elif defined(__APPLE__)
/* macOS on Apple silicon: the system's jmp_buf (48 ints, 192 bytes) and setjmp, as the runtime's sdklift_rt.c sees them
 * through the SDK's own header */
typedef int jmp_buf[48];
extern int setjmp (jmp_buf);
#else
typedef long int __jmp_buf[8];
typedef struct { unsigned long int __val[(1024 / (8 * sizeof (unsigned long int)))]; } __sigset_t;
struct __jmp_buf_tag {
    __jmp_buf __jmpbuf;
    int __mask_was_saved;
    __sigset_t __saved_mask;
};
typedef struct __jmp_buf_tag jmp_buf[1];
extern int setjmp (jmp_buf __env) __attribute__ ((__nothrow__));
#endif
#endif
