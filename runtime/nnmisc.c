/* Small SDK services: nn::oe notifications, nn::prepo, nn::aoc, nn::err,
 * the C-linkage nnos* wrappers, setjmp/longjmp, and "offline console" answers for the
 * network-facing services (friends, account extras, nifm, socket, ldn, ec accessor, album lists ...).
 *
 * Where a behaviour or layout is stated here it was read from one of:
 *   [sdk]   the real SDK module (binaries\13.0.4\sdk.elf), disassembled by symbol name;
 *   [game]  the game's own code at the quoted address;
 *   [doc]   public documentation (switchbrew / open-ead nnheaders), not checked against code.
 * Everything else only returns "success", "nothing" or "not available" and touches no
 * guest structure.
 *
 * Registration never replaces a handler another file already installed, with one
 * exception (nn::err::ErrorResultVariant, see below). */
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <setjmp.h>
#include <windows.h>
#include <wincrypt.h>
#include "runtime.h"
#include "hostrt.h"
#include "image_layout.h"          /* GUEST_RO_END: the start of main's data segment */

#pragma comment(lib, "advapi32.lib")

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define STR(n) ((const char *)(uintptr_t)c->x[n])

/* Some non-zero nn::Result. The SDK's real "not available" codes differ per service and are
 * not established; the game only tests these for failure. Same value as nnsdk.c. */

void os_init_event(uint64_t key, int autoclear);          /* os_threads.c */
void os_signal_event(uint64_t key);

static void keep(Ctx *c) { (void)c; }                     /* void function, or returns `this` */
static void ret0(Ctx *c) { RET(0); }                      /* Result success / false / 0 / null */
static void ret1(Ctx *c) { RET(1); }
/* nn::account network-service-account functions on a machine without a linked account: see nnfs.c ([sdk] 0x4b3a8,
 * account results 124-0200..0269 mean "no usable network service account"). */
static void acc_nsa_unavailable(Ctx *c) { RET(0x1907cull); }

static void fatal(Ctx *c, const char *fmt, ...) {
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    printf("\n%s (guest caller 0x%llx)\n", msg, (unsigned long long)c->x[30] - 4);
    trace_printf("%s (guest caller 0x%llx)\n", msg, (unsigned long long)c->x[30] - 4);
    report_and_exit(3);
}
static const char *safe_str(uint64_t p) { return p > 0x10000 ? (const char *)(uintptr_t)p : "(null)"; }

/* ---------------------------------------------------------------- C-linkage nn::os wrappers
 * [sdk] every nnosXxx is a tail jump to the C++ function with the same arguments, so each one
 * forwards to whatever handler is registered for the C++ name (looked up on first use). */
static ImportFn forward_target(Ctx *c, const char *name) {
    ImportFn f = import_lookup(name);
    if (!f) fatal(c, "nnmisc: no handler for %s, needed by a C wrapper", name);
    return f;
}
#define FWD(fn, target) static void fn(Ctx *c) { static ImportFn f; if (!f) f = forward_target(c, target); f(c); }
#define OS(n) "_ZN2nn2os" n
FWD(nnos_GetSystemTick, OS("13GetSystemTickEv"))
FWD(nnos_GetSystemTickFrequency, OS("22GetSystemTickFrequencyEv"))
FWD(nnos_ConvertTickToNanoSeconds, OS("17ConvertToTimeSpanENS0_4TickE"))

/* ---------------------------------------------------------------- setjmp / longjmp
 * Upstream wrote this for a call site of this shape (Smash's only one: Lua's luaD_rawrunprotected and luaD_throw):
 *
 *     bl   setjmp              ; x0 = &lj.b   (lj on the stack, jmp_buf 0x138 bytes)
 *     cbnz w0, after
 *     mov  x0, x19
 *     mov  x1, x21
 *     blr  x20                 ; the protected call
 *   after:                     ; reads lj.status from memory, not from setjmp's value
 *
 * Lifted code runs each guest function as a host function on the host stack, so the host
 * handler of setjmp cannot "return a second time" once it has returned. Instead the handler
 * performs the protected call itself (the three instructions after the cbnz) while its own
 * host frame is still alive, and then returns non-zero so the lifted code continues at
 * `after`. A guest longjmp unwinds the host stack back into that handler with the host's
 * longjmp, restores the guest callee-saved registers and stack pointer saved at setjmp time,
 * and the handler returns non-zero as well. Both ways reach `after`, exactly as on the console.
 *
 * Limitation: this is only valid for a setjmp whose call site has exactly this shape. The
 * handler checks the four instruction words and stops with a diagnostic for any other caller.
 * The host record lives in the handler's frame; the guest jmp_buf holds a magic and a pointer
 * to it, so nesting, guest threads and fibers need no extra bookkeeping. */
