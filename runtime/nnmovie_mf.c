/* movie:: (the SDK's video player library) on FFmpeg ([witchtime], see the FFmpeg section).
 *
 * The SDK library is a port of Android's stagefright (NuMediaExtractor / MediaCodec); the game's own
 * copy lives in subsdk0, whose exported symbols (movie::NnExtractor, movie::DecoderImpl, movie::NnMediaData,
 * android::FileSource, ...) give the object layouts below.  Legend: [game] read off the game's player code
 * (0x71036e6000-0x71036f9000), [subsdk] read off subsdk0's code and vtables, [doc] public documentation.
 *
 * Objects the game allocates (16 bytes each, with f_710392dce0(16, 16)) and drives through a vtable:
 *   movie::Extractor  {vtable, impl*}   [subsdk ctor 0x22eb0]      movie::Decoder {vtable, impl*}  [subsdk 0x20db0]
 * Extractor vtable (object pointer = _ZTV + 0x10, so slot 0 is the first virtual function) [subsdk _ZTVN5movie9ExtractorE]:
 *   +00 ~Extractor   +08 deleting dtor   +10 SetDataSource(const char*)   +18 SetDataSource(const void*, int)
 *   +20 GetTrackCount(int*)   +28 GetTrackConfiguration(int, MediaData*)   +30 SelectTrack(int)   +38 UnselectTrack(int)
 *   +40 SeekTo(int64)   +48 GetTrackIndexForAvailableData(size_t*)   +50 Read(Buffer*)   +58 Advance()
 *   +60 GetSampleTime(int64*)   +68 GetSampleMetaData(MediaData*)   +70 GetBufferedRange(int64*, int64*)
 *   +78 SetDataSource(ClientStreamReader*, const char*)
 * Decoder vtable [subsdk _ZTVN5movie7DecoderE]:
 *   +00 ~Decoder   +08 deleting dtor   +10 Configure(MediaData*)   +18 RegisterClientEvents(DecoderEvents*)
 *   +20 GetOutputFormat(MediaData*)   +28 Start   +30 Stop   +38 Reset   +40 Flush
 *   +48 AcquireInputBufferIndex(int *index, int *moreAvailable)   +50 GetInputBuffer(int, Buffer*)
 *   +58 SendInputBufferForDecode(int index, int offset, int size, int64 timeUs, unsigned flags)
 *   +60 AcquireOutputBufferIndex(int *index, int64 *timeUs, unsigned *flags, int *moreAvailable)
 *   +68 GetOutputBuffer(int, Buffer*)   +70 GetOutputBufferSize(int *size, int *count)   +78 ReleaseOutputBufferIndex(int)
 *   +80 GetLastError()   +88 GetOutputBuffer(int, Buffer*, MediaData*)   +90 GetOutputBufferFormat(int, MediaData*)
 * Results: 0 = ok; the SDK wrappers return 0x1000 when there is no implementation and 0x100b for bad arguments
 * [subsdk]; the game treats 0x2000 from GetTrackIndexForAvailableData/Read as "end of data" and retries on 0x5003
 * [game 0x71036f0230, 0x71036f6ce0]; any other non-zero result from Read makes it send an end-of-stream input
 * buffer (flags 4) [game 0x71036f6dd8].  Buffer flags follow MediaCodec: 4 = end of stream [game 0x71036f0784].
 * DecoderEvents = four pointers to nn::os::EventType: +0 input buffer available, +8 output buffer available,
 * +10 output format changed, +18 error [subsdk DecoderImpl::Send*EventToClient 0x1fec0..0x1ff20; game 0x71036f0590].
 * The game's player [game 0x71036f04c0] waits on those events: input -> AcquireInputBufferIndex (repeated
 * "moreAvailable" times) and hands the index to a feed thread; the feed thread [game 0x71036f6ce0] does
 * GetTrackIndexForAvailableData -> (own track?) GetInputBuffer -> Extractor::Read(Buffer) -> GetSampleTime ->
 * SendInputBufferForDecode(index, Offset(), Size(), time, flags) -> Advance.  Output event -> AcquireOutputBufferIndex,
 * entries {index, timeUs, flags} are queued and the render path [game 0x71036f7410] calls GetOutputBufferFormat(index)
 * ("width", "height", "nv12-y-stride", "nv12-uv-offset", "nv12-y-offset"), GetOutputFormat ("nv12-colorspace"),
 * GetOutputBuffer(index, Buffer{base = its own texture memory, capacity}) and ReleaseOutputBufferIndex; it uploads
 * the Y plane (w x h at nv12-y-offset) and the UV plane (w/2 x h/2 at nv12-uv-offset).  Format event ->
 * GetOutputFormat ("width", "height").  The game's clock is in microseconds (ConvertToTimeSpan / 1000) [game 0x71036f6ff0].
 * Decoder(type, outputFormat, mode, n): type 1 = H.264 ("video/avc"), 2 = VP8, 3 = VP9, 4 = AAC ("audio/mp4a-latm"),
 * 5 = Vorbis [game 0x71036f19a0 maps the track mime to these values; seen in a run: VP9 track -> type 3]; the game asks output format 4 for video and 1 for audio,
 * mode 1 [game 0x71036f0a0c, 0x71036f0e90]; format 4 is tightly packed NV12 (size w*h + 2*((w+1)/2)*((h+1)/2),
 * Y rows then interleaved UV rows of w bytes) and 2 is ARGB [subsdk GetOutputBufferSize, ConvertDecoderOutputFormat].
 * Track configuration keys the game reads: "mime", "durationUs", "width", "height", "sample-rate", "channel-count",
 * "max-input-size" (it adds 0x5000 and passes the MediaData to Decoder::Configure) [game 0x71036f19a0, 0x71036f0a68].
 * Container types: 1 = .mp4/.m4a, 2 = .mkv, 3 = .webm [game 0x71036f1d70].  The game selects the tracks it decodes
 * with SelectTrack [game 0x71036e74cc, 0x71036f2f90] and unselects audio it will not decode [game 0x71036f0f20].
 * ClientStreamReader (the object the game passes to SetDataSource; android::FileSource uses it) [subsdk 0x1d90e0..0x1d93a0]:
 *   +10 bool Open(const char *path, void **handle)   +18 int64 ReadAt(void *handle, int64 offset, void *buf, size_t n)
 *   +20 bool Close(void *handle)   +28 int GetSize(void *handle, int64 *size)
 * movie::Buffer (0x40 bytes) [subsdk 0x23a70..0x23d30]: +0 offset(i32) +4 size(i32) +8 int32 +c uint32 +10 owns(u8)
 *   +18 capacity(u64) +20 int64 +28 base +30 nvnDev +38 nvnSync; Buffer(size_t) allocates through movie::SetAllocator's
 *   alloc(size, align, user) and the destructor frees owned memory.
 * movie::MediaData is one pointer to a key/value store [subsdk 0x23d70]; here that is a host structure.
 * movie::SetAllocator(alloc(size, align, user), free(p, user), realloc(p, size, user), user) [game 0x71036e66b4].
 * The opening movie is stream:/movie/... in data.arc: WebM, VP9 1280x720, two Vorbis 48 kHz stereo tracks, 420 s
 * (extracted copy in movie_samples\).  FFmpeg decodes in software, so no codec of the system's and no GPU is needed. A movie
 * that cannot start must not happen: the game gives up on it, and without the opening New Game falls for ever (#68).
 *
 * How this file maps the protocol onto FFmpeg: the extractor reads the whole file through the game's ClientStreamReader into
 * memory and opens it there.  src_read demuxes AND decodes, so the "compressed sample" handed to the game in Extractor::Read
 * is only a token {magic, Sample*, time} written into the decoder's input slot; Decoder::SendInputBufferForDecode moves that
 * sample to the output queue, GetOutputBuffer copies the decoded NV12 / PCM data into the game's buffer and
 * ReleaseOutputBufferIndex frees the slot.
 *
 * Environment: SSBU_MOVIE=0 keeps the failing placeholder (nnmovie.c); SSBU_MOVIE_SAVE=<file> saves what the game's
 * reader returned; SSBU_MOVIE_LOG=1 logs every decoder call; SSBU_MOVIE_TEST_RESYNC=<flag>,<n> makes the game
 * re-synchronise (pause, seek, resume) the movie at the n-th video sample, see dec_SendInputBufferForDecode. */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
struct AvSrc;                                               /* the FFmpeg source of an extractor (Ext::rd) */
typedef struct Sample { unsigned char *data; size_t len; } Sample;   /* a decoded frame: tight NV12 or interleaved s16 */
static void hs_free(Sample *s) { if (s) { free(s->data); free(s); } }
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "hostrt.h"

void input_mark(const char *what);                       /* nnhid.c: anchor for scripted input */

#define A0 (c->x[0])
#define A1 (c->x[1])
#define A2 (c->x[2])
#define A3 (c->x[3])
#define A4 (c->x[4])
#define RET(v) (c->x[0] = (uint64_t)(v))

void os_signal_event(uint64_t key);                   /* os_threads.c */

#define ST_OK 0
#define ST_FAILED 0x1000
#define ST_INVALID 0x100b
#define ST_UNSUPPORTED 0x1006
#define ST_END 0x2000
#define ST_NO_INPUT 0x1011                             /* AcquireInputBufferIndex: nothing offered [subsdk 0x1fac8] */
#define ST_NO_OUTPUT 0x100e                            /* AcquireOutputBufferIndex: nothing decoded [subsdk 0x1e8d0] */
#define FLAG_EOS 4
#define MAX_TRACKS 8
#define SLOT_BYTES 64
#include "image_layout.h"
#define GAME_FREE (GUEST_BASE + GAME_FREE_OFF)        /* [Recomps#1] our main's exported free, as in libcxx.c */
/* [witchtime] B3 allocates the 16-byte Extractor / Decoder objects with its heap-aware operator new [game 0x7100cd50f0], which free()
 * does not know ("not a live block" at the end of the opening movie): the deleting destructors release them with operator delete. */
#define GAME_DELETE (GUEST_BASE + GAME_OPDELETE_OFF)
#define TOKEN_MAGIC 0x4d4f564945544f4bULL             /* "MOVIETOK" */

/* ---------------------------------------------------------------- small helpers */
static int g_log;                                     /* SSBU_MOVIE_LOG */
static int g_test_resync_flag, g_test_resync_at;      /* SSBU_MOVIE_TEST_RESYNC */
static uint64_t g_alloc_fn, g_free_fn, g_alloc_user;  /* movie::SetAllocator */

/* Call a guest function with up to five arguments; the caller's registers are preserved. */
static uint64_t gcall(Ctx *c, uint64_t fn, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    uint64_t save[9], lr = c->x[30], f = c->fn;
    memcpy(save, c->x, sizeof save);
    c->x[0] = a0; c->x[1] = a1; c->x[2] = a2; c->x[3] = a3; c->x[4] = a4;
    call_guest(c, fn);
    uint64_t r = c->x[0];
    memcpy(c->x, save, sizeof save);
    c->x[30] = lr; c->fn = f;
    return r;
}

static void *galloc(Ctx *c, size_t size, size_t align, int *owns) {
    if (g_alloc_fn) {
        void *p = (void *)(uintptr_t)gcall(c, g_alloc_fn, size, align, g_alloc_user, 0, 0);
        if (p) { *owns = 1; return p; }
    }
    *owns = 2;
    return _aligned_malloc(size ? size : 16, align < 16 ? 16 : align);
}
static void gfree(Ctx *c, void *p, int owns) {
    if (!p) return;
    if (owns == 1 && g_free_fn) gcall(c, g_free_fn, (uint64_t)(uintptr_t)p, g_alloc_user, 0, 0, 0);
    else if (owns == 2) _aligned_free(p);
}

/* ---------------------------------------------------------------- movie::MediaData */
enum { MD_NONE, MD_I32, MD_I64, MD_F32, MD_F64, MD_STR, MD_PTR, MD_SIZE, MD_RECT, MD_BUF };
typedef struct { char key[40]; int type; int32_t i32; int64_t i64; float f32; double f64; char *str; uint64_t ptr; int rect[4]; } MDEntry;
typedef struct { int n; MDEntry e[40]; } MD;

