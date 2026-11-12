/* Linux build only: Windows features this platform does not have (the Win32 window and GDI, dialogs, waveOut, Media
 * Foundation, the registry, raw input, CryptoAPI handles). Every call fails or does nothing, the way the Windows call is
 * allowed to fail, so the runtime's existing fallbacks run; the Linux back ends that replace them come in later steps
 * (window and Vulkan surface, audio, movies, fonts). Types carry only the fields the runtime touches. */
#ifndef RECOMP_WIN32_ABSENT_H
#define RECOMP_WIN32_ABSENT_H
#include "plat.h"

/* ---------------------------------------------------------------- handles and messages */
typedef void *HWND, *HDC, *HFONT, *HGLRC, *HICON, *HMONITOR, *HCURSOR, *HGDIOBJ, *HRAWINPUT, *HKEY, *HBITMAP, *HBRUSH, *HMENU;
typedef intptr_t LPARAM, LRESULT;
typedef uintptr_t WPARAM;
typedef LRESULT (*WNDPROC)(HWND, UINT, WPARAM, LPARAM);
typedef intptr_t (*DLGPROC)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL (*WNDENUMPROC)(HWND, LPARAM);
typedef struct { LONG left, top, right, bottom; } RECT;
typedef struct { LONG x, y; } POINT;
typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; } MSG;
typedef struct { UINT style; WNDPROC lpfnWndProc; int cbClsExtra, cbWndExtra; HINSTANCE hInstance; HICON hIcon; HCURSOR hCursor; void *hbrBackground;
                 LPCSTR lpszMenuName, lpszClassName; } WNDCLASSA;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; char szDevice[32]; } MONITORINFOEXA;