#define JMP_MAGIC 0x4a4d5052454330ull
typedef struct {
    jmp_buf host;
    uint64_t x[13];                                       /* x19..x30, sp */
    uint64_t d[8];                                        /* d8..d15 */
    const char *in_import;
    uint64_t import_from;
} JmpRec;

static void c_setjmp(Ctx *c) {
    Ctx *volatile cc = c;
    volatile uint64_t buf = A0;
    uint64_t site = c->x[30];                             /* the instruction after the bl */
    if (rd32(site) != 0x35000080u || rd32(site + 4) != 0xaa1303e0u || rd32(site + 8) != 0xaa1503e1u ||
        rd32(site + 12) != 0xd63f0280u)
        fatal(c, "setjmp: call site is not the `cbnz w0; mov x0,x19; mov x1,x21; blr x20` pattern this port can run");
    JmpRec r;
    for (int i = 0; i < 12; i++) r.x[i] = c->x[19 + i];
    r.x[12] = c->x[31];
    for (int i = 0; i < 8; i++) r.d[i] = c->v[8 + i].d[0];
    r.in_import = c->in_import;
    r.import_from = c->import_from;
    wr64(buf, JMP_MAGIC);
    wr64(buf + 8, (uint64_t)(uintptr_t)&r);
    if (setjmp(r.host) == 0) {
        c->x[0] = c->x[19];
        c->x[1] = c->x[21];
        c->x[30] = site + 16;
        c->in_import = NULL;                              /* guest code runs from here */
        call_indirect(c, c->x[20]);
    } else {                                              /* a guest longjmp landed here */
        Ctx *g = cc;
        for (int i = 0; i < 12; i++) g->x[19 + i] = r.x[i];
        g->x[31] = r.x[12];
        for (int i = 0; i < 8; i++) { g->v[8 + i].d[0] = r.d[i]; g->v[8 + i].d[1] = 0; }
    }
    cc->in_import = r.in_import;
    cc->import_from = r.import_from;
    wr64(buf, 0);                                         /* the record dies with this frame */
    cc->x[0] = 1;
}

static void c_longjmp(Ctx *c) {
    if (!A0 || rd64(A0) != JMP_MAGIC)
        fatal(c, "longjmp: jmp_buf %llx was not filled by a setjmp that is still active", (unsigned long long)A0);
    JmpRec *r = (JmpRec *)(uintptr_t)rd64(A0 + 8);
    longjmp(r->host, 1);
}


/* ---------------------------------------------------------------- abort helpers (never return on the console) */
static void diag_AbortImpl(Ctx *c) {                      /* (condition, function, file, line) */
    printf("\nnn::diag abort: %s  in %s  (%s:%d)\n", safe_str(A0), safe_str(A1), safe_str(A2), (int)A3);
    trace_printf("nn::diag abort: %s  in %s  (%s:%d)\n", safe_str(A0), safe_str(A1), safe_str(A2), (int)A3);
    guest_trap("guest called nn::diag::detail::AbortImpl", c->x[30] - 4);
}
static void diag_UnexpectedDefault(Ctx *c) {              /* (function, file, line) */
    printf("\nnn unexpected default: %s  (%s:%d)\n", safe_str(A0), safe_str(A1), (int)A2);
    trace_printf("nn unexpected default: %s  (%s:%d)\n", safe_str(A0), safe_str(A1), (int)A2);
    guest_trap("guest called nn::detail::UnexpectedDefaultImpl", c->x[30] - 4);
}
static void c_assert_fail(Ctx *c) {                       /* (expression, file, line, function) */
    printf("\nassertion failed: %s  (%s:%d, %s)\n", safe_str(A0), safe_str(A1), (int)A2, safe_str(A3));
    trace_printf("assertion failed: %s  (%s:%d, %s)\n", safe_str(A0), safe_str(A1), (int)A2, safe_str(A3));
    guest_trap("guest called __assert_fail", c->x[30] - 4);
}