static MD *md_of(uint64_t guest) { return guest ? (MD *)(uintptr_t)rd64(guest) : NULL; }
static MDEntry *md_find(MD *m, const char *key) {
    if (!m) return NULL;
    for (int i = 0; i < m->n; i++) if (strcmp(m->e[i].key, key) == 0) return &m->e[i];
    return NULL;
}
static MDEntry *md_put(MD *m, const char *key, int type) {
    MDEntry *e = md_find(m, key);
    if (!e) {
        if (m->n >= (int)(sizeof m->e / sizeof m->e[0])) return NULL;
        e = &m->e[m->n++];
        memset(e, 0, sizeof *e);
        strncpy(e->key, key, sizeof e->key - 1);
    } else if (e->str) { free(e->str); e->str = NULL; }
    e->type = type;
    return e;
}
static void md_set_i32(MD *m, const char *k, int32_t v) { MDEntry *e = md_put(m, k, MD_I32); if (e) e->i32 = v; }
static void md_set_i64(MD *m, const char *k, int64_t v) { MDEntry *e = md_put(m, k, MD_I64); if (e) e->i64 = v; }
static void md_set_f32(MD *m, const char *k, float v) { MDEntry *e = md_put(m, k, MD_F32); if (e) e->f32 = v; }
static void md_set_str(MD *m, const char *k, const char *v) { MDEntry *e = md_put(m, k, MD_STR); if (e) e->str = _strdup(v ? v : ""); }
static void md_set_ptr(MD *m, const char *k, uint64_t v) { MDEntry *e = md_put(m, k, MD_PTR); if (e) e->ptr = v; }
static void md_clear(MD *m) { if (!m) return; for (int i = 0; i < m->n; i++) free(m->e[i].str); m->n = 0; }
static int md_get_i32(MD *m, const char *k, int32_t *out) {
    MDEntry *e = md_find(m, k);
    if (!e) return 0;
    if (e->type == MD_I32) *out = e->i32; else if (e->type == MD_I64) *out = (int32_t)e->i64; else if (e->type == MD_SIZE) *out = (int32_t)e->i64; else return 0;
    return 1;
}
static int md_get_i64(MD *m, const char *k, int64_t *out) {
    MDEntry *e = md_find(m, k);
    if (!e) return 0;
    if (e->type == MD_I64 || e->type == MD_SIZE) *out = e->i64; else if (e->type == MD_I32) *out = e->i32; else return 0;
    return 1;
}
static int md_get_str(MD *m, const char *k, const char **out) { MDEntry *e = md_find(m, k); if (!e || e->type != MD_STR) return 0; *out = e->str; return 1; }
static int md_get_ptr(MD *m, const char *k, uint64_t *out) { MDEntry *e = md_find(m, k); if (!e || e->type != MD_PTR) return 0; *out = e->ptr; return 1; }

static void mdata_ctor(Ctx *c) { MD *m = calloc(1, sizeof *m); wr64(A0, (uint64_t)(uintptr_t)m); }
static void mdata_copy_ctor(Ctx *c) {                 /* MediaData(const MediaData &) */
    MD *m = calloc(1, sizeof *m), *src = md_of(A1);
    if (src) { *m = *src; for (int i = 0; i < m->n; i++) if (m->e[i].str) m->e[i].str = _strdup(m->e[i].str); }
    wr64(A0, (uint64_t)(uintptr_t)m);
}
static void mdata_dtor(Ctx *c) { MD *m = md_of(A0); if (m) { md_clear(m); free(m); wr64(A0, 0); } }
static void mdata_Clear(Ctx *c) { md_clear(md_of(A0)); }
static void mdata_SetInt32(Ctx *c) { MD *m = md_of(A0); if (m) md_set_i32(m, (const char *)(uintptr_t)A1, (int32_t)A2); }
static void mdata_SetInt64(Ctx *c) { MD *m = md_of(A0); if (m) md_set_i64(m, (const char *)(uintptr_t)A1, (int64_t)A2); }
static void mdata_SetString(Ctx *c) { MD *m = md_of(A0); if (m) md_set_str(m, (const char *)(uintptr_t)A1, (const char *)(uintptr_t)A2); }
static void mdata_SetFloat(Ctx *c) { MD *m = md_of(A0); if (m) md_set_f32(m, (const char *)(uintptr_t)A1, c->v[0].f[0]); }
static void mdata_SetPointer(Ctx *c) { MD *m = md_of(A0); if (m) md_set_ptr(m, (const char *)(uintptr_t)A1, A2); }
static void mdata_FindInt32(Ctx *c) { int32_t v = 0; int ok = md_get_i32(md_of(A0), (const char *)(uintptr_t)A1, &v); if (ok && A2) wr32(A2, (uint32_t)v); RET(ok); }
static void mdata_FindInt64(Ctx *c) { int64_t v = 0; int ok = md_get_i64(md_of(A0), (const char *)(uintptr_t)A1, &v); if (ok && A2) wr64(A2, (uint64_t)v); RET(ok); }
static void mdata_FindString(Ctx *c) { const char *s = NULL; int ok = md_get_str(md_of(A0), (const char *)(uintptr_t)A1, &s); if (ok && A2) wr64(A2, (uint64_t)(uintptr_t)s); RET(ok); }
static void mdata_FindPointer(Ctx *c) { uint64_t v = 0; int ok = md_get_ptr(md_of(A0), (const char *)(uintptr_t)A1, &v); if (ok && A2) wr64(A2, v); RET(ok); }
static void mdata_Contains(Ctx *c) { RET(md_find(md_of(A0), (const char *)(uintptr_t)A1) != NULL); }
static void mdata_CountEntries(Ctx *c) { MD *m = md_of(A0); RET(m ? m->n : 0); }
static void mdata_assign(Ctx *c) {                    /* operator= */
    MD *m = md_of(A0), *src = md_of(A1);
    if (m && src && m != src) { md_clear(m); *m = *src; for (int i = 0; i < m->n; i++) if (m->e[i].str) m->e[i].str = _strdup(m->e[i].str); }
    RET(A0);
}

/* ---------------------------------------------------------------- movie::Buffer */
#define B_OFFSET 0x00
#define B_SIZE 0x04
#define B_I32 0x08
#define B_U32 0x0c
#define B_OWNS 0x10
#define B_CAP 0x18
#define B_I64 0x20
#define B_BASE 0x28
#define B_DEV 0x30
#define B_SYNC 0x38

static void buf_zero(uint64_t b) { memset((void *)(uintptr_t)b, 0, 0x40); }
static void buf_ctor(Ctx *c) { buf_zero(A0); }
static void buf_ctor_size(Ctx *c) {                   /* Buffer(size_t capacity): owns its memory */
    uint64_t b = A0, cap = A1;
    int owns = 0;
    void *p = galloc(c, (size_t)cap, 16, &owns);
    buf_zero(b);
    wr64(b + B_CAP, cap);
    wr64(b + B_BASE, (uint64_t)(uintptr_t)p);
    wr8(b + B_OWNS, (uint8_t)(p ? owns : 0));
}
static void buf_ctor_data(Ctx *c) {                   /* Buffer(void *data, size_t capacity): borrowed */
    uint64_t b = A0;
    buf_zero(b);
    wr64(b + B_BASE, A1);
    wr64(b + B_CAP, A2);
}
static void buf_copy_from(Ctx *c, uint64_t b, uint64_t src) {
    uint8_t owns = rd8(src + B_OWNS);
    uint64_t cap = rd64(src + B_CAP), base = rd64(src + B_BASE);
    wr32(b + B_OFFSET, rd32(src + B_OFFSET)); wr32(b + B_SIZE, rd32(src + B_SIZE));
    wr32(b + B_I32, rd32(src + B_I32)); wr32(b + B_U32, rd32(src + B_U32));
    wr64(b + B_I64, rd64(src + B_I64)); wr64(b + B_DEV, rd64(src + B_DEV)); wr64(b + B_SYNC, rd64(src + B_SYNC));
    wr64(b + B_CAP, cap);
    if (owns && base) {                               /* an owning buffer is deep-copied */
        int o = 0;
        void *p = galloc(c, (size_t)cap, 16, &o);
        if (p) memcpy(p, (void *)(uintptr_t)base, (size_t)cap);
        wr64(b + B_BASE, (uint64_t)(uintptr_t)p);
        wr8(b + B_OWNS, (uint8_t)(p ? o : 0));
    } else {
        wr64(b + B_BASE, base);
        wr8(b + B_OWNS, 0);
    }
}
static void buf_release(Ctx *c, uint64_t b) {
    uint8_t owns = rd8(b + B_OWNS);
    if (owns) gfree(c, (void *)(uintptr_t)rd64(b + B_BASE), owns);
    wr64(b + B_BASE, 0);
    wr8(b + B_OWNS, 0);
}
static void buf_copy_ctor(Ctx *c) { buf_zero(A0); buf_copy_from(c, A0, A1); }
static void buf_assign(Ctx *c) { if (A0 != A1) { buf_release(c, A0); buf_copy_from(c, A0, A1); } RET(A0); }
static void buf_dtor(Ctx *c) { buf_release(c, A0); }
static void buf_SetDataAndCapacity(Ctx *c) { buf_release(c, A0); wr64(A0 + B_BASE, A1); wr64(A0 + B_CAP, A2); }
static void buf_SetCapacity(Ctx *c) { wr64(A0 + B_CAP, A1); }
static void buf_SetRange(Ctx *c) { wr32(A0 + B_OFFSET, (uint32_t)A1); wr32(A0 + B_SIZE, (uint32_t)A2); }
static void buf_Base(Ctx *c) { RET(rd64(A0 + B_BASE)); }
static void buf_Data(Ctx *c) { RET(rd64(A0 + B_BASE) + (int32_t)rd32(A0 + B_OFFSET)); }
static void buf_Size(Ctx *c) { RET(rd32(A0 + B_SIZE)); }
static void buf_Offset(Ctx *c) { RET(rd32(A0 + B_OFFSET)); }
static void buf_Capacity(Ctx *c) { RET(rd64(A0 + B_CAP)); }
static void buf_SetInt32Data(Ctx *c) { wr32(A0 + B_I32, (uint32_t)A1); }
static void buf_SetInt64Data(Ctx *c) { wr64(A0 + B_I64, A1); }
static void buf_SetUint32Data(Ctx *c) { wr32(A0 + B_U32, (uint32_t)A1); }
static void buf_GetInt32Data(Ctx *c) { RET(rd32(A0 + B_I32)); }
static void buf_GetInt64Data(Ctx *c) { RET(rd64(A0 + B_I64)); }
static void buf_GetUint32Data(Ctx *c) { RET(rd32(A0 + B_U32)); }
static void buf_SetNvnDev(Ctx *c) { wr64(A0 + B_DEV, A1); }
static void buf_GetNvnDev(Ctx *c) { RET(rd64(A0 + B_DEV)); }
static void buf_SetNvnSync(Ctx *c) { wr64(A0 + B_SYNC, A1); }
static void buf_GetNvnSync(Ctx *c) { RET(rd64(A0 + B_SYNC)); }
static void buf_Swap(Ctx *c) { unsigned char t[0x40]; memcpy(t, (void *)(uintptr_t)A0, 0x40); memcpy((void *)(uintptr_t)A0, (void *)(uintptr_t)A1, 0x40); memcpy((void *)(uintptr_t)A1, t, 0x40); }

/* ---------------------------------------------------------------- plumbing */
typedef struct {
    int kind;                                         /* 0 video, 1 audio, 2 other */
    char mime[48];
    int w, h, rate, ch, selected, eos;
    double fps;
    int failed;                                       /* close4: the decoder reported an error for this track */
} Track;

typedef struct Ext {
    CRITICAL_SECTION cs;
    struct AvSrc *rd;
    int ntracks;
    Track tr[MAX_TRACKS];
    int64_t duration_us;
    int have_pending, pend_track, pend_consumed, eos;
    Sample *pending;
    int64_t pend_us;
    struct { int have; Sample *s; int64_t us; } ahead[MAX_TRACKS];    /* next decoded sample of each selected track */
    char path[260];
    unsigned long long samples_read;
    volatile uint64_t gen;                            /* bumped by SeekTo: samples read before it are refused by the decoders */
} Ext;