typedef struct { DWORD style, dwExtendedStyle; WORD cdit; short x, y, cx, cy; } DLGTEMPLATE;
typedef struct { DWORD style, dwExtendedStyle; short x, y, cx, cy; WORD id; } DLGITEMTEMPLATE;
#define LOWORD(l) ((WORD)((uintptr_t)(l) & 0xffff))
#define HIWORD(l) ((WORD)(((uintptr_t)(l) >> 16) & 0xffff))
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define MulDiv(a, b, c) ((int)((int64_t)(a) * (b) / (c)))
#define CW_USEDEFAULT ((int)0x80000000)
#define WS_OVERLAPPEDWINDOW 0x00CF0000u
#define WS_POPUP 0x80000000u
#define WS_VISIBLE 0x10000000u
#define WS_BORDER 0x00800000u
#define WS_TABSTOP 0x00010000u
#define WS_EX_TOPMOST 0x8u
#define WS_EX_TOOLWINDOW 0x80u
#define WS_EX_NOACTIVATE 0x08000000u
#define CS_OWNDC 0x20u
#define SS_LEFT 0u
#define BS_PUSHBUTTON 0u
#define BS_DEFPUSHBUTTON 1u
#define ES_AUTOHSCROLL 0x80u
#define EM_SETSEL 0xB1u
#define EM_SETLIMITTEXT 0xC5u
#define EM_SETPASSWORDCHAR 0xCCu
#define WM_MOVE 3u
#define WM_SIZE 5u
#define WM_ACTIVATE 6u
#define WM_CLOSE 0x10u
#define WM_ERASEBKGND 0x14u
#define WM_SETCURSOR 0x20u
#define WM_SETICON 0x80u
#define WM_INPUT 0xFFu
#define WM_KEYDOWN 0x100u
#define WM_INITDIALOG 0x110u
#define WM_COMMAND 0x111u
#define WM_LBUTTONDOWN 0x201u
#define WM_RBUTTONDOWN 0x204u
#define WM_MBUTTONDOWN 0x207u
#define WM_XBUTTONDOWN 0x20Bu
#define WM_APP 0x8000u
#define HTCLIENT 1
#define ICON_SMALL 0
#define IDC_ARROW ((LPCSTR)(uintptr_t)32512)
#define GWLP_USERDATA (-21)
#define GW_OWNER 4u
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNOACTIVATE 4
#define SWP_NOZORDER 4u
#define SWP_NOACTIVATE 0x10u
#define MONITOR_DEFAULTTOPRIMARY 1u
#define MONITOR_DEFAULTTONEAREST 2u
#define IDOK 1
#define IDCANCEL 2
#define IDYES 6
#define MB_OK 0u
#define MB_YESNO 4u
#define MB_ICONERROR 0x10u
#define MB_ICONQUESTION 0x20u
#define MB_ICONWARNING 0x30u
#define MB_ICONINFORMATION 0x40u
#define MB_SETFOREGROUND 0x10000u
#define MB_TOPMOST 0x40000u
#define MB_TASKMODAL 0x2000u
#define CreateWindowExA(...) ((HWND)NULL)
#define CreateWindowExW(...) ((HWND)NULL)
#define RegisterClassA(wc) ((WORD)0)
#define DefWindowProcA(h, m, w, l) ((LRESULT)0)
#define GetMessageA(m, h, a, b) 0
#define TranslateMessage(m) FALSE
#define DispatchMessageA(m) ((LRESULT)0)
#define PostMessageA(...) FALSE
#define PostMessageW(...) FALSE
#define SendMessageA(...) ((LRESULT)0)
#define SendDlgItemMessageW(...) ((LRESULT)0)
#define ShowWindow(h, s) FALSE
#define SetWindowPos(...) FALSE
#define SetWindowLongPtrW(h, i, v) ((LONG_PTR)0)
#define GetWindowLongPtrW(h, i) ((LONG_PTR)0)
/* an HWND is an SDL_Window here: these are display.c's, on SDL (the keyboard and mouse state too) */
BOOL GetClientRect(HWND h, RECT *r);
BOOL IsIconic(HWND h);
int GetWindowTextA(HWND h, LPSTR out, int n);
BOOL SetWindowTextA(HWND h, LPCSTR t);
HWND GetForegroundWindow(void);
DWORD GetWindowThreadProcessId(HWND h, DWORD *pid);
SHORT GetAsyncKeyState(int vk);
#define GetWindowRect(h, r) (memset((r), 0, sizeof *(r)), FALSE)
#define AdjustWindowRect(r, s, m) TRUE
#define MapWindowPoints(a, b, p, n) 0
#define IsWindow(h) FALSE
#define IsWindowVisible(h) FALSE
#define SetForegroundWindow(h) FALSE
#define SetFocus(h) ((HWND)NULL)
#define GetWindow(h, c) ((HWND)NULL)
#define FindWindowW(c, n) ((HWND)NULL)
#define EnumWindows(fn, l) FALSE
#define GetDlgItem(h, id) ((HWND)NULL)
#define GetDlgItemTextW(h, id, s, n) 0
#define SetDlgItemTextW(h, id, s) FALSE
#define EndDialog(h, r) TRUE
#define DialogBoxIndirectParamW(i, t, p, fn, l) ((intptr_t)-1)
#define MonitorFromWindow(h, f) ((HMONITOR)NULL)
#define MonitorFromPoint(p, f) ((HMONITOR)NULL)
#define GetMonitorInfoA(m, mi) FALSE
#define ClipCursor(r) TRUE
#define SetCursor(c) ((HCURSOR)NULL)
#define LoadCursor(i, n) ((HCURSOR)NULL)
#define SetProcessDPIAware() TRUE
#define IsProcessDPIAware() TRUE
#define GetModuleHandleW(name) ((HMODULE)NULL)
#define lstrcmpiW wcscasecmp
#define _snwprintf swprintf
#define MessageBeep(t) TRUE
/* a message box nobody can see: the text goes to the log, the answer is the default (OK / no) */
#define MessageBoxA(h, text, title, t) (fprintf(stderr, "[message box] %s: %s\n", (const char *)(title), (const char *)(text)), ((t) & MB_YESNO) ? 7 : IDOK)
#define MessageBoxW(h, text, title, t) (fprintf(stderr, "[message box] %ls: %ls\n", (const wchar_t *)(title), (const wchar_t *)(text)), ((t) & MB_YESNO) ? 7 : IDOK)