/* ---------------------------------------------------------------- nn::oe */
static LARGE_INTEGER g_start;
/* Notification messages. On the console the system posts a message when the application's focus state changes
 * (HOME menu, an overlay), when the console is docked or undocked, and when it asks the application to exit; the
 * application reads them with TryPopNotificationMessage and may wait on the event GetNotificationMessageEvent
 * returns, which is signalled while messages are pending.
 * Here: the focus state follows the game window. When the window stops being the foreground window the state
 * becomes OutOfFocus (2) and MessageFocusStateChanged (15) is posted; back in the foreground: InFocus (1) and the
 * message again. [sdk] 0x32c3d0: SetFocusHandlingMode passes "notify focus changes" = true for the modes 1
 * (NoSuspend) and 2 (SuspendHomeSleepNotify) only, so the message is posted only in those modes.
 * The operation mode never changes on a PC (always the TV mode), so MessageOperationModeChanged (30) and
 * MessagePerformanceModeChanged (31) never occur; the enable flags are kept as the game sets them.
 * (close4: the state now follows the applet manager's table, see oe_track_focus: not in focus = 2 only while the window is
 * visible; minimised = 3 for a NoSuspend application and no change at all in the suspending modes.)
 * Differences from the console, stated: (1) in the suspending modes (0, 2, 3) the console also FREEZES the application
 * while it is in the background (mode 3: also while obscured); this port does not freeze the game's threads: a minimised
 * or covered game keeps running, sound included ([witchtime] the close4 freeze was removed: it silenced the game whenever
 * another window had the focus). (2) A window
 * that has never been in the foreground (the automated tests keep theirs off screen) is treated as in focus.
 * (3) Closing the window is the exit request: see oe_exit_request below.
 * WT_FOCUS_NOTIFY=0 turns the focus tracking off. */