/* Decoder buffer model, as in the real library [subsdk DecoderImpl 0x1e440 Flush, 0x1fa30 AcquireInputBufferIndex,
 * 0x1e710 AcquireOutputBufferIndex, 0x1ff40/0x20050 Add...List]: TWO independent pools.
 *   input buffers  0..nin-1 : the codec offers an index when it can take data ("input list", one event per index);
 *                             AcquireInputBufferIndex pops the first one (*more = how many are left, 0x1011 when the
 *                             list is empty); SendInputBufferForDecode gives it back to the codec, which offers it
 *                             again as soon as the data is consumed - it does NOT wait for the decoded frame to be shown.
 *   output buffers 0..nout-1: a decoded frame sits in one from "output list" (AcquireOutputBufferIndex pops, 0x100e
 *                             when empty) until ReleaseOutputBufferIndex. When every output buffer is in use the codec
 *                             stops consuming input (back-pressure): the sent input stays "in decode".
 * Flush [subsdk 0x1e440] = MediaCodec::flush + both lists emptied, no event: every index the client still holds is
 * void, and nothing is offered until Start (MediaCodec::start after a flush offers all input buffers again).
 * The game's Seek [game 0x71036e7620] does exactly Flush(video), Flush(audio), empties its own index lists,
 * Extractor::SeekTo, sleeps 16 ms, empties the lists again, Start(video), Start(audio). */
#define MAX_IN 16
#define MAX_OUT 64
enum { IN_CODEC = 0, IN_OFFERED, IN_CLIENT, IN_DECODING };
enum { OUT_FREE = 0, OUT_READY, OUT_CLIENT };
typedef struct { int state; Sample *s; int64_t ts; uint32_t flags; } OutBuf;

typedef struct {
    CRITICAL_SECTION cs;
    int type, fmt, mode, is_video;
    Ext *ext;
    int track;
    uint64_t events;                                  /* guest DecoderEvents* */
    int started, flushed, fmt_signalled;
    int nin, nout;
    unsigned char in_state[MAX_IN];
    int in_list[MAX_IN], in_n;                        /* offered input indices, oldest first */
    struct { Sample *s; int64_t ts; uint32_t flags; } in_data[MAX_IN];   /* what an IN_DECODING buffer holds */
    int dec_list[MAX_IN], dec_n;                      /* inputs waiting for a free output buffer, oldest first */
    OutBuf out[MAX_OUT];
    int out_list[MAX_OUT], out_n;                     /* decoded frames ready for the client, oldest first */
    unsigned char *mem;                               /* nin * SLOT_BYTES: the "input buffers" */
    int w, h, rate, ch;
    unsigned long long in_count, out_count, copied, dropped, flushes;
    int warned_small, warned_token;
    uint32_t last_error;                              /* DecoderImpl +0xd8 [subsdk 0x1fc80] */
} Dec;

typedef struct { uint64_t magic; Sample *s; int64_t ts; uint64_t gen; } Token;   /* gen = Ext::gen when it was read */

static void signal_event(Dec *d, int which) {
    if (!d->events) return;
    uint64_t ev = rd64(d->events + (uint64_t)which * 8);
    if (ev) os_signal_event(ev);
}

static void ext_lock(Ext *e) { EnterCriticalSection(&e->cs); }
static void ext_unlock(Ext *e) { LeaveCriticalSection(&e->cs); }

static Ext *ext_of(uint64_t guest) { return guest ? (Ext *)(uintptr_t)rd64(guest + 8) : NULL; }
static Dec *dec_of(uint64_t guest) { return guest ? (Dec *)(uintptr_t)rd64(guest + 8) : NULL; }

static void release_pending(Ext *e) {                 /* the current sample is done with (Advance) */
    if (e->have_pending && !e->pend_consumed && e->pending) hs_free(e->pending);
    if (e->have_pending) { e->ahead[e->pend_track].have = 0; e->ahead[e->pend_track].s = NULL; }
    e->have_pending = 0; e->pending = NULL; e->pend_consumed = 0;
}
static void release_ahead(Ext *e, int track) {        /* track < 0: all */
    for (int i = 0; i < MAX_TRACKS; i++) {
        if (track >= 0 && i != track) continue;
        if (e->have_pending && e->pend_track == i) continue;                  /* owned by release_pending */
        if (e->ahead[i].have && e->ahead[i].s) hs_free(e->ahead[i].s);
        e->ahead[i].have = 0; e->ahead[i].s = NULL;
    }
}

/* ---------------------------------------------------------------- FFmpeg
 * The whole file the game's reader returned stays in memory and is read through an AVIOContext. Each selected track has its
 * own decoder; packets of a selected track read while another track's sample was wanted wait in that track's queue, so each
 * track is read ahead by one decoded sample (see ext_fill). */
#define QMAX 512
struct AvSrc {
    unsigned char *data; size_t size, pos;
    AVIOContext *io; AVFormatContext *fmt; AVFrame *fr; AVPacket *pk;
    AVCodecContext *dec[MAX_TRACKS]; struct SwsContext *sws[MAX_TRACKS]; SwrContext *swr[MAX_TRACKS];
    AVPacket *q[MAX_TRACKS][QMAX]; int qn[MAX_TRACKS], flushed[MAX_TRACKS], file_eof;
};
static int av_read_cb(void *o, uint8_t *buf, int n) {
    struct AvSrc *a = o;
    if (a->pos >= a->size) return AVERROR_EOF;
    size_t k = a->size - a->pos < (size_t)n ? a->size - a->pos : (size_t)n;
    memcpy(buf, a->data + a->pos, k); a->pos += k;
    return (int)k;
}
static int64_t av_seek_cb(void *o, int64_t off, int whence) {
    struct AvSrc *a = o;
    if (whence & AVSEEK_SIZE) return (int64_t)a->size;
    int64_t p = ((whence & 3) == SEEK_CUR ? (int64_t)a->pos : (whence & 3) == SEEK_END ? (int64_t)a->size : 0) + off;
    if (p < 0 || p > (int64_t)a->size) return -1;
    a->pos = (size_t)p;
    return p;
}
static const char *av_mime(enum AVCodecID id) {
    switch (id) {
    case AV_CODEC_ID_VP9: return "video/x-vnd.on2.vp9";
    case AV_CODEC_ID_VP8: return "video/x-vnd.on2.vp8";
    case AV_CODEC_ID_H264: return "video/avc";
    case AV_CODEC_ID_HEVC: return "video/hevc";
    case AV_CODEC_ID_VORBIS: return "audio/vorbis";
    case AV_CODEC_ID_OPUS: return "audio/opus";
    case AV_CODEC_ID_AAC: return "audio/mp4a-latm";
    case AV_CODEC_ID_MP3: return "audio/mpeg";
    default: return NULL;
    }
}
static void q_clear(struct AvSrc *a, int i) { for (int k = 0; k < a->qn[i]; k++) av_packet_free(&a->q[i][k]); a->qn[i] = 0; }
/* Takes over data (freed with the extractor). */
static int ext_open_bytes(Ext *e, unsigned char *data, size_t size) {
    struct AvSrc *a = calloc(1, sizeof *a);
    unsigned char *iobuf = av_malloc(65536);
    a->data = data; a->size = size;
    a->io = avio_alloc_context(iobuf, 65536, 0, a, av_read_cb, NULL, av_seek_cb);
    a->fmt = avformat_alloc_context();
    a->fmt->pb = a->io;
    int r = avformat_open_input(&a->fmt, NULL, NULL, NULL);
    if (r < 0 || avformat_find_stream_info(a->fmt, NULL) < 0) {
        printf("movie: FFmpeg cannot open %s (%zu bytes): %d\n", e->path, size, r);
        if (r >= 0) avformat_close_input(&a->fmt); else avformat_free_context(a->fmt);
        av_freep(&a->io->buffer); avio_context_free(&a->io); free(a->data); free(a);
        return 0;
    }
    a->fr = av_frame_alloc(); a->pk = av_packet_alloc();
    e->rd = a;
    e->duration_us = a->fmt->duration > 0 ? a->fmt->duration : 0;          /* AV_TIME_BASE: microseconds */
    e->ntracks = 0;
    for (unsigned i = 0; i < a->fmt->nb_streams && i < MAX_TRACKS; i++) {
        AVCodecParameters *cp = a->fmt->streams[i]->codecpar;
        Track *tr = &e->tr[e->ntracks++];
        const char *mime = av_mime(cp->codec_id);
        memset(tr, 0, sizeof *tr);
        if (cp->codec_type == AVMEDIA_TYPE_VIDEO) {
            AVRational fr = a->fmt->streams[i]->avg_frame_rate;
            tr->kind = 0; tr->w = cp->width; tr->h = cp->height; tr->fps = fr.den ? av_q2d(fr) : 0.0;
            snprintf(tr->mime, sizeof tr->mime, "%s", mime ? mime : "video/unknown");
        } else if (cp->codec_type == AVMEDIA_TYPE_AUDIO) {
            tr->kind = 1; tr->rate = cp->sample_rate; tr->ch = cp->ch_layout.nb_channels;
            snprintf(tr->mime, sizeof tr->mime, "%s", mime ? mime : "audio/unknown");
        } else {
            tr->kind = 2; strcpy(tr->mime, "application/octet-stream");
        }
        a->fmt->streams[i]->discard = AVDISCARD_ALL;                       /* the game selects what it decodes */
    }
    printf("movie: opened %s with FFmpeg: %zu bytes, %.1f s, %d tracks:", e->path, size, e->duration_us / 1e6, e->ntracks);
    for (int i = 0; i < e->ntracks; i++) {
        Track *tr = &e->tr[i];
        if (tr->kind == 0) printf("  [%d] %s %dx%d %.2f fps", i, tr->mime, tr->w, tr->h, tr->fps);
        else if (tr->kind == 1) printf("  [%d] %s %d Hz %d ch", i, tr->mime, tr->rate, tr->ch);
        else printf("  [%d] %s", i, tr->mime);
    }
    printf("\n");
    return 1;
}
static void dec_close(struct AvSrc *a, int i) {
    avcodec_free_context(&a->dec[i]); sws_freeContext(a->sws[i]); a->sws[i] = NULL; swr_free(&a->swr[i]); q_clear(a, i); a->flushed[i] = 0;
}
static int ext_select(Ext *e, int i, int on) {
    if (!e->rd || i < 0 || i >= e->ntracks) return ST_INVALID;
    struct AvSrc *a = e->rd;
    Track *tr = &e->tr[i];
    AVStream *st = a->fmt->streams[i];
    int ok = 1;
    if (on && !a->dec[i] && tr->kind != 2) {
        const AVCodec *cd = avcodec_find_decoder(st->codecpar->codec_id);
        AVCodecContext *cc = cd ? avcodec_alloc_context3(cd) : NULL;
        if (cc) cc->thread_count = 0;                                      /* automatic */
        if (!cc || avcodec_parameters_to_context(cc, st->codecpar) < 0 || avcodec_open2(cc, cd, NULL) < 0) {
            printf("movie: track %d (%s): no FFmpeg decoder; track left unselected\n", i, tr->mime);
            avcodec_free_context(&cc);
            ok = 0;
        } else a->dec[i] = cc;
    }
    if (!on || !ok) dec_close(a, i);
    st->discard = on && ok ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    if (e->have_pending && e->pend_track == i) release_pending(e);
    release_ahead(e, i);
    tr->selected = on && ok;
    tr->eos = 0;
    printf("movie: %s track %d (%s)%s\n", on ? "selected" : "unselected", i, tr->mime, ok ? "" : " FAILED");
    return ok ? ST_OK : ST_UNSUPPORTED;
}
/* A decoded frame as the sample the game's buffers take: video tightly packed NV12 (rows of w), audio interleaved s16. */
static Sample *av_sample(struct AvSrc *a, int i, AVFrame *f) {
    Sample *s = calloc(1, sizeof *s);
    if (a->dec[i]->codec_type == AVMEDIA_TYPE_VIDEO) {
        int w = f->width, h = f->height;
        a->sws[i] = sws_getCachedContext(a->sws[i], w, h, (enum AVPixelFormat)f->format, w, h, AV_PIX_FMT_NV12, SWS_BILINEAR, NULL, NULL, NULL);
        s->len = (size_t)w * h + (size_t)w * ((h + 1) / 2);
        s->data = malloc(s->len);
        uint8_t *dst[4] = {s->data, s->data + (size_t)w * h, NULL, NULL};
        int ds[4] = {w, w, 0, 0};
        if (!a->sws[i] || sws_scale(a->sws[i], (const uint8_t *const *)f->data, f->linesize, 0, h, dst, ds) <= 0) memset(s->data, 0, s->len);
    } else {
        AVCodecContext *cc = a->dec[i];
        if (!a->swr[i]) {
            swr_alloc_set_opts2(&a->swr[i], &cc->ch_layout, AV_SAMPLE_FMT_S16, cc->sample_rate, &cc->ch_layout, cc->sample_fmt, cc->sample_rate, 0, NULL);
            if (a->swr[i] && swr_init(a->swr[i]) < 0) swr_free(&a->swr[i]);
        }
        int ch = cc->ch_layout.nb_channels > 0 ? cc->ch_layout.nb_channels : 2;
        s->data = malloc((size_t)f->nb_samples * ch * 2 + 16);
        uint8_t *out = s->data;
        int n = a->swr[i] ? swr_convert(a->swr[i], &out, f->nb_samples, (const uint8_t **)f->extended_data, f->nb_samples) : 0;
        s->len = n > 0 ? (size_t)n * ch * 2 : 0;
    }
    return s;
}
static int src_read(Ext *e, int i, Sample **out, int64_t *us) {
    struct AvSrc *a = e->rd;
    AVCodecContext *cc = a->dec[i];
    *out = NULL;
    if (!cc) return 1;
    for (;;) {
        int r = avcodec_receive_frame(cc, a->fr);
        if (r == 0) {
            int64_t pts = a->fr->best_effort_timestamp;
            *us = pts == AV_NOPTS_VALUE ? 0 : av_rescale_q(pts, a->fmt->streams[i]->time_base, (AVRational){1, 1000000});
            *out = av_sample(a, i, a->fr);
            av_frame_unref(a->fr);
            return 0;
        }
        if (r == AVERROR_EOF) return 1;
        if (r != AVERROR(EAGAIN)) { printf("movie: FFmpeg decoding of track %d failed: %d\n", i, r); return -1; }
        AVPacket *pk = NULL;
        if (a->qn[i]) { pk = a->q[i][0]; memmove(&a->q[i][0], &a->q[i][1], sizeof pk * (size_t)--a->qn[i]); }
        else while (!a->file_eof) {
            if (av_read_frame(a->fmt, a->pk) < 0) { a->file_eof = 1; break; }
            int si = a->pk->stream_index;
            if (si == i) { pk = av_packet_clone(a->pk); av_packet_unref(a->pk); break; }
            if (si >= 0 && si < MAX_TRACKS && si < e->ntracks && e->tr[si].selected) {
                if (a->qn[si] == QMAX) { av_packet_free(&a->q[si][0]); memmove(&a->q[si][0], &a->q[si][1], sizeof pk * (QMAX - 1)); a->qn[si]--; }   /* shortcut: drops the oldest, raise QMAX if a track starves */
                a->q[si][a->qn[si]++] = av_packet_clone(a->pk);
            }
            av_packet_unref(a->pk);
        }
        if (pk) { r = avcodec_send_packet(cc, pk); av_packet_free(&pk); if (r < 0 && r != AVERROR(EAGAIN)) { printf("movie: FFmpeg rejects a packet of track %d: %d\n", i, r); return -1; } }
        else if (!a->flushed[i]) { avcodec_send_packet(cc, NULL); a->flushed[i] = 1; }     /* the file is read: drain the decoder */
        else return 1;
    }
}
static int src_seek(Ext *e, int64_t us) {
    struct AvSrc *a = e->rd;
    int r = av_seek_frame(a->fmt, -1, us, AVSEEK_FLAG_BACKWARD);
    for (int i = 0; i < MAX_TRACKS; i++) { if (a->dec[i]) avcodec_flush_buffers(a->dec[i]); q_clear(a, i); a->flushed[i] = 0; }
    a->file_eof = 0;
    return r >= 0;
}
static void src_close(Ext *e) {
    struct AvSrc *a = e->rd;
    if (!a) return;
    for (int i = 0; i < MAX_TRACKS; i++) dec_close(a, i);
    av_frame_free(&a->fr); av_packet_free(&a->pk);
    avformat_close_input(&a->fmt);
    if (a->io) { av_freep(&a->io->buffer); avio_context_free(&a->io); }
    free(a->data); free(a);
    e->rd = NULL;
}