#define WS_CHILD 0x40000000u
#define WS_CAPTION 0x00C00000u
#define WS_SYSMENU 0x00080000u
#define DS_MODALFRAME 0x80u
#define DS_CENTER 0x800u
#define DS_SETFONT 0x40u
#define DS_SETFOREGROUND 0x200u
#define IMAGE_ICON 1u
#define SM_CXICON 11
#define SM_CYICON 12
#define MAKEINTRESOURCEA(i) ((LPCSTR)(uintptr_t)(WORD)(i))
#define LoadImageA(...) ((HANDLE)NULL)
#define GetSystemMetrics(i) 0
/* the critical-section fields the lock diagnostics print, on glibc's mutex */
#ifdef __APPLE__
#define OwningThread __sig & 0                /* macOS's mutex does not show its owner: nobody (diagnostics only) */
#define RecursionCount __sig & 0
#define LockCount __sig & 0
#else
#define OwningThread __data.__owner
#define RecursionCount __data.__count
#define LockCount __data.__lock
#endif

/* ---------------------------------------------------------------- keyboard and raw mouse input (display.c answers on SDL3) */
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_NUMPAD0 0x60
#define VK_F1 0x70
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define KEYEVENTF_KEYUP 2u
#define keybd_event(...) ((void)0)
typedef struct { DWORD dwType, dwSize; HANDLE hDevice; WPARAM wParam; } RAWINPUTHEADER;
typedef struct { USHORT usFlags; union { ULONG ulButtons; struct { USHORT usButtonFlags, usButtonData; }; }; ULONG ulRawButtons; LONG lLastX, lLastY; ULONG ulExtraInformation; } RAWMOUSE_;
typedef struct { RAWINPUTHEADER header; union { RAWMOUSE_ mouse; } data; } RAWINPUT;
typedef struct { USHORT usUsagePage, usUsage; DWORD dwFlags; HWND hwndTarget; } RAWINPUTDEVICE;
#define RID_INPUT 0x10000003u
#define RIM_TYPEMOUSE 0u
#define MOUSE_MOVE_ABSOLUTE 1u
#define RegisterRawInputDevices(d, n, s) FALSE
#define GetRawInputData(h, c, d, s, hs) ((UINT)-1)

/* ---------------------------------------------------------------- GDI and WGL (fonts: fontconfig later; GL: Linux renders with Vulkan) */
typedef struct { WORD nSize, nVersion; DWORD dwFlags; BYTE iPixelType, cColorBits, cRedBits, cRedShift, cGreenBits, cGreenShift, cBlueBits, cBlueShift,
                 cAlphaBits, cAlphaShift, cAccumBits, cAccumRedBits, cAccumGreenBits, cAccumBlueBits, cAccumAlphaBits, cDepthBits, cStencilBits,
                 cAuxBuffers, iLayerType, bReserved; DWORD dwLayerMask, dwVisibleMask, dwDamageMask; } PIXELFORMATDESCRIPTOR;
#define PFD_DOUBLEBUFFER 1u
#define PFD_DRAW_TO_WINDOW 4u
#define PFD_SUPPORT_OPENGL 0x20u
#define PFD_TYPE_RGBA 0
#define GetDC(h) ((HDC)NULL)
#define ReleaseDC(h, dc) 0
#define CreateDCA(...) ((HDC)NULL)
#define CreateCompatibleDC(dc) ((HDC)NULL)
#define SelectObject(dc, o) ((HGDIOBJ)NULL)
#define GetDeviceCaps(dc, i) 96
#define LOGPIXELSX 88
#define ChoosePixelFormat(dc, p) 0
#define SetPixelFormat(dc, f, p) FALSE
#define SwapBuffers(dc) FALSE
#define wglCreateContext(dc) ((HGLRC)NULL)
#define wglMakeCurrent(dc, rc) FALSE
#define wglDeleteContext(rc) FALSE
typedef struct { WORD fract; short value; } FIXED;
typedef struct { FIXED x, y; } POINTFX;
typedef struct { FIXED eM11, eM12, eM21, eM22; } MAT2;
typedef struct { UINT gmBlackBoxX, gmBlackBoxY; POINT gmptGlyphOrigin; short gmCellIncX, gmCellIncY; } GLYPHMETRICS;
typedef struct { DWORD cb, dwType; POINTFX pfxStart; } TTPOLYGONHEADER;
typedef struct { WORD wType, cpfx; POINTFX apfx[1]; } TTPOLYCURVE;
#define TT_PRIM_LINE 1
#define TT_PRIM_CSPLINE 3
#define GGO_BEZIER 3u
#define GGO_UNHINTED 0x100u
#define GGO_GLYPH_INDEX 0x80u
#define GDI_ERROR 0xFFFFFFFFu
#define LF_FACESIZE 32
#define FW_DONTCARE 0
#define DEFAULT_CHARSET 1
#define OUT_DEFAULT_PRECIS 0
#define CLIP_DEFAULT_PRECIS 0
#define ANTIALIASED_QUALITY 4
#define DEFAULT_PITCH 0
#define CreateFontW(...) ((HFONT)NULL)
#define GetFontData(dc, t, o, b, n) GDI_ERROR
#define GetGlyphOutlineW(...) GDI_ERROR
#define GetTextFaceW(dc, n, s) 0
#define AddFontMemResourceEx(p, n, r, c) ((HANDLE)NULL)
typedef struct __attribute__((packed)) { WORD bfType; DWORD bfSize; WORD bfReserved1, bfReserved2; DWORD bfOffBits; } BITMAPFILEHEADER;
typedef struct { DWORD biSize; LONG biWidth, biHeight; WORD biPlanes, biBitCount; DWORD biCompression, biSizeImage; LONG biXPelsPerMeter, biYPelsPerMeter;
                 DWORD biClrUsed, biClrImportant; } BITMAPINFOHEADER;