#define OE_MSG_FOCUS_STATE_CHANGED 15u
static uint8_t g_oe_event[0x40];
static uint32_t g_oe_queue[16];
static int g_oe_count, g_oe_event_ready, g_oe_focus_state = 1, g_oe_focus_mode = 0, g_oe_was_foreground;
static int g_oe_opmode_notify, g_oe_perfmode_notify, g_oe_focus_pending, g_oe_focus_recheck;
static HWND g_oe_window;
static CRITICAL_SECTION g_oe_lock;
static void oe_event_setup(void) {
    if (!g_oe_event_ready) { os_init_event((uint64_t)(uintptr_t)g_oe_event, 0); g_oe_event_ready = 1; }
}
static void oe_push(uint32_t message) {
    EnterCriticalSection(&g_oe_lock);
    oe_event_setup();
    if (g_oe_count < 16) g_oe_queue[g_oe_count++] = message;
    os_signal_event((uint64_t)(uintptr_t)g_oe_event);
    LeaveCriticalSection(&g_oe_lock);
}
static void oe_track_focus(void) {
    static int enabled = -1;
    static ULONGLONG last;
    ULONGLONG now = GetTickCount64();
    if (enabled < 0) { const char *e = getenv("WT_FOCUS_NOTIFY"); enabled = !(e && e[0] == '0'); }
    if (!enabled || (now - last < 30 && !g_oe_focus_recheck)) return;
    last = now;
    g_oe_focus_recheck = 0;
    DWORD pid = 0;
    HWND fg = GetForegroundWindow();
    int ours = fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
    if (ours) { g_oe_was_foreground = 1; g_oe_window = fg; }
    if (!g_oe_was_foreground) return;
    /* close4: the applet manager's rule (suyu am/lifecycle_manager.cpp UpdateRequestedFocusState, GetFocusStateWhile*), with
     * the window as the activity state: foreground = ForegroundVisible; visible but not the foreground window = ForegroundObscured
     * (something else has the input, as under an overlay or a library applet); minimised = Background (as behind the HOME menu).
     * The handling mode is what [sdk 0x32c3d0, table 0xa7ff70] sends for the oe mode: 0 = (notify 0, suspend 1), 1 = (notify 1,
     * suspend 0) NoSuspend, 2 = (notify 1, suspend 1), 3 = (notify 0, suspend 1) + SetOutOfFocusSuspendingEnabled(1) AlwaysSuspend. */
    int minimised = g_oe_window && IsIconic(g_oe_window);
    int state = ours ? 1                                                    /* InFocus */
              : g_oe_focus_mode == 3 ? 1                                    /* AlwaysSuspend: the application never learns it */
              : !minimised ? 2                                              /* obscured: NotInFocus in the other three modes */
              : g_oe_focus_mode == 1 ? 3 : 1;                               /* background: Background (3) for a NoSuspend application, else unchanged */
    if (state != g_oe_focus_state) {
        g_oe_focus_state = state;
        g_oe_focus_pending = 1;                                             /* m_has_focus_state_changed */
        trace_printf("     nn::oe: focus state -> %d (%s; handling mode %d)\n", state, state == 1 ? "in focus" : state == 2 ? "not in focus" : "background", g_oe_focus_mode);
    }
    if (g_oe_focus_pending && (g_oe_focus_mode == 1 || g_oe_focus_mode == 2)) { g_oe_focus_pending = 0; oe_push(OE_MSG_FOCUS_STATE_CHANGED); }   /* delivered only while notification is enabled */
}
/* Exit request. [sdk 0x32cc20 / 0x32cca0] Enter / LeaveExitRequestHandlingSection keep a nesting count under a mutex and call the
 * applet manager's ISelfController LockExit when the count goes 0 -> 1 and UnlockExit when it goes 1 -> 0. The applet manager is a
 * system module (not in the dump); its rule, from the reference implementation (suyu am/service/self_controller.cpp LockExit /
 * UnlockExit, application_accessor.cpp RequestExit):
 *   exit requested while the exit is locked   -> the application is NOT ended; it gets MessageExitRequest (4) and keeps running
 *   exit requested while it is not locked     -> the process is terminated at once
 *   LockExit after an exit request            -> terminated at once
 *   UnlockExit after an exit request          -> terminated
 * Here the exit request is the user closing the game window (display.c WM_CLOSE calls oe_exit_request). */
#define OE_MSG_EXIT_REQUEST 4u
static int g_oe_exit_locked, g_oe_exit_requested;
int oe_exit_request(void) {                              /* display.c, window thread. Returns 1 when the game keeps running until it leaves its section */
    int locked;
    EnterCriticalSection(&g_oe_lock);
    locked = g_oe_exit_locked;
    if (locked) g_oe_exit_requested = 1;
    LeaveCriticalSection(&g_oe_lock);
    if (!locked) return 0;
    trace_printf("     nn::oe: exit requested inside an exit-request handling section: MessageExitRequest posted\n");
    oe_push(OE_MSG_EXIT_REQUEST);
    return 1;
}
static void oe_TryPopNotificationMessage(Ctx *c) {       /* (uint32_t *out) -> bool */
    uint64_t out = A0;
    int got = 0;
    oe_track_focus();
    EnterCriticalSection(&g_oe_lock);
    if (g_oe_count > 0) {
        if (out) wr32(out, g_oe_queue[0]);
        memmove(g_oe_queue, g_oe_queue + 1, (size_t)(--g_oe_count) * sizeof g_oe_queue[0]);
        got = 1;
        if (g_oe_count == 0 && g_oe_event_ready) os_init_event((uint64_t)(uintptr_t)g_oe_event, 0);   /* nothing pending: event cleared */
    }
    LeaveCriticalSection(&g_oe_lock);
    RET(got);
}
static void oe_GetCurrentFocusState(Ctx *c) { oe_track_focus(); RET(g_oe_focus_state); }
static void oe_SetFocusHandlingMode(Ctx *c) { g_oe_focus_mode = (int)(uint32_t)A0; g_oe_focus_recheck = 1; oe_track_focus(); }               /* 0 SuspendHomeSleep, 1 NoSuspend, 2 SuspendHomeSleepNotify, 3 AlwaysSuspend */
static void oe_SetOperationModeChangedNotificationEnabled(Ctx *c) { g_oe_opmode_notify = (int)(A0 & 1); }