/* The current sample of the extractor, as NuMediaExtractor defines it: the one with the SMALLEST TIMESTAMP among the
 * next samples of the selected tracks (file order). Each selected track is read ahead by one decoded sample with its
 * own src_read call; reading "any stream" instead hands samples out in the order the decoders finish them,
 * which changes from run to run - and the game's two feed threads each wait for THEIR track to be the current one, so
 * a burst of one track fills that decoder's buffers and starves the other (the start-up stall).
 * Returns 0 with a pending sample, or ST_END. Caller holds the lock. */
static int ext_fill(Ext *e) {
    if (e->have_pending) return ST_OK;
    {   /* TEST ONLY, SSBU_SKIP_MOVIES=1 (tests\DEBUG_MODE.md): every movie ends at once, as a stream with no samples left */
        static int skip = -1;
        if (skip < 0) { const char *v = getenv("SSBU_SKIP_MOVIES"); skip = v && atoi(v) != 0; }
        static unsigned long long keep = 0;                /* SSBU_SKIP_MOVIES=<n>: n > 1 lets the first n samples through (default 1 = 12) */
        if (skip > 0 && !keep) { const char *v = getenv("SSBU_SKIP_MOVIES"); keep = atoi(v) > 1 ? (unsigned long long)atoi(v) : 12; }
        if (skip && e->samples_read >= keep) { if (!e->eos) printf("movie: SSBU_SKIP_MOVIES: end of stream after %llu samples%c", e->samples_read, 10); e->eos = 1; return ST_END; }
    }
    if (e->eos || !e->rd) return ST_END;
    int best = -1;
    for (int i = 0; i < e->ntracks; i++) {
        Track *tr = &e->tr[i];
        if (!tr->selected) continue;
        for (int guard = 0; !e->ahead[i].have && !tr->eos && guard < 256; guard++) {
            Sample *s = NULL; int64_t us = 0;
            int r = src_read(e, i, &s, &us);              /* [witchtime] FFmpeg (above) */
            {   /* test: SSBU_MOVIE_FAIL_AT=<n> makes the read after n samples fail */
                static int fail_at = -1;
                if (fail_at < 0) { const char *v = getenv("SSBU_MOVIE_FAIL_AT"); fail_at = v ? atoi(v) : 0; }
                if (fail_at > 0 && e->samples_read >= (unsigned long long)fail_at) { if (s) { hs_free(s); s = NULL; } r = -1; }
            }
            if (r < 0) {
                tr->failed = 1;                           /* the decoder of this track reports it: dec_error_check */
                printf("movie: reading track %d failed after %llu samples: treating as its end\n", i, e->samples_read);
                tr->eos = 1;
                break;
            }
            if (r > 0) { tr->eos = 1; break; }
            if (!s) continue;                             /* a tick or a format change without data */
            e->ahead[i].have = 1; e->ahead[i].s = s; e->ahead[i].us = us;
            e->samples_read++;
        }
        if (e->ahead[i].have && (best < 0 || e->ahead[i].us < e->ahead[best].us)) best = i;
    }
    if (best < 0) { if (!e->eos) printf("movie: end of stream after %llu samples\n", e->samples_read); e->eos = 1; return ST_END; }
    e->have_pending = 1; e->pending = e->ahead[best].s; e->pend_track = best; e->pend_us = e->ahead[best].us; e->pend_consumed = 0;
    return ST_OK;
}

/* ---------------------------------------------------------------- Extractor virtual functions */
static void ext_dtor_common(Ctx *c, int deleting) {
    Ext *e = ext_of(A0);
    if (e) {
        ext_lock(e);
        release_pending(e);
        release_ahead(e, -1);
        src_close(e);
        ext_unlock(e);
        DeleteCriticalSection(&e->cs);
        printf("movie: Extractor destroyed (%llu samples read)\n", e->samples_read);
        free(e);
        wr64(A0 + 8, 0);
    }
    if (deleting) gcall(c, GAME_DELETE, A0, 0, 0, 0, 0);     /* the game allocated the 16-byte object with its operator new [game 0x7100cd50f0] */
}
static void ext_dtor(Ctx *c) { ext_dtor_common(c, 0); }
static void ext_deleting_dtor(Ctx *c) { ext_dtor_common(c, 1); }

static int ext_set_source_bytes(Ext *e, unsigned char *data, size_t size) {
    const char *save = getenv("SSBU_MOVIE_SAVE");
    if (save) { FILE *f = fopen(save, "wb"); if (f) { fwrite(data, 1, size, f); fclose(f); printf("movie: saved %zu bytes to %s\n", size, save); } }
    int ok = ext_open_bytes(e, data, size);
    return ok ? ST_OK : ST_FAILED;
}

static void ext_SetDataSource_reader(Ctx *c) {       /* (ClientStreamReader *reader, const char *path) */
    Ext *e = ext_of(A0);
    uint64_t reader = A1;
    const char *path = (const char *)(uintptr_t)A2;
    if (!e || !reader) { RET(ST_INVALID); return; }
    strncpy(e->path, path ? path : "", sizeof e->path - 1);
    uint64_t vt = rd64(reader);
    uint64_t handle = 0;
    uint64_t ok = gcall(c, rd64(vt + 0x10), reader, (uint64_t)(uintptr_t)path, (uint64_t)(uintptr_t)&handle, 0, 0);
    if (!(ok & 1)) { printf("movie: SetDataSource(%s): the game's reader could not open it\n", path); RET(ST_FAILED); return; }
    int64_t size = 0;
    uint64_t r = gcall(c, rd64(vt + 0x28), reader, handle, (uint64_t)(uintptr_t)&size, 0, 0);
    if (size <= 0 || size > (int64_t)2 << 30) {       /* the SDK ignores GetSize's return value [subsdk 0x1d93dc] */
        printf("movie: SetDataSource(%s): GetSize -> %llx, size %lld\n", path, (unsigned long long)r, (long long)size);
        gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0);
        RET(ST_FAILED); return;
    }
    unsigned char *data = malloc((size_t)size);
    if (!data) { gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0); RET(ST_FAILED); return; }
    int64_t got = 0;
    LARGE_INTEGER f0, t0, t1; QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&t0);
    while (got < size) {
        uint64_t chunk = (uint64_t)(size - got) < (8u << 20) ? (uint64_t)(size - got) : (8u << 20);
        int64_t n = (int64_t)gcall(c, rd64(vt + 0x18), reader, handle, (uint64_t)got, (uint64_t)(uintptr_t)(data + got), chunk);
        if (n <= 0) break;
        got += n;
    }
    QueryPerformanceCounter(&t1);
    gcall(c, rd64(vt + 0x20), reader, handle, 0, 0, 0);
    printf("movie: SetDataSource(%s): read %lld of %lld bytes through the game's reader in %.2f s\n", path, (long long)got, (long long)size,
           (double)(t1.QuadPart - t0.QuadPart) / f0.QuadPart);
    if (got != size) { free(data); RET(ST_FAILED); return; }
    RET(ext_set_source_bytes(e, data, (size_t)size));
}