#define BI_RGB 0u

/* ---------------------------------------------------------------- waveOut (on SDL3 audio: plat_audio_sdl.c) */
typedef void *HWAVEOUT;
typedef UINT MMRESULT;
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec; WORD nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX;
typedef struct WAVEHDR { char *lpData; DWORD dwBufferLength, dwBytesRecorded; DWORD_PTR dwUser; DWORD dwFlags, dwLoops; struct WAVEHDR *lpNext; DWORD_PTR reserved; } WAVEHDR;
typedef struct { UINT wType; union { DWORD ms, sample, cb, ticks; } u; } MMTIME;
#define WAVE_FORMAT_PCM 1
#define WAVE_MAPPER ((UINT)-1)
#define CALLBACK_EVENT 0x50000u
#define WHDR_DONE 1u
#define TIME_SAMPLES 2u
#define MMSYSERR_NOERROR 0u
#define MMSYSERR_NODRIVER 6u
#define TIMERR_NOERROR 0u
/* plat_audio_sdl.c: on SDL3 audio */
MMRESULT waveOutOpen(HWAVEOUT *out, UINT dev, const WAVEFORMATEX *f, DWORD_PTR cb, DWORD_PTR inst, DWORD flags);
MMRESULT waveOutClose(HWAVEOUT h);
MMRESULT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n);
MMRESULT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n);
MMRESULT waveOutWrite(HWAVEOUT h, WAVEHDR *hdr, UINT n);
MMRESULT waveOutReset(HWAVEOUT h);
MMRESULT waveOutPause(HWAVEOUT h);
MMRESULT waveOutRestart(HWAVEOUT h);
MMRESULT waveOutSetVolume(HWAVEOUT h, DWORD v);
MMRESULT waveOutGetPosition(HWAVEOUT h, MMTIME *t, UINT n);
#define timeBeginPeriod(ms) TIMERR_NOERROR
#define timeEndPeriod(ms) TIMERR_NOERROR