/* ---------------------------------------------------------------- nn::settings
 * [doc] nn::settings::Language values and their codes (switchbrew "Settings services"); the
 * order agrees with what nnsdk.c already uses (4 = "it"). LanguageCode is 8 chars, returned in x0. */
static const char *const g_language_codes[] = {"ja", "en-US", "fr", "de", "it", "es", "zh-CN", "ko", "nl", "pt", "ru",
                                               "zh-TW", "en-GB", "fr-CA", "es-419", "zh-Hans", "zh-Hant", "pt-BR"};
static uint64_t language_code(uint32_t lang) {
    uint64_t v = 0;
    if (lang < sizeof g_language_codes / sizeof g_language_codes[0]) memcpy(&v, g_language_codes[lang], strlen(g_language_codes[lang]));
    return v;
}
static void settings_LanguageCode_Make(Ctx *c) { RET(language_code((uint32_t)A0)); }
static void settings_LanguageEqualsCode(Ctx *c) {         /* operator==(const Language&, const LanguageCode&) */
    char a[9] = {0}, b[9] = {0};
    uint64_t v = language_code(rd32(A0));
    memcpy(a, &v, 8);
    memcpy(b, (void *)(uintptr_t)A1, 8);
    RET(v != 0 && strcmp(a, b) == 0);
}
static void settings_CodeEqualsLanguage(Ctx *c) {         /* operator==(const LanguageCode&, const Language&): the same comparison, arguments swapped */
    uint64_t t = c->x[0];
    c->x[0] = c->x[1];
    c->x[1] = t;
    settings_LanguageEqualsCode(c);
}
/* nn::oe::GetDesiredLanguage: the language the application should use = the system language when the application
 * supports it, otherwise the system's choice among the supported ones. The system language here is the Windows
 * display language of the user. Languages this game ships (its message files): ja, en-US, en-GB, fr, fr-CA, de, it,
 * es, es-419, zh-Hans, zh-Hant, ko, ru; anything else gives en-US (NOT VERIFIED: the console decides from the
 * supported-language mask of the application's control data, which was not read here). nl and pt have message files
 * but their text is empty (no subtitles, blank menus: issue #16), so they give en-US too.
 * WT_LANGUAGE=<code> overrides (for example WT_LANGUAGE=en-US). */
static uint64_t desired_language(void) {
    static uint64_t cached;
    static const char *const supported[] = {"ja", "en-US", "en-GB", "fr", "fr-CA", "de", "it", "es", "es-419", "zh-Hans", "zh-Hant", "ko", "ru"};
    if (cached) return cached;
    char name[LOCALE_NAME_MAX_LENGTH * 3] = "", pick[16] = "en-US";
    const char *env = getenv("WT_LANGUAGE");
    wchar_t wname[LOCALE_NAME_MAX_LENGTH] = L"";
    ULONG count = 0, size = 0;
    if (env && env[0]) snprintf(name, sizeof name, "%s", env);
    else {
        /* the display language of the user interface, not the regional format */
        wchar_t list[512];
        size = 512;
        if (GetUserPreferredUILanguages(MUI_LANGUAGE_NAME, &count, list, &size) && count > 0) wcsncpy(wname, list, LOCALE_NAME_MAX_LENGTH - 1);
        else GetUserDefaultLocaleName(wname, LOCALE_NAME_MAX_LENGTH);
        WideCharToMultiByte(CP_UTF8, 0, wname, -1, name, sizeof name, NULL, NULL);
    }
    int exact = 0;
    for (size_t i = 0; i < sizeof supported / sizeof supported[0]; i++) if (!_stricmp(name, supported[i])) { strcpy(pick, supported[i]); exact = 1; }
    if (!exact) {
        if (!_strnicmp(name, "zh", 2)) strcpy(pick, strstr(name, "TW") || strstr(name, "HK") || strstr(name, "MO") || strstr(name, "Hant") ? "zh-Hant" : "zh-Hans");
        else if (!_strnicmp(name, "en", 2)) strcpy(pick, !_stricmp(name, "en-US") || !_stricmp(name, "en-CA") ? "en-US" : "en-GB");
        else if (!_strnicmp(name, "fr", 2)) strcpy(pick, !_stricmp(name, "fr-CA") ? "fr-CA" : "fr");
        else if (!_strnicmp(name, "es", 2)) strcpy(pick, !_stricmp(name, "es-ES") || !_stricmp(name, "es") ? "es" : "es-419");
        else {
            char two[3] = {name[0], name[1], 0};
            for (size_t i = 0; i < sizeof supported / sizeof supported[0]; i++) if (!_stricmp(two, supported[i])) strcpy(pick, supported[i]);
        }
    }
    uint64_t v = 0;
    memcpy(&v, pick, strlen(pick));
    printf("nn::oe::GetDesiredLanguage: Windows display language \"%s\" -> \"%s\"\n", name, pick);
    cached = v;
    return v;
}
static void oe_GetDesiredLanguage(Ctx *c) { RET(desired_language()); }   /* LanguageCode (8 chars) by value in x0 */