static void ext_SetDataSource_path(Ctx *c) {         /* (const char *path): a host file, for tests */
    Ext *e = ext_of(A0);
    const char *path = (const char *)(uintptr_t)A1;
    if (!e || !path) { RET(ST_INVALID); return; }
    strncpy(e->path, path, sizeof e->path - 1);
    FILE *f = fopen(path, "rb");
    if (!f) { printf("movie: SetDataSource(%s): not a host file\n", path); RET(ST_FAILED); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *data = malloc((size_t)sz);
    size_t got = fread(data, 1, (size_t)sz, f);
    fclose(f);
    RET(got == (size_t)sz ? ext_set_source_bytes(e, data, got) : ST_FAILED);
}

static void ext_SetDataSource_mem(Ctx *c) {          /* (const void *data, int size) */
    Ext *e = ext_of(A0);
    if (!e || !A1 || (int)A2 <= 0) { RET(ST_INVALID); return; }
    strcpy(e->path, "memory.webm");
    unsigned char *data = malloc((size_t)(int)A2);
    memcpy(data, (void *)(uintptr_t)A1, (size_t)(int)A2);
    RET(ext_set_source_bytes(e, data, (size_t)(int)A2));
}

static void ext_GetTrackCount(Ctx *c) { Ext *e = ext_of(A0); if (!e || !A1) { RET(ST_INVALID); return; } wr32(A1, (uint32_t)(e->rd ? e->ntracks : 0)); RET(e->rd ? ST_OK : ST_FAILED); }

static void ext_GetTrackConfiguration(Ctx *c) {       /* (int track, MediaData *out) */
    Ext *e = ext_of(A0);
    int i = (int)A1;
    MD *m = md_of(A2);
    if (!e || !m) { RET(ST_INVALID); return; }
    if (!e->rd || i < 0 || i >= e->ntracks) { RET(ST_INVALID); return; }
    Track *tr = &e->tr[i];
    md_clear(m);
    md_set_str(m, "mime", tr->mime);
    md_set_i64(m, "durationUs", e->duration_us);
    md_set_i32(m, "max-input-size", 0x10000);
    if (tr->kind == 0) {
        md_set_i32(m, "width", tr->w); md_set_i32(m, "height", tr->h);
        md_set_i32(m, "frame-rate", (int)(tr->fps + 0.5));
    } else if (tr->kind == 1) {
        md_set_i32(m, "sample-rate", tr->rate); md_set_i32(m, "channel-count", tr->ch);
    }
    md_set_ptr(m, "nn-extractor", (uint64_t)(uintptr_t)e);    /* lets the decoder find its source */
    md_set_i32(m, "nn-track", i);
    if (g_log) printf("movie: GetTrackConfiguration(%d) -> %s\n", i, tr->mime);
    RET(ST_OK);
}

static void ext_SelectTrack(Ctx *c) { Ext *e = ext_of(A0); if (!e) { RET(ST_INVALID); return; } ext_lock(e); int r = ext_select(e, (int)A1, 1); ext_unlock(e); RET(r); }
static void ext_UnselectTrack(Ctx *c) { Ext *e = ext_of(A0); if (!e) { RET(ST_INVALID); return; } ext_lock(e); int r = ext_select(e, (int)A1, 0); ext_unlock(e); RET(r); }

static void ext_SeekTo(Ctx *c) {                      /* (int64 timeUs) */
    Ext *e = ext_of(A0);
    if (!e || !e->rd) { RET(ST_INVALID); return; }
    ext_lock(e);
    release_pending(e);
    release_ahead(e, -1);
    e->gen++;
    int ok = src_seek(e, (int64_t)A1);
    e->eos = 0;
    for (int i = 0; i < e->ntracks; i++) e->tr[i].eos = 0;
    ext_unlock(e);
    printf("movie: SeekTo(%.3f s)%s\n", (int64_t)A1 / 1e6, ok ? "" : " FAILED");
    RET(ok ? ST_OK : ST_FAILED);
}

static void ext_GetTrackIndexForAvailableData(Ctx *c) {   /* (size_t *track) */
    Ext *e = ext_of(A0);
    if (!e || !A1) { RET(ST_INVALID); return; }
    ext_lock(e);
    int r = ext_fill(e);
    if (r == ST_OK) wr64(A1, (uint64_t)e->pend_track);
    ext_unlock(e);
    RET(r);
}

static void ext_Read(Ctx *c) {                        /* (Buffer *out): the sample token goes into the decoder's input slot */
    Ext *e = ext_of(A0);
    uint64_t b = A1;
    if (!e || !b) { RET(ST_INVALID); return; }
    ext_lock(e);
    int r = ext_fill(e);
    if (r == ST_OK) {
        uint64_t base = rd64(b + B_BASE), cap = rd64(b + B_CAP);
        if (base && cap >= sizeof(Token) && !e->pend_consumed) {
            Token t = {TOKEN_MAGIC, e->pending, e->pend_us, e->gen};
            memcpy((void *)(uintptr_t)base, &t, sizeof t);
            wr32(b + B_OFFSET, 0);
            wr32(b + B_SIZE, (uint32_t)sizeof t);
            wr64(b + B_I64, (uint64_t)e->pend_us);
            e->pend_consumed = 1;                     /* the sample now belongs to the token */
        } else {
            r = ST_FAILED;
        }
    }
    ext_unlock(e);
    RET(r);
}

static void ext_Advance(Ctx *c) {
    Ext *e = ext_of(A0);
    if (!e) { RET(ST_INVALID); return; }
    ext_lock(e);
    release_pending(e);
    ext_unlock(e);
    RET(ST_OK);
}

static void ext_GetSampleTime(Ctx *c) {               /* (int64 *timeUs) */
    Ext *e = ext_of(A0);
    if (!e || !A1) { RET(ST_INVALID); return; }
    ext_lock(e);
    int r = e->have_pending ? ST_OK : ext_fill(e);
    wr64(A1, (uint64_t)(r == ST_OK ? e->pend_us : -1));
    ext_unlock(e);
    RET(r);
}

static void ext_GetSampleMetaData(Ctx *c) {           /* (MediaData *) */
    Ext *e = ext_of(A0);
    MD *m = md_of(A1);
    if (!e || !m) { RET(ST_INVALID); return; }
    ext_lock(e);
    md_clear(m);
    if (e->have_pending) { md_set_i64(m, "timeUs", e->pend_us); md_set_i32(m, "track-index", e->pend_track); md_set_i32(m, "is-sync-frame", 1); }
    ext_unlock(e);
    RET(ST_OK);
}

static void ext_GetBufferedRange(Ctx *c) {            /* (int64 *startUs, int64 *endUs) */
    Ext *e = ext_of(A0);
    if (!e) { RET(ST_INVALID); return; }
    if (A1) wr64(A1, 0);
    if (A2) wr64(A2, (uint64_t)e->duration_us);
    RET(ST_OK);
}

/* ---------------------------------------------------------------- Decoder virtual functions */
static void dec_lock(Dec *d) { EnterCriticalSection(&d->cs); }
static void dec_unlock(Dec *d) { LeaveCriticalSection(&d->cs); }

static void dlog(Dec *d, const char *fmt, ...) {      /* SSBU_MOVIE_LOG: one line per call, with thread and time */
    if (!g_log) return;
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    printf("movie[%5lu %8llu] %c%04llx %s\n", GetCurrentThreadId(), (unsigned long long)GetTickCount64(), d->is_video ? 'V' : 'A', (unsigned long long)((uintptr_t)d >> 4 & 0xffff), line);      /* the hex: which decoder */
}

/* Frees every sample the decoder holds and empties all lists (caller holds the lock). */
static void dec_release_all(Dec *d) {
    for (int i = 0; i < d->nout; i++) { if (d->out[i].s) hs_free(d->out[i].s); memset(&d->out[i], 0, sizeof d->out[i]); }
    for (int i = 0; i < d->nin; i++) {
        if (d->in_state[i] == IN_DECODING && d->in_data[i].s) hs_free(d->in_data[i].s);
        Token t;                                      /* a sample read into the buffer but not sent yet */
        memcpy(&t, d->mem + (size_t)i * SLOT_BYTES, sizeof t);
        if (t.magic == TOKEN_MAGIC && t.s) hs_free(t.s);
        memset(d->mem + (size_t)i * SLOT_BYTES, 0, SLOT_BYTES);
        memset(&d->in_data[i], 0, sizeof d->in_data[i]);
        d->in_state[i] = IN_CODEC;
    }
    d->in_n = d->dec_n = d->out_n = 0;
}

/* The codec's own progress (caller holds the lock): every input waiting in decode moves to a free output buffer,
 * and its input buffer is offered again. Returns bit 0 = input indices were offered, bit 1 = frames became ready. */
static int dec_pump(Dec *d) {
    int ev = 0;
    while (d->dec_n > 0) {
        int o = -1;
        for (int i = 0; i < d->nout; i++) if (d->out[i].state == OUT_FREE) { o = i; break; }
        if (o < 0) break;
        int in = d->dec_list[0];
        memmove(d->dec_list, d->dec_list + 1, (size_t)(d->dec_n - 1) * sizeof d->dec_list[0]);
        d->dec_n--;
        d->out[o].s = d->in_data[in].s; d->out[o].ts = d->in_data[in].ts; d->out[o].flags = d->in_data[in].flags;
        d->out[o].state = OUT_READY;
        d->out_list[d->out_n++] = o;
        memset(&d->in_data[in], 0, sizeof d->in_data[in]);
        d->in_state[in] = IN_OFFERED;
        d->in_list[d->in_n++] = in;
        ev |= 3;
    }
    return ev;
}
static void dec_signal(Dec *d, int ev) { if (ev & 2) signal_event(d, 1); if (ev & 1) signal_event(d, 0); }

static void dec_dtor_common(Ctx *c, int deleting) {
    Dec *d = dec_of(A0);
    if (d) {
        dec_lock(d);
        dec_release_all(d);
        dec_unlock(d);
        DeleteCriticalSection(&d->cs);
        printf("movie: Decoder(%s) destroyed: %llu input buffers, %llu output buffers, %llu copied to the game, %llu refused, %llu flushes\n",
               d->is_video ? "video" : "audio", d->in_count, d->out_count, d->copied, d->dropped, d->flushes);
        free(d->mem);
        free(d);
        wr64(A0 + 8, 0);
    }
    if (deleting) gcall(c, GAME_DELETE, A0, 0, 0, 0, 0);     /* the game allocated the 16-byte object with its operator new [game 0x7100cd50f0] */
}
static void dec_dtor(Ctx *c) { dec_dtor_common(c, 0); }
static void dec_deleting_dtor(Ctx *c) { dec_dtor_common(c, 1); }

static void dec_Configure(Ctx *c) {                   /* (MediaData *format) */
    Dec *d = dec_of(A0);
    MD *m = md_of(A1);
    if (!d) { RET(ST_INVALID); return; }
    uint64_t ext = 0; int32_t track = -1, v = 0;
    const char *mime = "?";
    if (m) {
        md_get_ptr(m, "nn-extractor", &ext);
        md_get_i32(m, "nn-track", &track);
        md_get_str(m, "mime", &mime);
        if (md_get_i32(m, "width", &v)) d->w = v;
        if (md_get_i32(m, "height", &v)) d->h = v;
        if (md_get_i32(m, "sample-rate", &v)) d->rate = v;
        if (md_get_i32(m, "channel-count", &v)) d->ch = v;
    }
    d->ext = (Ext *)(uintptr_t)ext;
    d->track = track;
    printf("movie: Decoder(%s type %d, output format %d) configured for track %d (%s%s, %dx%d, %d Hz, %d ch)\n",
           d->is_video ? "video" : "audio", d->type, d->fmt, track, mime, ext ? "" : ", NO extractor link, ", d->w, d->h, d->rate, d->ch);
    if (!ext) { RET(ST_UNSUPPORTED); return; }
    RET(ST_OK);                                       /* input buffers are offered by Start, not here */
}

static void dec_RegisterClientEvents(Ctx *c) {
    Dec *d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    dec_lock(d);
    d->events = A1;
    int ev = (d->in_n > 0 ? 1 : 0) | (d->out_n > 0 ? 2 : 0);     /* registered after Start: nothing may be lost */
    dec_unlock(d);
    dec_signal(d, ev);
    RET(ST_OK);
}

static void dec_fill_format(Dec *d, MD *m) {
    md_clear(m);
    if (d->is_video) {
        int w = d->w, h = d->h;
        md_set_str(m, "mime", "video/raw");
        md_set_i32(m, "width", w); md_set_i32(m, "height", h);
        md_set_i32(m, "stride", w); md_set_i32(m, "slice-height", h);
        md_set_i32(m, "crop-width", w); md_set_i32(m, "crop-height", h);
        md_set_i32(m, "color-format", 21);            /* OMX_COLOR_FormatYUV420SemiPlanar = NV12 */
        md_set_i32(m, "nv12-width", w); md_set_i32(m, "nv12-height", h);
        md_set_i32(m, "nv12-y-stride", w); md_set_i32(m, "nv12-uv-stride", w);
        md_set_i32(m, "nv12-y-offset", 0); md_set_i32(m, "nv12-uv-offset", w * h);
        md_set_i32(m, "nv12-colorspace", 0);
    } else {
        md_set_str(m, "mime", "audio/raw");
        md_set_i32(m, "sample-rate", d->rate); md_set_i32(m, "channel-count", d->ch);
        md_set_i32(m, "pcm-encoding", 2);             /* 16-bit */
    }
}

static void dec_GetOutputFormat(Ctx *c) { Dec *d = dec_of(A0); MD *m = md_of(A1); if (!d || !m) { RET(ST_INVALID); return; } dec_fill_format(d, m); RET(ST_OK); }
static void dec_GetOutputBufferFormat(Ctx *c) { Dec *d = dec_of(A0); MD *m = md_of(A2); if (!d || !m) { RET(ST_INVALID); return; } dec_fill_format(d, m); RET(ST_OK); }

/* P3 round 3: which decoders send the "output format" event (DecoderEvents + 0x10). The game's handler thread
 * [game 0x71036f3500] signals the player's "formats known" event (player + 0x498) only when the format event of EVERY
 * decoder it created has arrived (cases 3 and 7 of its switch: flags + 0x4c1 and + 0x4c0), and the playback thread
 * waits for it for 5 s while holding the player's mutex [0x71036f2580]: with the event sent by the video decoder only,
 * every movie start waited the full 5 s and the main thread stood at that mutex [0x71036e7690]. So the real audio
 * decoder sends it as well. SSBU_MOVIE_NO_AUDIO_FORMAT_EVENT=1: video only, as before. */
static int fmt_event_wanted(const Dec *d) {
    static int video_only = -1;
    if (video_only < 0) video_only = getenv("SSBU_MOVIE_NO_AUDIO_FORMAT_EVENT") != NULL;
    return (d->is_video || !video_only) && !d->fmt_signalled;
}
static void dec_Start(Ctx *c) {
    Dec *d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    dec_lock(d);
    int offer = !d->started || d->flushed;            /* first start, or the start that follows a flush: all input buffers are offered */
    if (offer) {
        dec_release_all(d);
        for (int i = 0; i < d->nin; i++) { d->in_state[i] = IN_OFFERED; d->in_list[i] = i; }
        d->in_n = d->nin;
    }
    d->started = 1; d->flushed = 0;
    int fmt = fmt_event_wanted(d);
    if (fmt) d->fmt_signalled = 1;
    dec_unlock(d);
    dlog(d, "Start%s", offer ? " (all input buffers offered)" : " (already running)");
    if (fmt) signal_event(d, 2);
    if (offer) signal_event(d, 0);
    RET(ST_OK);
}
static void dec_halt(Ctx *c, const char *what, int stop) {
    Dec *d = dec_of(A0);
    if (!d) { RET(ST_INVALID); return; }
    dec_lock(d);
    int held_in = 0, held_out = 0;
    for (int i = 0; i < d->nin; i++) held_in += d->in_state[i] == IN_CLIENT;
    for (int i = 0; i < d->nout; i++) held_out += d->out[i].state == OUT_CLIENT;
    int queued = d->out_n + d->dec_n;
    dec_release_all(d);
    d->flushed = 1; d->flushes++;
    if (stop) d->started = 0;
    dec_unlock(d);
    dlog(d, "%s: dropped %d decoded frames, voided %d input and %d output indices held by the game", what, queued, held_in, held_out);
    RET(ST_OK);                                       /* no event: nothing is offered until Start */
}
static void dec_Stop(Ctx *c) { dec_halt(c, "Stop", 1); }
static void dec_Flush(Ctx *c) { dec_halt(c, "Flush", 0); }
static void dec_Reset(Ctx *c) { dec_halt(c, "Reset", 1); }
/* [subsdk 0x21090] Decoder::GetLastError: 0x1000 without an implementation object, else DecoderImpl +0xd8 [0x1fc80]. That field is
 * written by the codec's message receiver [0x203a8..0x20410]: on an error message the status (0x1000 unless the codec's own code
 * converts to another one [0x35e140]) is stored and the client's error event (+0x18) is signalled [0x20428]. Here the codec is
 * FFmpeg's, inside src_read: a failed read of the track is that error; its code has no movie::Status, so 0x1000. */
static void dec_error_check(Dec *d) {                 /* called with the decoder locked */
    if (d->last_error || !d->ext || d->track < 0 || d->track >= d->ext->ntracks || !d->ext->tr[d->track].failed) return;
    d->last_error = ST_FAILED;
    printf("movie: decoder of track %d: codec error -> last error 0x%x, error event signalled\n", d->track, ST_FAILED);
    signal_event(d, 3);
}
static void dec_GetLastError(Ctx *c) { Dec *d = dec_of(A0); RET(d ? d->last_error : ST_FAILED); }

static void dec_AcquireInputBufferIndex(Ctx *c) {     /* (int *index, int *moreAvailable) */
    Dec *d = dec_of(A0);
    if (!d || !A1 || !A2) { RET(ST_INVALID); return; }     /* both pointers are required [subsdk 0x1fa48] */
    dec_lock(d);
    int found = -1, more = 0;
    if (d->in_n > 0) {
        found = d->in_list[0];
        memmove(d->in_list, d->in_list + 1, (size_t)(d->in_n - 1) * sizeof d->in_list[0]);
        more = --d->in_n;
        d->in_state[found] = IN_CLIENT;
    }
    dec_unlock(d);
    wr32(A1, (uint32_t)found);
    wr32(A2, (uint32_t)more);
    dlog(d, "AcquireInput -> %d (more %d)", found, more);
    RET(found >= 0 ? ST_OK : ST_NO_INPUT);
}

static void dec_GetInputBuffer(Ctx *c) {              /* (int index, Buffer *out) */
    Dec *d = dec_of(A0);
    int i = (int)A1;
    uint64_t b = A2;
    if (!d || !b || i < 0 || i >= d->nin) { RET(ST_INVALID); return; }
    buf_release(c, b);
    wr64(b + B_BASE, (uint64_t)(uintptr_t)(d->mem + (size_t)i * SLOT_BYTES));
    wr64(b + B_CAP, SLOT_BYTES);
    wr32(b + B_OFFSET, 0); wr32(b + B_SIZE, 0);
    RET(ST_OK);
}

static void dec_SendInputBufferForDecode(Ctx *c) {    /* (int index, int offset, int size, int64 timeUs, unsigned flags) */
    Dec *d = dec_of(A0);
    int i = (int)A1, size = (int)A3;                  /* A2 = offset, A3 = size, A4 = timeUs, x5 = flags [game 0x71036f6e2c] */
    int64_t ts = (int64_t)A4;
    uint32_t flags = (uint32_t)c->x[5];
    if (!d || i < 0 || i >= d->nin) { RET(ST_INVALID); return; }
    dec_lock(d);
    dec_error_check(d);
    Sample *sample = NULL;
    int stale = 0;
    Token t;
    memcpy(&t, d->mem + (size_t)i * SLOT_BYTES, sizeof t);
    memset(d->mem + (size_t)i * SLOT_BYTES, 0, sizeof t);
    if (t.magic == TOKEN_MAGIC && t.s) {
        sample = t.s;
        if (ts <= 0) ts = t.ts;
        if (d->ext && t.gen != d->ext->gen) stale = 1;      /* read before the last SeekTo */
    } else if (!(flags & FLAG_EOS) && !d->warned_token) {
        d->warned_token = 1;
        printf("movie: Decoder(%s): input buffer %d holds no sample token (size %d): dropped\n", d->is_video ? "video" : "audio", i, size);
    }
    if (d->in_state[i] != IN_CLIENT || stale) {       /* an index voided by Flush, or data from before the seek: the codec refuses it */
        if (sample) hs_free(sample);
        d->dropped++;
        int was = d->in_state[i];
        int ev = 0;
        if (was == IN_CLIENT) { d->in_state[i] = IN_OFFERED; d->in_list[d->in_n++] = i; ev = 1; }
        dec_unlock(d);
        dlog(d, "SendInput %d REFUSED (%s) t=%.3f flags %x", i, was != IN_CLIENT ? "index not held by the game" : "sample from before the seek", ts / 1e6, flags);
        dec_signal(d, ev);
        RET(was == IN_CLIENT ? ST_OK : ST_INVALID);
        return;
    }
    d->in_count++;
    if (g_test_resync_at && d->is_video && d->in_count == (unsigned long long)g_test_resync_at) {
        /* SSBU_MOVIE_TEST_RESYNC=<flag>,<n>: at the n-th video sample raise the game's own "re-synchronise movies" request
         * (byte 0x7107446040, set by its system-notification handler f_7103741700: 8 = operation mode changed, 4 / 2 = the
         * two halves of a focus change); the movie manager [game 0x71036e3984] then pauses, seeks and resumes the player. */
        uint8_t *req = (uint8_t *)(uintptr_t)0x7107446040ULL;
#ifdef __MINGW32__
        __atomic_fetch_or(req, (uint8_t)g_test_resync_flag, __ATOMIC_SEQ_CST);     /* [Recomps#1] mingw: no _InterlockedOr8 intrinsic */
#else
        InterlockedOr8((char *)req, (char)g_test_resync_flag);
#endif
        printf("movie: TEST: resync request %d raised at video sample %d\n", g_test_resync_flag, g_test_resync_at);
    }
    if (!sample && !(flags & FLAG_EOS)) {             /* nothing to decode: the buffer is offered again */
        d->dropped++;
        d->in_state[i] = IN_OFFERED; d->in_list[d->in_n++] = i;
        dec_unlock(d);
        signal_event(d, 0);
        RET(ST_OK);
        return;
    }
    d->in_data[i].s = sample; d->in_data[i].ts = ts; d->in_data[i].flags = flags & FLAG_EOS;
    d->in_state[i] = IN_DECODING;
    d->dec_list[d->dec_n++] = i;
    int first_fmt = fmt_event_wanted(d);
    if (first_fmt) d->fmt_signalled = 1;
    if (d->in_count == 1 || (flags & FLAG_EOS)) printf("movie: Decoder(%s): %s queued (t=%.3f s)\n", d->is_video ? "video" : "audio", flags & FLAG_EOS ? "end of stream" : "first sample", ts / 1e6);
    int ev = dec_pump(d);
    int waiting = d->dec_n, ready = d->out_n;
    dec_unlock(d);
    dlog(d, "SendInput %d t=%.3f flags %x (ready %d, waiting for an output buffer %d)", i, ts / 1e6, flags, ready, waiting);
    if (first_fmt) signal_event(d, 2);
    dec_signal(d, ev);
    RET(ST_OK);
}

static void dec_AcquireOutputBufferIndex(Ctx *c) {    /* (int *index, int64 *timeUs, unsigned *flags, int *moreAvailable) */
    Dec *d = dec_of(A0);
    if (!d || !A1 || !A2 || !A3 || !A4) { RET(ST_INVALID); return; }    /* all four are required [subsdk 0x1e730] */
    dec_lock(d);
    dec_error_check(d);
    int r = ST_NO_OUTPUT, idx = -1; int64_t ts = 0; uint32_t flags = 0; int more = 0;
    if (d->out_n > 0) {
        idx = d->out_list[0];
        memmove(d->out_list, d->out_list + 1, (size_t)(d->out_n - 1) * sizeof d->out_list[0]);
        more = --d->out_n;
        OutBuf *s = &d->out[idx];
        s->state = OUT_CLIENT; ts = s->ts; flags = s->flags;
        d->out_count++;
        r = ST_OK;
    }
    dec_unlock(d);
    wr32(A1, (uint32_t)idx);
    wr64(A2, (uint64_t)ts);
    wr32(A3, flags);
    wr32(A4, (uint32_t)more);
    dlog(d, "AcquireOutput -> %d t=%.3f flags %x (more %d, lr %llx)", idx, ts / 1e6, flags, more, (unsigned long long)c->x[30]);
    RET(r);
}

/* Copies the decoded sample of slot i into the game's buffer; returns the number of bytes written. */
static size_t copy_sample(Dec *d, OutBuf *s, unsigned char *dst, size_t cap) {      /* [witchtime] already in the game's layout */
    if (!s->s || !s->s->data) return 0;
    size_t n = s->s->len < cap ? s->s->len : cap;
    if (s->s->len > cap && !d->warned_small) { d->warned_small = 1; printf("movie: game buffer of %zu bytes is smaller than a decoded %s (%zu)\n", cap, d->is_video ? "frame" : "block", s->s->len); }
    if (d->is_video && s->s->len > cap) return 0;
    memcpy(dst, s->s->data, n);
    return n;
}

static void dec_GetOutputBuffer(Ctx *c) {             /* (int index, Buffer *out[, MediaData *format]) */
    Dec *d = dec_of(A0);
    int i = (int)A1;
    uint64_t b = A2;
    if (!d || !b || i < 0 || i >= d->nout) { RET(ST_INVALID); return; }
    dec_lock(d);
    OutBuf *s = &d->out[i];
    int r = ST_OK;
    if (s->state != OUT_CLIENT) r = ST_INVALID;       /* not acquired, or voided by Flush */
    else {
        uint64_t base = rd64(b + B_BASE), cap = rd64(b + B_CAP);
        size_t n = base ? copy_sample(d, s, (unsigned char *)(uintptr_t)base, (size_t)cap) : 0;
        wr32(b + B_OFFSET, 0);
        wr32(b + B_SIZE, (uint32_t)n);
        wr64(b + B_I64, (uint64_t)s->ts);
        wr32(b + B_U32, s->flags);
        if (n) d->copied++;
        if (d->copied == 1 && n) printf("movie: Decoder(%s): first frame copied to the game (%zu bytes into a %llu-byte buffer at %llx, t=%.3f s)\n",
                                        d->is_video ? "video" : "audio", n, (unsigned long long)cap, (unsigned long long)base, s->ts / 1e6);
        else if (d->is_video && d->copied % 300 == 0 && n) printf("movie: %llu video frames delivered (t=%.1f s)\n", d->copied, s->ts / 1e6);
    }
    dec_unlock(d);
    if (r) dlog(d, "GetOutputBuffer %d REFUSED (not held by the game)", i);
    else dlog(d, "GetOutputBuffer %d t=%.3f (lr %llx)", i, s->ts / 1e6, (unsigned long long)c->x[30]);
    RET(r);
}

static void dec_GetOutputBufferWithFormat(Ctx *c) {
    Dec *d = dec_of(A0);
    MD *m = md_of(A3);
    if (d && m) dec_fill_format(d, m);
    dec_GetOutputBuffer(c);
}

static void dec_GetOutputBufferSize(Ctx *c) {         /* (int *size, int *count) */
    Dec *d = dec_of(A0);
    if (!d || !A1) { RET(ST_INVALID); return; }
    int size = d->is_video ? d->w * d->h + 2 * ((d->w + 1) / 2) * ((d->h + 1) / 2) : 0x8000;
    wr32(A1, (uint32_t)size);
    if (A2) wr32(A2, 1);
    RET(ST_OK);
}

static void dec_ReleaseOutputBufferIndex(Ctx *c) {    /* (int index) */
    Dec *d = dec_of(A0);
    int i = (int)A1;
    if (!d || i < 0 || i >= d->nout) { RET(ST_INVALID); return; }
    dec_lock(d);
    OutBuf *s = &d->out[i];
    int ev = 0, ok = s->state == OUT_CLIENT;
    if (ok) {
        if (s->s) hs_free(s->s);
        memset(s, 0, sizeof *s);
        ev = dec_pump(d);                             /* a waiting input can now be decoded into this buffer */
    }
    dec_unlock(d);
    dlog(d, "ReleaseOutput %d%s (lr %llx)", i, ok ? "" : " IGNORED (not held by the game)", (unsigned long long)c->x[30]);
    dec_signal(d, ev);
    RET(ST_OK);
}

/* ---------------------------------------------------------------- vtables and constructors */
static uint64_t g_ext_vtable[20], g_dec_vtable[20];
static CRITICAL_SECTION g_init_lock;

typedef struct { const char *name; ImportFn fn; } Slot_;
static void build_vtables(void) {
    static const Slot_ ext_slots[] = {
        {"movie::Extractor::~Extractor", ext_dtor}, {"movie::Extractor::~Extractor(delete)", ext_deleting_dtor},
        {"movie::Extractor::SetDataSource(path)", ext_SetDataSource_path}, {"movie::Extractor::SetDataSource(memory)", ext_SetDataSource_mem},
        {"movie::Extractor::GetTrackCount", ext_GetTrackCount}, {"movie::Extractor::GetTrackConfiguration", ext_GetTrackConfiguration},
        {"movie::Extractor::SelectTrack", ext_SelectTrack}, {"movie::Extractor::UnselectTrack", ext_UnselectTrack},
        {"movie::Extractor::SeekTo", ext_SeekTo}, {"movie::Extractor::GetTrackIndexForAvailableData", ext_GetTrackIndexForAvailableData},
        {"movie::Extractor::Read", ext_Read}, {"movie::Extractor::Advance", ext_Advance},
        {"movie::Extractor::GetSampleTime", ext_GetSampleTime}, {"movie::Extractor::GetSampleMetaData", ext_GetSampleMetaData},
        {"movie::Extractor::GetBufferedRange", ext_GetBufferedRange}, {"movie::Extractor::SetDataSource(reader)", ext_SetDataSource_reader},
    };
    static const Slot_ dec_slots[] = {
        {"movie::Decoder::~Decoder", dec_dtor}, {"movie::Decoder::~Decoder(delete)", dec_deleting_dtor},
        {"movie::Decoder::Configure", dec_Configure}, {"movie::Decoder::RegisterClientEvents", dec_RegisterClientEvents},
        {"movie::Decoder::GetOutputFormat", dec_GetOutputFormat}, {"movie::Decoder::Start", dec_Start},
        {"movie::Decoder::Stop", dec_Stop}, {"movie::Decoder::Reset", dec_Reset}, {"movie::Decoder::Flush", dec_Flush},
        {"movie::Decoder::AcquireInputBufferIndex", dec_AcquireInputBufferIndex}, {"movie::Decoder::GetInputBuffer", dec_GetInputBuffer},
        {"movie::Decoder::SendInputBufferForDecode", dec_SendInputBufferForDecode}, {"movie::Decoder::AcquireOutputBufferIndex", dec_AcquireOutputBufferIndex},
        {"movie::Decoder::GetOutputBuffer", dec_GetOutputBuffer}, {"movie::Decoder::GetOutputBufferSize", dec_GetOutputBufferSize},
        {"movie::Decoder::ReleaseOutputBufferIndex", dec_ReleaseOutputBufferIndex}, {"movie::Decoder::GetLastError", dec_GetLastError},
        {"movie::Decoder::GetOutputBuffer(format)", dec_GetOutputBufferWithFormat}, {"movie::Decoder::GetOutputBufferFormat", dec_GetOutputBufferFormat},
    };
    EnterCriticalSection(&g_init_lock);
    if (!g_ext_vtable[0]) {
        for (unsigned i = 0; i < sizeof ext_slots / sizeof ext_slots[0]; i++) g_ext_vtable[i] = hostfn_address(ext_slots[i].name, ext_slots[i].fn);
        for (unsigned i = 0; i < sizeof dec_slots / sizeof dec_slots[0]; i++) g_dec_vtable[i] = hostfn_address(dec_slots[i].name, dec_slots[i].fn);
    }
    LeaveCriticalSection(&g_init_lock);
}

static void extractor_ctor(Ctx *c) {                  /* Extractor(ContainerType) */
    build_vtables();
    Ext *e = calloc(1, sizeof *e);
    InitializeCriticalSection(&e->cs);
    wr64(A0, (uint64_t)(uintptr_t)g_ext_vtable);
    wr64(A0 + 8, (uint64_t)(uintptr_t)e);
    input_mark("movie");
    printf("movie: Extractor created (container type %u: %s) on %s\n", (unsigned)A1, A1 == 1 ? "mp4" : A1 == 2 ? "mkv" : A1 == 3 ? "webm" : "?", "FFmpeg");
}

static void decoder_ctor(Ctx *c) {                    /* Decoder(DecoderType, DecoderOutputFormat, DecoderMode, int) */
    build_vtables();
    Dec *d = calloc(1, sizeof *d);
    InitializeCriticalSection(&d->cs);
    d->type = (int)A1; d->fmt = (int)A2; d->mode = (int)A3;
    d->is_video = d->type >= 1 && d->type <= 3;
    d->nin = 8;                                       /* compressed-data buffers: only bound how far the feed runs ahead of the codec */
    /* [witchtime] decoded frames the game may hold before the codec stalls. B3's player [game 0x7100cd6cb4] keeps them in a
     * pool of 16 nodes; a frame acquired while the pool is empty [0x7100cd7244] is neither queued nor released, so the
     * picture stops (ev0011, which is pre-rolled while ev0010 plays and then taken in one burst: this decoder has no
     * decoding time). 16 = the pool: the codec stalls first. (SSBU's player wanted 48: over a second before its first frame.) */
    d->nout = d->is_video ? 16 : 64;
    d->mem = calloc((size_t)d->nin, SLOT_BYTES);
    wr64(A0, (uint64_t)(uintptr_t)g_dec_vtable);
    wr64(A0 + 8, (uint64_t)(uintptr_t)d);
    printf("movie: Decoder created (type %d = %s, output format %d, mode %d, %d)\n", d->type,
           d->type == 1 ? "H.264" : d->type == 2 ? "VP8" : d->type == 3 ? "VP9" : d->type == 4 ? "AAC" : d->type == 5 ? "Vorbis" : "?",
           d->fmt, d->mode, (int)A4);
    if (d->is_video && d->fmt != 4) printf("movie: WARNING output format %d requested, only 4 (NV12) is produced\n", d->fmt);
}

/* ---------------------------------------------------------------- free functions */
static void movie_SetAllocator(Ctx *c) {              /* (alloc, free, realloc, user) */
    g_alloc_fn = A0; g_free_fn = A1; g_alloc_user = A3;
    printf("movie: SetAllocator(alloc %llx, free %llx, realloc %llx, user %llx)\n", (unsigned long long)A0, (unsigned long long)A1, (unsigned long long)A2, (unsigned long long)A3);
}
static void movie_SetCoreMask(Ctx *c) { (void)c; }
static int g_thread_priority;
static void movie_SetThreadPriority(Ctx *c) { g_thread_priority = (int)A0; }
static void movie_GetThreadPriority(Ctx *c) { RET(g_thread_priority); }

/* H.264 helpers (the game's elementary-stream path, not used by the WebM movies). */
static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end) {
    for (; p + 3 <= end; p++) if (p[0] == 0 && p[1] == 0 && p[2] == 1) return p;
    return NULL;
}
static void movie_GetH264NalUnitStartPositionAndNalSize(Ctx *c) {   /* (uint8_t **pos, size_t *remaining, uint8_t **nalStart, size_t *nalSize) -> bool */
    uint64_t ppos = A0, prem = A1, pstart = A2, psize = A3;
    if (!ppos || !prem || !pstart || !psize) { RET(0); return; }
    const uint8_t *p = (const uint8_t *)(uintptr_t)rd64(ppos), *end = p + rd64(prem);
    const uint8_t *sc = p ? find_start_code(p, end) : NULL;
    if (!sc) { RET(0); return; }
    const uint8_t *nal = sc + 3;
    const uint8_t *next = find_start_code(nal, end);
    const uint8_t *nal_end = next ? next : end;
    while (nal_end > nal && nal_end[-1] == 0) nal_end--;      /* a 4-byte start code's leading zero belongs to the next unit */
    wr64(pstart, (uint64_t)(uintptr_t)nal);
    wr64(psize, (uint64_t)(nal_end - nal));
    wr64(ppos, (uint64_t)(uintptr_t)(next ? next : end));
    wr64(prem, (uint64_t)(end - (next ? next : end)));
    RET(1);
}
typedef struct { const uint8_t *p; size_t n; size_t bit; } BitReader;
static unsigned br_bit(BitReader *b) { if (b->bit >= b->n * 8) return 0; unsigned v = (b->p[b->bit >> 3] >> (7 - (b->bit & 7))) & 1; b->bit++; return v; }
static unsigned br_bits(BitReader *b, int n) { unsigned v = 0; while (n-- > 0) v = (v << 1) | br_bit(b); return v; }
static unsigned br_ue(BitReader *b) { int z = 0; while (!br_bit(b) && z < 32) z++; return z ? ((1u << z) - 1 + br_bits(b, z)) : 0; }
static int br_se(BitReader *b) { unsigned v = br_ue(b); return (v & 1) ? (int)((v + 1) / 2) : -(int)(v / 2); }
static void movie_GetH264PropertiesFromSps(Ctx *c) {  /* (const uint8_t *sps, size_t len, int *width, int *height, double *frameRate) -> bool */
    const uint8_t *sps = (const uint8_t *)(uintptr_t)A0;
    size_t len = A1;
    if (!sps || len < 4) { RET(0); return; }
    uint8_t clean[512]; size_t n = 0;                 /* strip emulation prevention bytes */
    for (size_t i = 0; i < len && n < sizeof clean; i++) { if (i >= 2 && sps[i] == 3 && sps[i - 1] == 0 && sps[i - 2] == 0) continue; clean[n++] = sps[i]; }
    BitReader b = {clean, n, 0};
    if ((br_bits(&b, 8) & 0x1f) != 7) { RET(0); return; }
    unsigned profile = br_bits(&b, 8); br_bits(&b, 16); br_ue(&b);
    unsigned chroma = 1;
    if (profile == 100 || profile == 110 || profile == 122 || profile == 244 || profile == 44 || profile == 83 || profile == 86 || profile == 118 || profile == 128) {
        chroma = br_ue(&b); if (chroma == 3) br_bit(&b);
        br_ue(&b); br_ue(&b); br_bit(&b);
        if (br_bit(&b)) for (int i = 0; i < (chroma != 3 ? 8 : 12); i++) if (br_bit(&b)) { int last = 8, nxt = 8; for (int j = 0; j < (i < 6 ? 16 : 64); j++) { if (nxt) { int delta = br_se(&b); nxt = (last + delta + 256) % 256; } last = nxt ? nxt : last; } }
    }
    br_ue(&b);
    unsigned poc = br_ue(&b);
    if (poc == 0) br_ue(&b);
    else if (poc == 1) { br_bit(&b); br_se(&b); br_se(&b); unsigned k = br_ue(&b); for (unsigned i = 0; i < k && i < 256; i++) br_se(&b); }
    br_ue(&b); br_bit(&b);
    unsigned wmb = br_ue(&b) + 1, hmap = br_ue(&b) + 1, frame_mbs_only = br_bit(&b);
    if (!frame_mbs_only) br_bit(&b);
    br_bit(&b);
    unsigned cl = 0, cr = 0, ct = 0, cb = 0;
    if (br_bit(&b)) { cl = br_ue(&b); cr = br_ue(&b); ct = br_ue(&b); cb = br_ue(&b); }
    unsigned sub_w = chroma == 3 ? 1 : 2, sub_h = chroma == 1 ? 2 : 1, unit_y = sub_h * (2 - frame_mbs_only);
    int width = (int)(wmb * 16 - sub_w * (cl + cr)), height = (int)((2 - frame_mbs_only) * hmap * 16 - unit_y * (ct + cb));
    double fps = 0.0;
    if (br_bit(&b)) {                                 /* VUI */
        if (br_bit(&b)) { if (br_bits(&b, 8) == 255) br_bits(&b, 32); }
        if (br_bit(&b)) br_bit(&b);
        if (br_bit(&b)) { br_bits(&b, 4); if (br_bit(&b)) br_bits(&b, 24); }
        if (br_bit(&b)) { br_ue(&b); br_ue(&b); }
        if (br_bit(&b)) { unsigned num = br_bits(&b, 32), den = br_bits(&b, 32); if (num) fps = (double)den / (2.0 * num); }
    }
    if (A2) wr32(A2, (uint32_t)width);
    if (A3) wr32(A3, (uint32_t)height);
    if (A4) { double f = fps; uint64_t bits; memcpy(&bits, &f, 8); wr64(A4, bits); }
    RET(1);
}
static void movie_CreateH264DecoderConfigurationData(Ctx *c) {   /* (sps, spsLen, pps, ppsLen, MediaData *out) */
    MD *m = md_of(A4);
    if (!m) { RET(ST_INVALID); return; }
    int w = 0, h = 0;
    md_set_str(m, "mime", "video/avc");
    md_set_i64(m, "csd-0-ptr", A0); md_set_i64(m, "csd-0-size", A1);
    md_set_i64(m, "csd-1-ptr", A2); md_set_i64(m, "csd-1-size", A3);
    (void)w; (void)h;
    printf("movie: CreateH264DecoderConfigurationData(sps %llu bytes, pps %llu bytes): H.264 elementary streams are not implemented\n", (unsigned long long)A1, (unsigned long long)A3);
    RET(ST_UNSUPPORTED);
}