/* ---------------------------------------------------------------- COM and Media Foundation (FFmpeg later) */
typedef struct { uint32_t Data1; uint16_t Data2, Data3; uint8_t Data4[8]; } GUID;
typedef GUID IID, CLSID;
typedef const GUID *REFGUID, *REFIID;
static const GUID GUID_NULL;
#define IsEqualGUID(a, b) (memcmp((a), (b), sizeof(GUID)) == 0)
typedef struct { DWORD LowPart; LONG HighPart; } LUID;
#define E_FAIL ((HRESULT)0x80004005)
#define E_INVALIDARG ((HRESULT)0x80070057)
#define E_NOTIMPL ((HRESULT)0x80004001)
#define COINIT_MULTITHREADED 0u
#define CLSCTX_ALL 0x17u
#define CoInitializeEx(p, f) E_NOTIMPL
#define CoUninitialize() ((void)0)
#define CoCreateInstance(...) E_NOTIMPL
#define STDMETHODCALLTYPE
#define E_NOINTERFACE ((HRESULT)0x80004002)
typedef struct { GUID fmtid; DWORD pid; } PROPERTYKEY;
typedef struct { WORD vt; WORD r1, r2, r3; union { int64_t hVal; uint64_t uhVal; }; } PROPVARIANT;
#define VT_I8 20
#define VT_UI8 21
#define PropVariantInit(p) memset((p), 0, sizeof *(p))
#define PropVariantClear(p) S_OK
typedef struct IStream IStream;
#define IStream_Release(s) 0u
#define SHCreateMemStream(p, n) ((IStream *)NULL)
typedef struct IMFAttributes IMFAttributes;
typedef struct IMFMediaType IMFMediaType;
typedef struct IMFSample IMFSample;
typedef struct IMFMediaBuffer IMFMediaBuffer;
typedef struct IMF2DBuffer IMF2DBuffer;
typedef struct IMFByteStream IMFByteStream;
typedef struct IMFSinkWriter IMFSinkWriter;
typedef struct IMFSourceReader IMFSourceReader;
#define MF_VERSION 0x20070u
#define MFSTARTUP_FULL 0u
#define MFStartup(v, f) E_NOTIMPL
#define MFShutdown() S_OK
#define MFCreateMediaType(t) E_NOTIMPL
#define MFCreateSample(s) E_NOTIMPL
#define MFCreateMemoryBuffer(n, b) E_NOTIMPL
#define MFCreateSinkWriterFromURL(u, b, a, w) E_NOTIMPL
#define MFCreateAttributes(a, n) E_NOTIMPL
#define MFCreateMFByteStreamOnStream(s, b) E_NOTIMPL
#define MFCreateSourceReaderFromByteStream(b, a, r) E_NOTIMPL
#define MFVideoInterlace_Progressive 2u
static const GUID MF_MT_MAJOR_TYPE, MF_MT_SUBTYPE, MF_MT_FRAME_SIZE, MF_MT_FRAME_RATE, MF_MT_PIXEL_ASPECT_RATIO, MF_MT_INTERLACE_MODE,
    MF_MT_AVG_BITRATE, MF_MT_AUDIO_NUM_CHANNELS, MF_MT_AUDIO_SAMPLES_PER_SECOND, MF_MT_AUDIO_BITS_PER_SAMPLE, MF_MT_AUDIO_BLOCK_ALIGNMENT,
    MF_MT_AUDIO_AVG_BYTES_PER_SECOND, MFMediaType_Audio, MFMediaType_Video, MFAudioFormat_PCM, MFAudioFormat_AAC, MFVideoFormat_NV12,
    MFVideoFormat_H264, MF_BYTESTREAM_CONTENT_TYPE, MF_BYTESTREAM_ORIGIN_NAME, MF_PD_DURATION, IID_IMF2DBuffer, IID_IMFAttributes;
#define IMFAttributes_Release(o) 0u
#define IMFAttributes_SetString(...) E_NOTIMPL
#define IMFMediaType_Release(o) 0u
#define IMFMediaType_SetGUID(...) E_NOTIMPL
#define IMFMediaType_GetGUID(...) E_NOTIMPL
#define IMFMediaType_SetUINT32(...) E_NOTIMPL
#define IMFMediaType_GetUINT32(...) E_NOTIMPL
#define IMFMediaType_SetUINT64(...) E_NOTIMPL
#define IMFMediaType_GetUINT64(...) E_NOTIMPL
#define IMFSample_Release(o) 0u
#define IMFSample_AddBuffer(...) E_NOTIMPL
#define IMFSample_SetSampleTime(...) E_NOTIMPL
#define IMFSample_SetSampleDuration(...) E_NOTIMPL
#define IMFSample_ConvertToContiguousBuffer(...) E_NOTIMPL
#define IMFMediaBuffer_Release(o) 0u
#define IMFMediaBuffer_Lock(...) E_NOTIMPL
#define IMFMediaBuffer_Unlock(o) E_NOTIMPL
#define IMFMediaBuffer_SetCurrentLength(...) E_NOTIMPL
#define IMFMediaBuffer_QueryInterface(...) E_NOTIMPL
#define IMF2DBuffer_Release(o) 0u
#define IMF2DBuffer_Lock2D(...) E_NOTIMPL
#define IMF2DBuffer_Unlock2D(o) E_NOTIMPL
#define IMFByteStream_Release(o) 0u
#define IMFByteStream_QueryInterface(...) E_NOTIMPL
#define IMFSinkWriter_Release(o) 0u
#define IMFSinkWriter_AddStream(...) E_NOTIMPL
#define IMFSinkWriter_SetInputMediaType(...) E_NOTIMPL
#define IMFSinkWriter_BeginWriting(o) E_NOTIMPL
#define IMFSinkWriter_WriteSample(...) E_NOTIMPL
#define IMFSinkWriter_Finalize(o) E_NOTIMPL