/* ---------------------------------------------------------------- nn::err */
/* The console's error viewer shows "Error Code: 2MMM-DDDD" (2000 + module, description) in a dialog and returns when
 * the user closes it. Here it is a Windows message box on the calling thread, which blocks the same way. Scripted
 * runs (WT_SEQ / WT_PLAY / WT_PRESS / WT_SCRIPT) and WT_ERROR_DIALOG=0 only log, so tests never hang. */
static void err_dialog(unsigned category, unsigned number) {
    const char *off = getenv("WT_ERROR_DIALOG");
    char text[160];
    if ((off && off[0] == '0') || getenv("WT_SEQ") || getenv("WT_PLAY") || getenv("WT_PRESS") || getenv("WT_SCRIPT")) return;
    snprintf(text, sizeof text, "Error Code: %04u-%04u\n\nAn error has occurred.", category, number);
    MessageBoxA(NULL, text, "Error", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TASKMODAL);
}
static void err_ShowErrorResult(Ctx *c) {                 /* (Result): the console shows the error applet */
    uint32_t r = (uint32_t)A0;
    err_dialog(2000 + (r & 0x1ff), (r >> 9) & 0x1fff);
    printf("nn::err::ShowError: result 0x%x (module %u, description %u), caller 0x%llx\n", r, r & 0x1ff, (r >> 9) & 0x1fff,
           (unsigned long long)c->x[30] - 4);
    trace_printf("     nn::err::ShowError result 0x%x (module %u, description %u)\n", r, r & 0x1ff, (r >> 9) & 0x1fff);
}

/* ---------------------------------------------------------------- nn::prepo (play reports): accepted and discarded
 * [sdk] PlayReport(): byte @0 = 0, three 8-byte fields @0x20..0x37 = 0. */
static void prepo_PlayReport_ctor(Ctx *c) { wr8(A0, 0); wr64(A0 + 0x20, 0); wr64(A0 + 0x28, 0); wr64(A0 + 0x30, 0); }


/* ---------------------------------------------------------------- nn::util */
static HCRYPTPROV crypt_provider(void) {
    static HCRYPTPROV prov;
    static volatile LONG lock;
    while (InterlockedExchange(&lock, 1)) SwitchToThread();
    if (!prov && !CryptAcquireContextA(&prov, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)) prov = 0;
    InterlockedExchange(&lock, 0);
    return prov;
}
int host_fixed_random(void *buffer, uint64_t size, const char *who, uint64_t lr);      /* os_threads.c: WT_FIXED_SEED */
static void random_bytes(void *p, size_t n) {
    if (host_fixed_random(p, n, "nn::crypto / uuid random bytes", 0)) return;
    HCRYPTPROV prov = crypt_provider();
    if (!prov || !CryptGenRandom(prov, (DWORD)n, p)) for (size_t i = 0; i < n; i++) ((uint8_t *)p)[i] = (uint8_t)rand();
}
/* [sdk] GenerateUuid: 16 random bytes made an RFC 4122 version-4 UUID (byte 6 = 0x4X, byte 8 =
 * 10xxxxxx), returned by value in x0/x1 (16-byte struct; [game] 0x71016fa08c stores x0 and x1). */
