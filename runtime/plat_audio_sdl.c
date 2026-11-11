/* Linux build only: the waveOut subset nnaudio.c uses, on an SDL3 audio stream. A header is "done" (WHDR_DONE, and the
 * CALLBACK_EVENT event is set) once the device has pulled all of it; the position counts the frames pulled so far. */
#ifndef _WIN32
#include <SDL3/SDL.h>
#include "posix/win32_absent.h"

typedef struct {
    SDL_AudioStream *stream;
    HANDLE event;
    SDL_Mutex *lock;
    WAVEHDR *queue[256];               /* written, not yet fully pulled */
    unsigned head, tail;
    DWORD offset;                      /* bytes of queue[head] already pulled */
    uint64_t frames;                   /* pulled since open / the last reset */
    unsigned block;                    /* bytes per frame */
    unsigned told;                     /* WT_AUDIO_STEP_MS: headers before this one have been reported done */
    Uint64 next_step;
} WaveOut;

/* TEST ONLY, WT_AUDIO_STEP_MS=<n> (issue 157): a device that plays at once but reports its finished headers only every
 * n ms, all together. 60 gives that report's figures: 667 buffers in 10 s instead of 937, an underrun every 4 buffers. */
static int step_ms(void) { static int v = -1; if (v < 0) { const char *e = getenv("WT_AUDIO_STEP_MS"); v = e ? atoi(e) : 0; } return v; }

static void SDLCALL pull(void *user, SDL_AudioStream *stream, int need, int total) {
    (void)total;
    WaveOut *w = user;
    int done = 0, step = step_ms();
    SDL_LockMutex(w->lock);
    while (need > 0 && w->head != w->tail) {
        WAVEHDR *h = w->queue[w->head % 256];
        int n = (int)(h->dwBufferLength - w->offset);
        if (n > need) n = need;
        SDL_PutAudioStreamData(stream, h->lpData + w->offset, n);
        w->offset += (DWORD)n; need -= n;
        w->frames += (uint64_t)n / w->block;
        if (w->offset == h->dwBufferLength) { w->head++; w->offset = 0; }
    }
    if (!step || SDL_GetTicks() >= w->next_step) {
        w->next_step = SDL_GetTicks() + (Uint64)step;
        for (; w->told != w->head; w->told++, done = 1) w->queue[w->told % 256]->dwFlags |= WHDR_DONE;
    }
    SDL_UnlockMutex(w->lock);
    if (done && w->event) SetEvent(w->event);
}

MMRESULT waveOutOpen(HWAVEOUT *out, UINT dev, const WAVEFORMATEX *f, DWORD_PTR cb, DWORD_PTR inst, DWORD flags) {
    (void)dev; (void)inst;
    *out = NULL;
    if (f->wFormatTag != WAVE_FORMAT_PCM || f->wBitsPerSample != 16) return 32;   /* WAVERR_BADFORMAT: the runtime asks for 16-bit PCM only */
    if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) { printf("nn::audio: SDL audio cannot start: %s\n", SDL_GetError()); return MMSYSERR_NODRIVER; }
    WaveOut *w = SDL_calloc(1, sizeof *w);
    if (!w) return 7;                                  /* MMSYSERR_NOMEM */
    w->event = (flags & CALLBACK_EVENT) ? (HANDLE)cb : NULL;
    w->block = f->nBlockAlign;
    w->lock = SDL_CreateMutex();
    SDL_AudioSpec spec = { SDL_AUDIO_S16LE, f->nChannels, (int)f->nSamplesPerSec };
    w->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, pull, w);
    if (!w->stream) { printf("nn::audio: no SDL playback device: %s\n", SDL_GetError()); SDL_DestroyMutex(w->lock); SDL_free(w); return MMSYSERR_NODRIVER; }
    SDL_ResumeAudioStreamDevice(w->stream);           /* waveOutOpen plays at once (SDL opens paused) */
    printf("nn::audio: SDL audio %s, %d Hz, %d channels\n", SDL_GetCurrentAudioDriver(), spec.freq, spec.channels);
    *out = w;
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n) { (void)h; (void)n; hdr->dwFlags &= ~WHDR_DONE; return MMSYSERR_NOERROR; }
MMRESULT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n) { (void)h; (void)n; (void)hdr; return MMSYSERR_NOERROR; }
MMRESULT waveOutWrite(HWAVEOUT h, WAVEHDR *hdr, UINT n) {
    (void)n;
    WaveOut *w = h;
    MMRESULT r = MMSYSERR_NOERROR;
    SDL_LockMutex(w->lock);
    if (w->tail - w->head >= 256) r = 7;               /* never at the runtime's at most 64 slots */
    else { hdr->dwFlags &= ~WHDR_DONE; w->queue[w->tail++ % 256] = hdr; }
    SDL_UnlockMutex(w->lock);
    return r;
}
MMRESULT waveOutPause(HWAVEOUT h) { return SDL_PauseAudioStreamDevice(((WaveOut *)h)->stream) ? MMSYSERR_NOERROR : MMSYSERR_NODRIVER; }
MMRESULT waveOutRestart(HWAVEOUT h) { return SDL_ResumeAudioStreamDevice(((WaveOut *)h)->stream) ? MMSYSERR_NOERROR : MMSYSERR_NODRIVER; }
MMRESULT waveOutReset(HWAVEOUT h) {                    /* every queued header becomes done, the position goes back to 0 */
    WaveOut *w = h;
    SDL_LockMutex(w->lock);
    for (; w->told != w->head; w->told++) w->queue[w->told % 256]->dwFlags |= WHDR_DONE;
    for (; w->head != w->tail; w->head++) w->queue[w->head % 256]->dwFlags |= WHDR_DONE;
    w->told = w->head;
    w->offset = 0; w->frames = 0;
    SDL_ClearAudioStream(w->stream);
    SDL_UnlockMutex(w->lock);
    if (w->event) SetEvent(w->event);
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutSetVolume(HWAVEOUT h, DWORD v) { return SDL_SetAudioStreamGain(((WaveOut *)h)->stream, (float)(v & 0xffff) / 65535.0f) ? MMSYSERR_NOERROR : MMSYSERR_NODRIVER; }
MMRESULT waveOutGetPosition(HWAVEOUT h, MMTIME *t, UINT n) {
    (void)n;
    WaveOut *w = h;
    SDL_LockMutex(w->lock);
    uint64_t pulled = w->frames;
    SDL_UnlockMutex(w->lock);
    int queued = SDL_GetAudioStreamQueued(w->stream);  /* pulled into the stream but not yet given to the device */
    uint64_t q = queued > 0 ? (uint64_t)queued / w->block : 0;
    t->wType = TIME_SAMPLES;
    t->u.sample = (DWORD)(pulled > q ? pulled - q : 0);
    return MMSYSERR_NOERROR;
}
MMRESULT waveOutClose(HWAVEOUT h) {
    WaveOut *w = h;
    SDL_DestroyAudioStream(w->stream);
    SDL_DestroyMutex(w->lock);
    SDL_free(w);
    return MMSYSERR_NOERROR;
}
#endif