/* ---------------------------------------------------------------- registry, locale, power, processor sets */
#define HKEY_LOCAL_MACHINE ((HKEY)(uintptr_t)0x80000002)
#define HKEY_CURRENT_USER ((HKEY)(uintptr_t)0x80000001)
#define SHGetValueA(...) ((LONG)ERROR_FILE_NOT_FOUND)
#define SHGetValueW(...) ((LONG)ERROR_FILE_NOT_FOUND)
typedef struct { LONG Bias; WCHAR StandardName[32]; SYSTEMTIME StandardDate; LONG StandardBias; WCHAR DaylightName[32]; SYSTEMTIME DaylightDate; LONG DaylightBias;
                 WCHAR TimeZoneKeyName[128]; BOOLEAN DynamicDaylightTimeDisabled; } DYNAMIC_TIME_ZONE_INFORMATION;
/* shortcut: the zone's current offset only, no daylight-saving transitions (milestone 4: read /etc/localtime, a TZif file nntime.c can parse) */
static inline DWORD GetDynamicTimeZoneInformation(DYNAMIC_TIME_ZONE_INFORMATION *d) {
    memset(d, 0, sizeof *d);
    time_t now = time(NULL);
    struct tm lt;
    localtime_r(&now, &lt);
    d->Bias = (LONG)(-lt.tm_gmtoff / 60);      /* Windows: UTC = local + Bias minutes */
    d->DynamicDaylightTimeDisabled = 1;
    return 0;                                  /* TIME_ZONE_ID_UNKNOWN */
}
#define LOCALE_NAME_MAX_LENGTH 85
#define MUI_LANGUAGE_NAME 8u
/* the user's language from LANG / LC_ALL ("fr_FR.UTF-8" -> "fr-FR") */
static inline int GetUserDefaultLocaleName(WCHAR *out, int n) {
    const char *l = getenv("LC_ALL");
    if (!l || !*l) l = getenv("LC_MESSAGES");
    if (!l || !*l) l = getenv("LANG");
    if (!l || !*l || !strcmp(l, "C") || !strcmp(l, "POSIX")) l = "en_US";
    int k = 0;
    for (; l[k] && l[k] != '.' && l[k] != '@' && k < n - 1; k++) out[k] = l[k] == '_' ? L'-' : (WCHAR)(unsigned char)l[k];
    out[k] = 0;
    return k + 1;
}
static inline BOOL GetUserPreferredUILanguages(DWORD flags, ULONG *count, WCHAR *buf, ULONG *n) {
    (void)flags;
    WCHAR one[LOCALE_NAME_MAX_LENGTH];
    int k = GetUserDefaultLocaleName(one, LOCALE_NAME_MAX_LENGTH);
    if (count) *count = 1;
    if (!buf || *n < (ULONG)k + 1) { *n = (ULONG)k + 1; return buf == NULL; }
    memcpy(buf, one, sizeof(WCHAR) * (size_t)k);
    buf[k] = 0;                                /* a double-terminated list */
    *n = (ULONG)k + 1;
    return TRUE;
}
#define ES_SYSTEM_REQUIRED 1u
#define ES_DISPLAY_REQUIRED 2u
#define ES_CONTINUOUS 0x80000000u
#define SetThreadExecutionState(f) ((DWORD)0)   /* shortcut: the screensaver is not held off (later: SDL_DisableScreenSaver) */
typedef struct { ULONG Version, ControlMask, StateMask; } PROCESS_POWER_THROTTLING_STATE;
#define PROCESS_POWER_THROTTLING_CURRENT_VERSION 1u
#define PROCESS_POWER_THROTTLING_EXECUTION_SPEED 1u
#define ProcessPowerThrottling 4
#define SetProcessInformation(...) FALSE
typedef enum { CpuSetInformation } CPU_SET_INFORMATION_TYPE;
typedef struct { DWORD Size; CPU_SET_INFORMATION_TYPE Type; struct { DWORD Id; WORD Group; BYTE LogicalProcessorIndex, CoreIndex, LastLevelCacheIndex, NumaNodeIndex, EfficiencyClass, AllFlags; DWORD Reserved; DWORD64 AllocationTag; } CpuSet; } SYSTEM_CPU_SET_INFORMATION, *PSYSTEM_CPU_SET_INFORMATION;
#define GetSystemCpuSetInformation(...) FALSE
#define SetThreadSelectedCpuSets(...) FALSE
typedef struct { DWORD cb, PageFaultCount; SIZE_T PeakWorkingSetSize, WorkingSetSize, QuotaPeakPagedPoolUsage, QuotaPagedPoolUsage,
                 QuotaPeakNonPagedPoolUsage, QuotaNonPagedPoolUsage, PagefileUsage, PeakPagefileUsage, PrivateUsage; } PROCESS_MEMORY_COUNTERS_EX;