static void util_GenerateUuid(Ctx *c) {
    uint8_t b[16];
    random_bytes(b, sizeof b);
    b[6] = (uint8_t)((b[6] & 0x0f) | 0x40);
    b[8] = (uint8_t)((b[8] & 0x3f) | 0x80);
    memcpy(&c->x[0], b, 8);
    memcpy(&c->x[1], b + 8, 8);
}

/* ---------------------------------------------------------------- nn::friends: no friends, no network */
/* An asynchronous request that has already finished (with a failure): its event is signalled. */
static void async_GetSystemEvent(Ctx *c) { os_init_event(A1, 0); os_signal_event(A1); RET(0); }   /* (this, SystemEvent *out) */


/* nn::ldn, nn::nifm leftovers, nn::nsd, nn::ssl, sockets and the nn::ec accessor: nnnet_c4a.inc (included by nnnet.c). */


/* ---------------------------------------------------------------- byte order helpers of nn::socket */
static void socket_swap16(Ctx *c) { RET((uint16_t)(((A0 & 0xff) << 8) | ((A0 >> 8) & 0xff))); }
static void socket_swap32(Ctx *c) { RET(bswap32((uint32_t)A0)); }

/* ---------------------------------------------------------------- registration */
#define OE(n) "_ZN2nn2oe" n
#define PR(n) "_ZN2nn5prepo10PlayReport" n
#define AC(n) "_ZN2nn7account" n
#define SO(n) "_ZN2nn6socket" n
#define LDN(n) "_ZN2nn3ldn" n

/* Replace whatever is registered: exact SDK behaviour for one small class (see nn::err above). */
static const ImportDef replace_imports[] = {
    {"_ZN2nn2oe25TryPopNotificationMessageEPj", oe_TryPopNotificationMessage}, {"_ZN2nn2oe20GetCurrentFocusStateEv", oe_GetCurrentFocusState},
    {"_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", oe_SetFocusHandlingMode},
    {"_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", oe_SetOperationModeChangedNotificationEnabled},
    {"_ZN2nn2oe18GetDesiredLanguageEv", oe_GetDesiredLanguage},
    {"_ZN2nn8settingseqERKNS0_12LanguageCodeERKNS0_8LanguageE", settings_CodeEqualsLanguage},
};

/* Only installed where no other file provides the function. */
static const ImportDef fill_imports[] = {
    /* C wrappers */
      {"nnosGetSystemTick", nnos_GetSystemTick},
    {"nnosGetSystemTickFrequency", nnos_GetSystemTickFrequency}, {"nnosConvertTickToNanoSeconds", nnos_ConvertTickToNanoSeconds},
    {"setjmp", c_setjmp}, {"longjmp", c_longjmp},
    {"_ZN2nn4diag6detail9AbortImplEPKcS3_S3_i", diag_AbortImpl},
    {"_ZN2nn6detail21UnexpectedDefaultImplEPKcS2_i", diag_UnexpectedDefault}, {"__assert_fail", c_assert_fail},
    /* nn::oe */
    {OE("25TryPopNotificationMessageEPj"), oe_TryPopNotificationMessage}, {OE("20GetCurrentFocusStateEv"), oe_GetCurrentFocusState},
     {OE("42SetOperationModeChangedNotificationEnabledEb"), keep},
     {OE("15SetCpuBoostModeENS0_12CpuBoostModeE"), keep},
    /* nn::settings, nn::err */
    {"_ZN2nn8settings12LanguageCode4MakeENS0_8LanguageE", settings_LanguageCode_Make},
    {"_ZN2nn3err9ShowErrorENS_6ResultE", err_ShowErrorResult},
    /* nn::prepo */
    {"_ZN2nn5prepo10InitializeEv", ret0},
    {PR("C1Ev"), prepo_PlayReport_ctor}, {PR("10SetEventIdEPKc"), ret0}, {PR("9SetBufferEPvm"), keep},
    {PR("3AddEPKcRKNS0_10Any64BitIdE"), ret0}, {PR("3AddEPKcl"), ret0},  {PR("3AddEPKcS3_"), ret0},
    {PR("4SaveERKNS_7account3UidE"), ret0},
    /* nn::aoc, nn::pctl, nn::pdm */
    /* close5: both return a BOOL, not a Result: [sdk 0x33a620] `cset w0, eq` on the service Result (slot +0x28), [sdk 0x33aef0] the
     * byte the service writes (slot +0x148). A console without parental controls: true (suyu pctl/parental_control_service.cpp
     * CheckFreeCommunicationPermissionImpl / ConfirmStereoVisionPermissionImpl: no PIN -> true). The old ret0 answered "refused". */
    /* nn::util, nn::crypto */
    {"_ZN2nn4util12GenerateUuidEv", util_GenerateUuid},
    /* nn::fs */
    /* nn::friends */
    /* nn::account */
    {AC("44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE"), acc_nsa_unavailable},
    {AC("12AsyncContext14GetSystemEventEPNS_2os11SystemEventE"), async_GetSystemEvent},
    {AC("37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE"), acc_nsa_unavailable},
    {AC("26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE"), acc_nsa_unavailable},
    /* nn::nifm, nn::nsd, nn::ssl */
    /* sockets */
    {SO("9InetHtonsEt"), socket_swap16}, {SO("9InetNtohsEt"), socket_swap16}, {SO("9InetHtonlEj"), socket_swap32},
    /* nn::ldn */
    /* nn::ec, nn::album, nn::ngc */
};