/* ---------------------------------------------------------------- registration */
static const ImportDef movie_mf_imports[] = {
    {"_ZN5movie9ExtractorC1ENS_13ContainerTypeE", extractor_ctor},
    {"_ZN5movie9ExtractorC2ENS_13ContainerTypeE", extractor_ctor},
    {"_ZN5movie9ExtractorC1ENS_13ContainerTypeENS_9CacheSizeE", extractor_ctor},      /* [witchtime] SDK 13's form (x2 = cache size) */
    {"_ZN5movie7DecoderC1ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", decoder_ctor},
    {"_ZN5movie7DecoderC2ENS_11DecoderTypeENS_19DecoderOutputFormatENS_11DecoderModeEi", decoder_ctor},
    {"_ZN5movie12SetAllocatorEPFPvmmS0_EPFvS0_S0_EPFS0_S0_mS0_ES0_", movie_SetAllocator},
    {"_ZN5movie11SetCoreMaskEm", movie_SetCoreMask},
    {"_ZN5movie17SetThreadPriorityEi", movie_SetThreadPriority},
    {"_ZN5movie17GetThreadPriorityEv", movie_GetThreadPriority},
    {"_ZN5movie34CreateH264DecoderConfigurationDataEPKhmS1_mPNS_9MediaDataE", movie_CreateH264DecoderConfigurationData},
    {"_ZN5movie37GetH264NalUnitStartPositionAndNalSizeEPPhPmS1_S2_", movie_GetH264NalUnitStartPositionAndNalSize},
    {"_ZN5movie37GetH264PropertiresFromSeqParamSetDataEPKhmPiS2_Pd", movie_GetH264PropertiesFromSps},
    /* movie::Buffer */
    {"_ZN5movie6BufferC1Ev", buf_ctor}, {"_ZN5movie6BufferC2Ev", buf_ctor},
    {"_ZN5movie6BufferC1Em", buf_ctor_size}, {"_ZN5movie6BufferC2Em", buf_ctor_size},
    {"_ZN5movie6BufferC1EPvm", buf_ctor_data}, {"_ZN5movie6BufferC2EPvm", buf_ctor_data},
    {"_ZN5movie6BufferC1ERKS0_", buf_copy_ctor}, {"_ZN5movie6BufferC2ERKS0_", buf_copy_ctor},
    {"_ZN5movie6BufferD1Ev", buf_dtor}, {"_ZN5movie6BufferD2Ev", buf_dtor},
    {"_ZN5movie6BufferaSERKS0_", buf_assign},
    {"_ZN5movie6Buffer18SetDataAndCapacityEPvm", buf_SetDataAndCapacity}, {"_ZN5movie6Buffer11SetCapacityEm", buf_SetCapacity},
    {"_ZN5movie6Buffer8SetRangeEii", buf_SetRange}, {"_ZN5movie6Buffer4SwapERS0_", buf_Swap},
    {"_ZNK5movie6Buffer4BaseEv", buf_Base}, {"_ZNK5movie6Buffer4DataEv", buf_Data}, {"_ZNK5movie6Buffer4SizeEv", buf_Size},
    {"_ZNK5movie6Buffer6OffsetEv", buf_Offset}, {"_ZNK5movie6Buffer8CapacityEv", buf_Capacity},
    {"_ZN5movie6Buffer12SetInt32DataEi", buf_SetInt32Data}, {"_ZN5movie6Buffer12SetInt64DataEl", buf_SetInt64Data},
    {"_ZN5movie6Buffer13SetUint32DataEj", buf_SetUint32Data}, {"_ZNK5movie6Buffer12GetInt32DataEv", buf_GetInt32Data},
    {"_ZNK5movie6Buffer12GetInt64DataEv", buf_GetInt64Data}, {"_ZNK5movie6Buffer13GetUint32DataEv", buf_GetUint32Data},
    {"_ZN5movie6Buffer9SetNvnDevEPv", buf_SetNvnDev}, {"_ZNK5movie6Buffer9GetNvnDevEv", buf_GetNvnDev},
    {"_ZN5movie6Buffer10SetNvnSyncEPv", buf_SetNvnSync}, {"_ZNK5movie6Buffer10GetNvnSyncEv", buf_GetNvnSync},
    /* movie::MediaData */
    {"_ZN5movie9MediaDataC1Ev", mdata_ctor}, {"_ZN5movie9MediaDataC2Ev", mdata_ctor},
    {"_ZN5movie9MediaDataC1ERKS0_", mdata_copy_ctor}, {"_ZN5movie9MediaDataC2ERKS0_", mdata_copy_ctor},
    {"_ZN5movie9MediaDataD1Ev", mdata_dtor}, {"_ZN5movie9MediaDataD2Ev", mdata_dtor},
    {"_ZN5movie9MediaDataaSERKS0_", mdata_assign},
    {"_ZN5movie9MediaData5ClearEv", mdata_Clear},
    {"_ZN5movie9MediaData8SetInt32EPKci", mdata_SetInt32}, {"_ZN5movie9MediaData8SetInt64EPKcl", mdata_SetInt64},
    {"_ZN5movie9MediaData9SetStringEPKcS2_", mdata_SetString}, {"_ZN5movie9MediaData8SetFloatEPKcf", mdata_SetFloat},
    {"_ZN5movie9MediaData10SetPointerEPKcPv", mdata_SetPointer},
    {"_ZNK5movie9MediaData9FindInt32EPKcPi", mdata_FindInt32}, {"_ZNK5movie9MediaData9FindInt64EPKcPl", mdata_FindInt64},
    {"_ZNK5movie9MediaData10FindStringEPKcPS2_", mdata_FindString}, {"_ZNK5movie9MediaData11FindPointerEPKcPPv", mdata_FindPointer},
    {"_ZNK5movie9MediaData8ContainsEPKc", mdata_Contains}, {"_ZNK5movie9MediaData12CountEntriesEv", mdata_CountEntries},
};

void register_movie_mf_imports(void) {
    const char *env = getenv("SSBU_MOVIE");
    if (env && atoi(env) == 0) { printf("movie: SSBU_MOVIE=0, keeping the failing placeholder\n"); return; }
    g_log = getenv("SSBU_MOVIE_LOG") != NULL;
    if (getenv("SSBU_MOVIE_TEST_RESYNC")) sscanf(getenv("SSBU_MOVIE_TEST_RESYNC"), "%d,%d", &g_test_resync_flag, &g_test_resync_at);
    InitializeCriticalSection(&g_init_lock);
    register_imports(movie_mf_imports, (int)(sizeof movie_mf_imports / sizeof movie_mf_imports[0]));
}