#define K32GetProcessMemoryInfo GetProcessMemoryInfo
#define GetDiskFreeSpaceExA(path, avail, total, free_) FALSE
#define MEM_IMAGE 0x1000000u
typedef RUNTIME_FUNCTION *PRUNTIME_FUNCTION;
/* PE headers: the Vulkan code reads them only to find vulkan-1.dll's exports, which does not exist here */
typedef struct { WORD e_magic; WORD r[29]; LONG e_lfanew; } IMAGE_DOS_HEADER;
typedef struct { DWORD VirtualAddress, Size; } IMAGE_DATA_DIRECTORY;
typedef struct { BYTE r[108]; DWORD NumberOfRvaAndSizes; IMAGE_DATA_DIRECTORY DataDirectory[16]; } IMAGE_OPTIONAL_HEADER64;
typedef struct { DWORD Signature; BYTE FileHeader[20]; IMAGE_OPTIONAL_HEADER64 OptionalHeader; } IMAGE_NT_HEADERS64;
typedef struct { DWORD Characteristics, TimeDateStamp; WORD MajorVersion, MinorVersion; DWORD Name, Base, NumberOfFunctions, NumberOfNames,
                 AddressOfFunctions, AddressOfNames, AddressOfNameOrdinals; } IMAGE_EXPORT_DIRECTORY;
#define IMAGE_DOS_SIGNATURE 0x5A4D
#define IMAGE_NT_SIGNATURE 0x00004550u

/* ---------------------------------------------------------------- CryptoAPI: SHA-1, MD5 and random bytes (plat_posix.c, plat_hash.h) */
typedef uintptr_t HCRYPTPROV, HCRYPTHASH;
#define PROV_RSA_FULL 1u
#define PROV_RSA_AES 24u
#define CRYPT_VERIFYCONTEXT 0xF0000000u
#define CALG_MD5 0x8003u
#define CALG_SHA1 0x8004u
#define HP_HASHVAL 2u
BOOL CryptAcquireContextA(HCRYPTPROV *p, LPCSTR c, LPCSTR prov, DWORD type, DWORD flags);
BOOL CryptReleaseContext(HCRYPTPROV p, DWORD flags);
BOOL CryptGenRandom(HCRYPTPROV p, DWORD n, BYTE *out);
BOOL CryptCreateHash(HCRYPTPROV p, UINT alg, HCRYPTPROV key, DWORD flags, HCRYPTHASH *h);
BOOL CryptHashData(HCRYPTHASH h, const BYTE *data, DWORD n, DWORD flags);
BOOL CryptGetHashParam(HCRYPTHASH h, DWORD param, BYTE *out, DWORD *n, DWORD flags);
BOOL CryptDestroyHash(HCRYPTHASH h);
#endif