/* WT_DUMP_UNREG=<file>: write the names of all imports that still have no handler. */
static void dump_unregistered(void) {
    const char *path = getenv("WT_DUMP_UNREG");
    if (!path || !path[0]) return;
    FILE *f = fopen(path, "w");
    if (!f) return;
    for (int i = 0; i < dyn_sym_count; i++)
        if (!dyn_syms[i].defined && dyn_syms[i].name[0] && !import_lookup(dyn_syms[i].name)) fprintf(f, "%s\n", dyn_syms[i].name);
    fclose(f);
}

/* close5 (lead) self-test: the image's text is read-only, its data writable (hostrt.c runtime_init). */
int close5_lead_selftest(Ctx *c) {
    int bad = 0, n = 0;
    (void)c;
    if (!getenv("WT_NO_PROTECT")) {
        n += 3;
        if (!IsBadWritePtr((void *)(uintptr_t)0x7100000000ull, 1)) { bad++; printf("  close5 lead: the first text page is writable\n"); }
        if (!IsBadWritePtr((void *)(uintptr_t)(GUEST_BASE + GUEST_RO_END - 1), 1)) { bad++; printf("  close5 lead: the last read-only data byte is writable\n"); }
        if (IsBadWritePtr((void *)(uintptr_t)(GUEST_BASE + GUEST_RO_END), 1) || IsBadReadPtr((void *)(uintptr_t)0x7100000000ull, 16)) { bad++; printf("  close5 lead: data segment not writable or text not readable\n"); }
    }
    printf("  close5 lead (image protection): %d cases, %d failed\n", n, bad);
    return bad;
}

void register_misc_imports(void) {
    QueryPerformanceCounter(&g_start);
    InitializeCriticalSection(&g_oe_lock);
    register_imports(replace_imports, (int)(sizeof replace_imports / sizeof replace_imports[0]));
    int n = (int)(sizeof fill_imports / sizeof fill_imports[0]), added = 0;
    for (int i = 0; i < n; i++) {
        if (!import_data(fill_imports[i].name)) { printf("nnmisc: \"%s\" is not an import of the game (typo?)\n", fill_imports[i].name); continue; }
        if (!import_lookup(fill_imports[i].name)) { register_imports(&fill_imports[i], 1); added++; }
    }
    for (int i = 0; i < (int)(sizeof replace_imports / sizeof replace_imports[0]); i++)
        if (!import_data(replace_imports[i].name)) printf("nnmisc: \"%s\" is not an import of the game (typo?)\n", replace_imports[i].name);
    trace_printf("nnmisc: %d of %d functions installed (the rest are provided elsewhere or not imported)\n", added, n);
    dump_unregistered();
    (void)ret1;
}
