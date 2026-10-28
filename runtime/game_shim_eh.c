/* Shim of the game DLL generated WITH landing-pad resume entries (recomp_eh.py; build with RECOMP_SHIM=game_shim_eh.c).
 * Everything of game_shim.c, plus what the exception unwinder needs from main: the resume table, and the host's
 * nro_catch_call for the functions that have a catch handler. The host finds game_eh_bind with GetProcAddress
 * (nncxxrt.c), so an exe works with either kind of DLL. */
#include "game_shim.c"
#include "scene_step.h"

typedef struct { uint64_t lp; void (*fn)(Ctx *, uint64_t); } NroResume;
extern const NroResume resume_table_main[];
extern const int resume_count_main;

static void (*host_catch_call)(Ctx *, void (*)(Ctx *, uint64_t));
void nro_catch_call(Ctx *c, void (*body)(Ctx *, uint64_t)) { host_catch_call(c, body); }

__declspec(dllexport) void game_eh_bind(void (*catch_call)(Ctx *, void (*)(Ctx *, uint64_t)), const NroResume **table, int *count) {
    host_catch_call = catch_call;
    *table = resume_table_main;
    *count = resume_count_main;
}

#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "quad_walk.h"
/* [witchtime] Host versions of game functions (lifter/recomp_eh.py HOST_OVERRIDES: their direct calls come here).
 * f_7100b382c0(value w0, uint32 *even x1, uint32 *odd x2): even = the even bits of the value packed into 16, odd = the odd
 * bits; returns odd in x0 (the original: three bit reversals and a 32-step loop of read-modify-writes, about 1,300
 * instructions). Outputs that overlap go to the lifted original (its order of stores decides there).
 * SSBU_HOSTFN_CHECK=1 (test): on the first call, 200,000 values through both, outputs and x0 compared. */
void f_7100b382c0(Ctx *c);
static uint32_t even_bits16(uint32_t v) {
    v &= 0x55555555u; v = (v | v >> 1) & 0x33333333u; v = (v | v >> 2) & 0x0F0F0F0Fu; v = (v | v >> 4) & 0x00FF00FFu;
    return (v | v >> 8) & 0x0000FFFFu;
}
static void morton_split32(Ctx *c) {
    uint64_t a = c->x[1], b = c->x[2];
    uint32_t v = (uint32_t)c->x[0], e = even_bits16(v), o = even_bits16(v >> 1);
    memcpy((void *)(uintptr_t)a, &e, 4); memcpy((void *)(uintptr_t)b, &o, 4);
    c->x[0] = o; c->x[8] = 32; c->x[9] = 0x80000000u;
}
static void morton_check(Ctx *c) {
    static uint32_t out[4];
    uint64_t s = 0x9E3779B97F4A7C15ull;
    long bad = 0;
    for (int i = 0; i < 200000; i++) {
        Ctx t = *c, u = *c;
        s ^= s << 13; s ^= s >> 7; s ^= s << 17;
        uint32_t v = i < 64 ? (i < 32 ? 1u << i : ~(1u << (i - 32))) : (uint32_t)s;
        t.x[0] = u.x[0] = v; t.x[1] = u.x[1] = (uint64_t)(uintptr_t)&out[0]; t.x[2] = u.x[2] = (uint64_t)(uintptr_t)&out[1];
        out[0] = out[1] = 0xdeadbeef; morton_split32(&t); uint32_t e = out[0], o = out[1];
        out[0] = out[1] = 0xdeadbeef; f_7100b382c0(&u);
        if (e != out[0] || o != out[1] || t.x[0] != u.x[0]) { if (bad++ < 5) printf("hostfn: TEST: morton_split32(%08x): host %08x %08x x0 %llx, game %08x %08x x0 %llx\n", v, e, o, (unsigned long long)t.x[0], out[0], out[1], (unsigned long long)u.x[0]); }
    }
#ifndef _WIN32
    {   /* the time of each, on this thread (Linux only: clock_gettime) */
        struct timespec t0, t1, t2;
        Ctx t = *c; t.x[1] = (uint64_t)(uintptr_t)&out[0]; t.x[2] = (uint64_t)(uintptr_t)&out[1];
        clock_gettime(CLOCK_MONOTONIC, &t0);
        for (int i = 0; i < 100000; i++) { t.x[0] = (uint32_t)(i * 2654435761u); morton_split32(&t); }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        for (int i = 0; i < 100000; i++) { t.x[0] = (uint32_t)(i * 2654435761u); t.x[31] = c->x[31]; f_7100b382c0(&t); }
        clock_gettime(CLOCK_MONOTONIC, &t2);
        printf("hostfn: TEST: morton_split32 %.1f ns a call, the lifted original %.1f ns\n", ((t1.tv_sec - t0.tv_sec) * 1e9 + (t1.tv_nsec - t0.tv_nsec)) / 1e5,
               ((t2.tv_sec - t1.tv_sec) * 1e9 + (t2.tv_nsec - t1.tv_nsec)) / 1e5);
    }
#endif
    printf("hostfn: TEST: morton_split32 against the game's f_7100b382c0: 200000 values, %ld differ\n", bad);
}
void h_morton_split32(Ctx *c) {
    static int chk = -1;
    if (chk < 0) { chk = 0; if (getenv("SSBU_HOSTFN_CHECK")) morton_check(c); }
    uint64_t a = c->x[1], b = c->x[2];
    if (a < b + 4 && b < a + 4) { f_7100b382c0(c); return; }
    morton_split32(c);
}

/* f_71007e1fe4: see quad_walk.h. w1 width, w2 height, w7 levels below this node; on the stack the node's Morton index and the
 * two maps. SSBU_QUADWALK=0 off; SSBU_QUADWALK_CHECK=1 (test): the lifted walk runs too and must return the same. */
void f_71007e1fe4(Ctx *c);
void h_quad_walk(Ctx *c) {
    static int on = -1, chk;
    static int lifted;                             /* the check's walk: every node lifted (the game DLL has no thread-local storage; one thread walks) */
    if (on < 0) { const char *e = getenv("SSBU_QUADWALK"); chk = getenv("SSBU_QUADWALK_CHECK") != NULL; on = !(e && !atoi(e)); }
    int d = (int)(uint32_t)c->x[7];
    if (on && !lifted && d >= 2 && d < 16) {
        uint64_t sp = c->x[31], pa, pb;
        uint32_t idx;
        memcpy(&idx, (void *)(uintptr_t)sp, 4); memcpy(&pa, (void *)(uintptr_t)(sp + 8), 8); memcpy(&pb, (void *)(uintptr_t)(sp + 16), 8);
        int r = quad_state((const uint8_t *)(uintptr_t)pa, (const uint8_t *)(uintptr_t)pb, (uint32_t)c->x[1], (uint32_t)c->x[2], even_bits16(idx) << d, even_bits16(idx >> 1) << d, 1u << d);
        if (r >= 0) {
            if (chk) { lifted = 1; f_71007e1fe4(c); lifted = 0; if ((uint32_t)c->x[0] != (uint32_t)r) printf("quadwalk: TEST: FAILED: levels %d: %d here, %u lifted\n", d, r, (uint32_t)c->x[0]); }
            c->x[0] = (uint32_t)r;
            return;
        }
    }
    f_71007e1fe4(c);
}

/* f_7100d02de0(mode w0): the game's main loop (0x7100670b80) asks for its frame-rate mode each frame from its own state
 * flags (1 = 30 fps on the title, in menus and cutscenes; 0 = 60; 2 = 15); the mode is applied at 0x7100cfd9b8 (present
 * interval 1 / 2 / 4) and read back by 0x7100670c08 and 0x7100cb4f54 (frames -> seconds at 60 / 30 / 15). With
 * cutscene_fps = 60 (graphics.ini, SSBU_CUTSCENE_FPS) a 1 is made 0: those scenes run as gameplay does. Each change of
 * the mode asked for is logged with the wall-clock time (the start and end of a cutscene). */
void f_7100d02de0(Ctx *c);
/* frame_rate above 60 (graphics.ini; gfx_options.c sets RECOMPS_UNCAP=1): the clock below measures real time below 1.0 */
static int uncap_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("RECOMPS_UNCAP"); v = e && atoi(e); } return v; }
static int g_fps_forced;                           /* the game asked for 30 fps and runs at 60 (cutscene_fps = 60) */
static int g_one_pass;                             /* h_frame_clock: this fixed-step frame runs one logic step (h_main_pass) */
static int g_half;                                 /* the last frame ran the first kind of logic step alone, so this one is the second (h_step_kind) */
static uint32_t state_flags(void) { uint64_t fp; uint32_t f; memcpy(&fp, (void *)(uintptr_t)0x710249c388ull, 8); memcpy(&f, (void *)(uintptr_t)fp, 4); return f; }
__declspec(dllexport) volatile int game_cutscene_fps;      /* 0 = not read yet, 30 / 60; the PC menu (pcmenu.c) changes it while the game runs */
void h_fps_mode_set(Ctx *c) {
    static int last = -1;
    if (!game_cutscene_fps) { const char *e = getenv("SSBU_CUTSCENE_FPS"); game_cutscene_fps = e && atoi(e) == 60 ? 60 : 30; }
    int sixty = game_cutscene_fps == 60;
    int want = (int)(uint32_t)c->x[0];
    if (want != last) {
        struct timespec ts; timespec_get(&ts, TIME_UTC);
        printf("fps mode: the game asks for mode %d (%s) at %.3f s (wall clock; SSBU_FRAMETIMES has it per frame)%s\n", want, want == 0 ? "60 fps" : want == 1 ? "30 fps" : want == 2 ? "15 fps" : "?",
               (double)ts.tv_sec + ts.tv_nsec / 1e9, sixty && want == 1 ? ": runs at 60 (cutscene_fps = 60)" : "");
        fflush(stdout);
        last = want;
    }
    /* above 60 fps the 30 fps scenes run at frame_rate too (real time), but not the ride set pieces (#45: flag 16, the game's
     * mode 1 is 2 ticks of frame_rate there): the chapter 3 spider ride's prompts need logic steps of exactly 1.0, 60 a second (scene_even) */
    g_fps_forced = want == 1 && (uncap_on() ? !(state_flags() & 0x10000) : sixty);
    if (g_fps_forced) c->x[0] = 0;
    f_7100d02de0(c);
}
/* f_7100c1f600(clock x0, step s0, measured w1): the main loop's per-frame clock, called just before the mode is chosen. It
 * stores the last frame's length in ms at clock+0xf0 and keeps the frame's step at clock+0xe0: with w1 = 1 (gameplay) a
 * ratio of the measured frame time, with w1 = 0 (the title, menus and cutscenes: the game's state flags 28 and 30) s0 as it
 * is, 1.0 a logic step, two of them a 30 Hz frame (h_main_pass). Such a scene follows real time at any frame rate (#45, #47: its long sounds do, they never wait for a late frame):
 * the step stays even, 0.5 at 60, 1.0 at 30, and what late frames owe is paid off gently (scene_step.h), on the spider ride in whole frames (scene_even). (A fixed 0.5: 92.3 s for the 92.1 s cutscene at 60 fps, but 97.9 s at 56 fps in 4K.) A measured step (w1 = 1)
 * follows the real frame time already. */
void f_7100c1f600(Ctx *c);
/* The main loop runs two logic steps in a frame of a fixed-step scene, when flags 28 and 30 were set before the frame's first:
 * 2 steps of 1.0 a 30 Hz frame, 60 a second, each 1/60 s. They are of two kinds. In the first, flag 27 set (0x7100670d0c to
 * 0x7100670d90), the timeline update 0x7100bbcb94 recomputes its pause bits. In the second (0x7100670da4: pads, 0x7100671600) it
 * latches what was asked for: a skip, a branch, a chain, ProgressAct (0x71007078a0, 0x7100707860 read the flag). At 60 fps
 * both are 120 steps a second, so whatever counts steps runs twice as fast whatever the step (#45: the spider ride's third
 * jump prompt, not found by the census), and the first kind alone latches nothing (#65: a chained scene's skip sticks and it
 * plays on behind black; #67, #69: the ride's ProgressAct is lost and it runs into its death cut). So above 30 fps a frame
 * takes one step of twice the size and the kinds take turns (h_step_kind): on time those are the console's logic steps,
 * one a frame. f_7100b20a20 ends a logic step; called from 0x7100670d88 it comes just before the loop's test of bit 28 of
 * w21, that register's last use. Flag 21 repeats the second step until the scene player reaches a frame it seeks
 * (0x7100bb2350 clears the flag), so it is read after the first step; the console's next frame starts with the first kind. */
void f_7100b20a20(Ctx *c);
void h_main_pass(Ctx *c) {
    int first = c->x[30] == 0x7100670d8cull;
    f_7100b20a20(c);
    if (!first || !g_one_pass) return;
    if (state_flags() & 0x200000) g_half = 0; else c->x[21] &= ~(uint64_t)0x10000000;
}
/* f_710067037c: its one call, 0x7100670d18, comes just after flag 27 is set in the word at x19 and before the first step's jobs */
void f_710067037c(Ctx *c);
void h_step_kind(Ctx *c) {
    if (scene_second(&g_half, g_one_pass)) { uint32_t f = state_flags() & ~0x8000000u; memcpy((void *)(uintptr_t)c->x[19], &f, 4); }
    f_710067037c(c);
}
/* The frame-counter census (docs/STATUS.md, "Frame rates above 60"): a build lifted with WT_FPS_CENSUS=1 calls
 * fps_census before the store of every counter site (tools/wtbuild/src/lifter/fps.rs, the list in gen_eh/fps_sites.txt).
 * SSBU_FPS_CENSUS=<file>: per (site, field) the visits are counted against the game clock's updates (h_frame_clock):
 * a visit in the update after the last one of the same field (consec: a frame counter, whatever the frame rate), in the
 * same update (a loop), or later; and visits from another guest thread than the clock's. Every 1,200 updates the file
 * gets one line per site (rewritten whole): site visits fields consec same later offthread updates_with_a_visit, then
 * what a visit adds (the value stored - the field's value: code 1 / 2 / 4 an integer of that size, 0 a float): the mean
 * of its size, its least and its largest; and a first line with the updates counted. scripts/fps_census.py compares a
 * run at 60 with one above 60. */
static volatile uint64_t g_upd;                 /* the clock's updates so far */
static Ctx *volatile g_clock_ctx;               /* the guest thread that runs the clock */
typedef struct { uint64_t site, field, last; } FcRec;
typedef struct { uint64_t site, visits, fields, consec, same, later, off, upds, last; double dsum, dmin, dmax; } FcSite;
#define FC_RECS (1u << 21)
#define FC_SITES (1u << 16)
static FcRec *fc_rec; static FcSite *fc_site; static volatile int fc_lock; static int fc_on = -1; static const char *fc_path;
static uint64_t fc_hash(uint64_t a) { a ^= a >> 33; a *= 0xff51afd7ed558ccdull; a ^= a >> 33; return a; }
static void fc_dump(void) {
    FILE *f = fopen(fc_path, "w");
    if (!f) return;
    fprintf(f, "updates %llu\n", (unsigned long long)g_upd);
    for (uint32_t i = 0; i < FC_SITES; i++) {
        FcSite *t = &fc_site[i];
        if (t->site) fprintf(f, "%llx %llu %llu %llu %llu %llu %llu %llu %.6g %.6g %.6g\n", (unsigned long long)t->site, (unsigned long long)t->visits, (unsigned long long)t->fields,
                             (unsigned long long)t->consec, (unsigned long long)t->same, (unsigned long long)t->later, (unsigned long long)t->off, (unsigned long long)t->upds,
                             t->dsum / (double)(t->visits ? t->visits : 1), t->dmin, t->dmax);
    }
    fclose(f);
}
void fps_census(Ctx *c, uint64_t site, uint64_t field, uint64_t v, int code) {
    if (fc_on <= 0) {
        if (fc_on == 0) return;
        fc_path = getenv("SSBU_FPS_CENSUS");
        if (fc_path && *fc_path) { fc_rec = calloc(FC_RECS, sizeof *fc_rec); fc_site = calloc(FC_SITES, sizeof *fc_site); }
        fc_on = fc_rec && fc_site;
        if (!fc_on) return;
        printf("fps census: counter sites recorded to %s\n", fc_path);
    }
    uint64_t u = g_upd;
    double d;
    if (code) {
        uint32_t o = 0; memcpy(&o, (void *)(uintptr_t)field, (size_t)code);
        int sh = 64 - 8 * code;
        d = (double)((int64_t)((v - o) << sh) >> sh);
    } else {
        float a, b; uint32_t w = (uint32_t)v; memcpy(&a, (void *)(uintptr_t)field, 4); memcpy(&b, &w, 4);
        d = (double)b - (double)a;
        if (d != d) d = 0.0;
    }
    while (__atomic_exchange_n(&fc_lock, 1, __ATOMIC_ACQUIRE)) {}
    FcSite *t = 0;
    for (uint32_t i = (uint32_t)fc_hash(site) & (FC_SITES - 1), n = 0; n < FC_SITES; i = (i + 1) & (FC_SITES - 1), n++)
        if (fc_site[i].site == site || !fc_site[i].site) { t = &fc_site[i]; break; }
    FcRec *r = 0;
    for (uint32_t i = (uint32_t)fc_hash(site * 31 + field) & (FC_RECS - 1), n = 0; n < 64; i = (i + 1) & (FC_RECS - 1), n++)
        if ((fc_rec[i].site == site && fc_rec[i].field == field) || !fc_rec[i].site) { r = &fc_rec[i]; break; }
    if (t) {
        if (!t->site) { t->site = site; t->last = ~0ull; t->dmin = t->dmax = d; }
        t->visits++;
        t->dsum += d < 0 ? -d : d; if (d < t->dmin) t->dmin = d; if (d > t->dmax) t->dmax = d;
        if (t->last != u) { t->upds++; t->last = u; }
        if (c != g_clock_ctx) t->off++;
        if (r && !r->site) { r->site = site; r->field = field; t->fields++; }
        else if (r) { if (r->last == u) t->same++; else if (r->last + 1 == u) t->consec++; else t->later++; }
        if (r) r->last = u;
    }
    __atomic_store_n(&fc_lock, 0, __ATOMIC_RELEASE);
}
/* Frame counters at any frame rate (tools/wtbuild/fps_counters.txt: the sites; the lifter makes their +/-1 a call of
 * fps_ci / fps_cf). The game's counters move 1 a frame, made for 60 Hz frames (30 Hz in the 30 fps scenes). Each clock
 * update adds its step (frames of the game's own cadence) to an accumulator; G = the whole frames due so far (at most 4
 * an update). A counter visit moves by the frames due since that field's last visit when it also ran in the previous
 * update, so it keeps the game's time at 90, 120, 144 fps or a rate that changes from frame to frame; a second visit in
 * the same update moves as the first did (a counter the game steps twice a frame); any other visit (the first, a one-off
 * event) moves 1 as made. A countdown never passes 0 in one visit and takes the stock step at 0 with no frame due (a
 * test of the result for 0 fires as made); an up-counter moves at most 1 a visit (a test for an exact value is not
 * passed over); what a limit holds back is owed to the next visits (at most 8). Off (the stock 1) unless frame_rate is
 * above 60; SSBU_FPS_COUNTERS=0 keeps the stock one above 60 too (tests: is a fault the sites'). The rules of B2recomp's
 * fps_patch.py (finding 134), written again for B3. */
static int fc_sites_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("SSBU_FPS_COUNTERS"); v = uncap_on() && !(e && !atoi(e)); } return v; }
static float fc_stab[256];                  /* the step of update n at [n & 255] (fps_fs) */
static uint64_t fc_gtab[256];               /* G of update n at [n & 255], stored before n is published: other threads read n, then its G */
typedef struct { uint64_t key, field, S, U; int64_t now, carry; float f; } FcCnt;
#define FC_CNTS 4096
static FcCnt fc_cnt[FC_CNTS];
static FcCnt *fc_find(uint64_t key, uint64_t field) {
    uint64_t h = fc_hash(key * 0x9E3779B97F4A7C15ull ^ field);
    FcCnt *victim = 0;
    for (int i = 0; i < 8; i++) {
        FcCnt *r = &fc_cnt[(h + (uint64_t)i) % FC_CNTS];
        uint64_t k = __atomic_load_n(&r->key, __ATOMIC_ACQUIRE);
        if (k == key && r->field == field) return r;
        if (!k) {                                    /* a free slot, claimed with a CAS (two threads may insert) */
            if (__atomic_compare_exchange_n(&r->key, &k, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) { r->field = field; r->U = ~0ull; r->now = r->carry = 0; return r; }
            if (k == key && r->field == field) return r;
            continue;
        }
        if (!victim || r->U < victim->U) victim = r; /* all 8 taken: the one visited longest ago gives way */
    }
    victim->key = key; victim->field = field; victim->U = ~0ull; victim->now = victim->carry = 0;
    return victim;
}
static int64_t fc_visit(uint64_t key, uint64_t field, FcCnt **rp) {
    uint64_t n = __atomic_load_n(&g_upd, __ATOMIC_ACQUIRE), g = fc_gtab[n & 255];
    FcCnt *r = fc_find(key, field);
    int64_t de;
    if (r->U == n) de = r->now;
    else {
        if (r->U == n - 1) { de = (int64_t)(g - r->S); if (de < 0) de = 0; if (de > 4) de = 4; de += r->carry; }
        else de = 1;
        r->carry = 0;
        if (de > 8) { r->carry = de - 8; de = 8; }
        r->now = de;
    }
    r->S = g; r->U = n;
    *rp = r;
    return de;
}
static void fc_limit(FcCnt *r, int64_t *de, int64_t keep) {
    if (*de > keep) { r->carry += *de - keep; if (r->carry > 8) r->carry = 8; *de = keep; }
}
/* an integer counter's add / sub of k (w: its value before): what it moves by */
uint32_t fps_ci(Ctx *c, uint64_t key, uint64_t field, uint32_t wb, int k, int sign) {
    (void)c;
    if (!fc_sites_on() || !g_upd) return (uint32_t)k;
    FcCnt *r;
    int64_t de = fc_visit(key, field, &r), w = (int32_t)wb;
    if (sign < 0) { if (de == 0 && w == 0) de = 1; fc_limit(r, &de, w > k ? 1 + (w - k) / k : 1); }
    else fc_limit(r, &de, 1);
    return (uint32_t)(k * de);
}
/* a float counter's add of +/-1.0 (w: its value before): the frames that multiply the 1.0 */
int fps_cf(Ctx *c, uint64_t key, uint64_t field, float w, int sign) {
    (void)c;
    if (!fc_sites_on() || !g_upd) return 1;
    FcCnt *r;
    int64_t de = fc_visit(key, field, &r);
    fc_limit(r, &de, sign < 0 ? (w >= 2.0f ? 1 + (int64_t)(w - 1.0f) : 1) : 1);
    return (int)de;
}
/* a float the game adds a per-frame amount to (x +/- a): the factor of a, the step of this update (frames of game time,
 * fractional: motion stays smooth at any rate) when the site also ran in the previous update; a second visit in the same
 * update takes the same factor; any other visit (a one-off event) 1, as made. Off: 1. */
float fps_fs(Ctx *c, uint64_t key, uint64_t field) {
    (void)c;
    if (!fc_sites_on() || !g_upd) return 1.0f;
    uint64_t n = __atomic_load_n(&g_upd, __ATOMIC_ACQUIRE);
    FcCnt *r = fc_find(key, field);
    float f = r->U == n ? r->f : r->U == n - 1 ? fc_stab[n & 255] : 1.0f;
    r->f = f; r->U = n; r->S = fc_gtab[n & 255];
    return f;
}
/* f_71007077d0(w0): the game's set / clear of flag 28, called by a scene's start (0x7100bb1bf4) and end (0x7100bb2a6c, after it
 * zeroes the movie handle at +168). A movie scene left without it plays on with the subtitles of a 30 fps scene (#72), so it is
 * set again here. Not seen on Linux; the line in the log tells whether that is what a reporter's PC does. */
void f_71007077d0(Ctx *c);
static void keep_movie_flag(Ctx *c) {
    static uint64_t told;
    uint64_t slots, x0 = c->x[0];
    memcpy(&slots, (void *)(uintptr_t)0x71024aae68ull, 8);
    for (int i = 0; slots && i < 8; i++) {
        uint64_t q = slots + 112 * (uint64_t)i, sc, hdr; uint32_t on, movie; int8_t fps, base;
        memcpy(&on, (void *)(uintptr_t)q, 4); memcpy(&sc, (void *)(uintptr_t)(q + 56), 8);
        if (!(on & 1) || !sc) continue;
        memcpy(&movie, (void *)(uintptr_t)(sc + 168), 4); memcpy(&hdr, (void *)(uintptr_t)(sc + 8), 8);
        memcpy(&fps, (void *)(uintptr_t)(hdr + 144), 1); memcpy(&base, (void *)(uintptr_t)(hdr + 145), 1);
        uint32_t flags = state_flags();
        if (!movie_flag_lost(flags, movie != 0, fps, base)) continue;
        c->x[0] = 1; f_71007077d0(c); c->x[0] = x0;
        if (sc != told) { told = sc; printf("movie scene %d: flag 28 was clear, set again (flags %08x -> %08x)\n", i, flags, state_flags()); fflush(stdout); }
        return;
    }
}
void h_frame_clock(Ctx *c) {
    uint64_t obj = c->x[0];
    g_clock_ctx = c;
    keep_movie_flag(c);
    int fixed = (uint32_t)c->x[1] == 0;
    /* s0 is 4.0 in the game's mode 2 */
    g_one_pass = fixed && c->v[0].f[0] == 1.0f && (game_cutscene_fps == 60 || uncap_on());
    f_7100c1f600(c);
    {
        static float owed;                         /* 30 Hz frames of real time the scene is behind (scene_step.h) */
        static int was_fixed;
        if (fixed) {
            float v, ms;
            uint32_t flags = state_flags();
            int ride = flags & 0x10000;
            memcpy(&v, (void *)(uintptr_t)(obj + 0xe0), 4); memcpy(&ms, (void *)(uintptr_t)(obj + 0xf0), 4);
            float even = g_one_pass ? 0.5f : 1.0f;     /* the step of a frame on time; above 60 fps a frame is shorter */
            float due = ms > 0.0f && ms == ms ? ms / (1000.0f / 30.0f) : even;
            if (uncap_on() && due < even && !ride) even = due;
            /* the load before a scene is not the scene's time, nor is its pause menu (flags 14 and 0: its clock stands) */
            if (!was_fixed || flags & 0x4001) { owed = 0.0f; due = even; }
            float k = ride ? scene_even(&owed, due, even, &g_one_pass, g_half) : scene_step(&owed, due, even);
            v *= g_one_pass ? 2.0f * k : k;            /* one logic step: it is 2 of the 1/60 s ones of a 30 Hz frame */
            memcpy((void *)(uintptr_t)(obj + 0xe0), &v, 4);
        }
        was_fixed = fixed;
    }
    /* frame_rate above 60: a measured step (w1 = 1) is this frame's ms / the ms of a 60 Hz frame (+0xec) with the floor 1.0
     * lowered to 1/16 (960 fps) and no carry; the 2.0 ceiling (slow below 30 fps) and the global override (7100ba3300's float,
     * when above 0) stay the game's. The game's frame counters of tools/wtbuild/fps_counters.txt follow (fps_ci below). */
    if (!fixed && uncap_on()) {
        uint64_t p; float ov, ms, per;
        memcpy(&p, (void *)(uintptr_t)0x71024aaea0ull, 8); memcpy(&ov, (void *)(uintptr_t)p, 4);
        memcpy(&ms, (void *)(uintptr_t)(obj + 0xf0), 4); memcpy(&per, (void *)(uintptr_t)(obj + 0xec), 4);
        if (!(ov > 0.0f) && per > 0.0f && ms == ms) {
            float s = ms / per, z = 0.0f;
            s = s < 0.0625f ? 0.0625f : s > 2.0f ? 2.0f : s;
            memcpy((void *)(uintptr_t)(obj + 0xe0), &s, 4); memcpy((void *)(uintptr_t)(obj + 0xe4), &z, 4);
        }
    }
    {   /* this update's step into the frames due (fps_ci), then the update is published */
        static double acc; static uint64_t g;
        float st; memcpy(&st, (void *)(uintptr_t)(obj + 0xe0), 4);
        acc += st > 0.0f && st == st ? st : 0.0;
        int t = (int)(acc + 1e-3);
        if (t > 4) { t = 4; acc = 4.0; }
        acc -= t; if (acc < 0) acc = 0;
        g += (uint64_t)t;
        fc_gtab[(g_upd + 1) & 255] = g;
        fc_stab[(g_upd + 1) & 255] = st > 0.0f && st == st ? st : 1.0f;
    }
    if (fc_on > 0) { while (__atomic_exchange_n(&fc_lock, 1, __ATOMIC_ACQUIRE)) {} g_upd++; if (g_upd % 1200 == 0) fc_dump(); __atomic_store_n(&fc_lock, 0, __ATOMIC_RELEASE); }
    else __atomic_store_n(&g_upd, g_upd + 1, __ATOMIC_RELEASE);
}

/* [witchtime] aspect ratios (graphics.ini aspect_ratio; gfx_options.c hands it on as RECOMPS_ASPECT = width / height;
 * docs/STATUS.md "Aspect ratios"). f_710070a740(int *w x0, int *h x1) answers the docked screen size, 1600x900;
 * f_710070a760 the handheld one, 1280x720. Their callers (the NVN window textures, 0x7100d0c990; the docked / handheld
 * switch, 0x7100cfdc50) take it as the window's size. The height stays, the width follows the ratio (even). The scene's
 * targets keep their 16:9 sizes in the game (nvn_vk_draw.c makes them wider: rs_xstretch); the cameras follow below.
 * shortcut: checked on the title screen and 4 minutes of Shibuya (movies, menus and a real ultrawide monitor not yet). */
void f_710070a740(Ctx *c);
void f_710070a760(Ctx *c);
static float aspect_want(void) { static float v = -1; if (v < 0) { const char *e = getenv("RECOMPS_ASPECT"); v = e ? (float)atof(e) : 0.0f; if (!(v >= 0.25f && v <= 8.0f)) v = 0.0f; } return v; }
static void screen_size_aspect(Ctx *c) {
    float a = aspect_want();
    if (a <= 0.0f) return;
    int32_t w, h; memcpy(&w, (void *)(uintptr_t)c->x[0], 4); memcpy(&h, (void *)(uintptr_t)c->x[1], 4);
    w = 2 * (int32_t)((float)h * a / 2.0f + 0.5f);
    memcpy((void *)(uintptr_t)c->x[0], &w, 4);
}
void h_screen_docked(Ctx *c) { f_710070a740(c); screen_size_aspect(c); }      /* the originals only store: x0, x1 stay */
void h_screen_handheld(Ctx *c) { f_710070a760(c); screen_size_aspect(c); }
/* f_7100d01380(config x0): the graphics set-up (one caller, 0x710069d1ec). config + 0x90 is the size of the heap the
 * render targets and the window textures are placed in, 0x10400000 (260 MB; the game's last memory block, 0x10600000).
 * Wider than 16:9 it grows with the width (r2_imports.c adds as much to the memory the game is told it has): at 21:9
 * the 84th render target, 1600x896, did not fit. */
void f_7100d01380(Ctx *c);
void h_gfx_setup(Ctx *c) {
    float a = aspect_want();
    if (a > 16.0f / 9.0f) {
        uint64_t sz; memcpy(&sz, (void *)(uintptr_t)(c->x[0] + 0x90), 8);
        uint64_t extra = ((uint64_t)((double)sz * (a * 9.0 / 16.0 - 1.0)) + 0x1fffff) & ~0x1fffffull;      /* same as r2_imports.c */
        sz += extra;
        memcpy((void *)(uintptr_t)(c->x[0] + 0x90), &sz, 8);
        printf("aspect: the render-target heap is 0x%llx bytes (RECOMPS_ASPECT)\n", (unsigned long long)sz);
    }
    f_7100d01380(c);
}
/* f_7100cba90c(camera x0, fov s0, near s1, far s2, width w1, height w2): a camera's perspective (fov: vertical, radians);
 * its aspect (camera + 0x3d8, read by the projection 0x7100cb9920) is width / height. Its callers give fixed sizes:
 * 1280x720 (0x7100624e10, 0x7100c04b00), 256 high (0x7100646b64, not 16:9), the caller's own (0x710063fe50). A 16:9 size
 * (within 1 %) gets the screen's shape: wider, the same vertical view and more at the sides (Hor+); narrower, the same
 * horizontal view and more above and below (Vert+: the vertical angle grows). */
void f_7100cba90c(Ctx *c);
float tanf(float); float atanf(float);
static float vert_plus(float fov, float a) { return 2.0f * atanf(tanf(fov * 0.5f) * (16.0f / 9.0f) / a); }
void h_camera_perspective(Ctx *c) {
    float a = aspect_want();
    int32_t w = (int32_t)c->x[1], h = (int32_t)c->x[2];
    float d = h > 0 ? (float)w / (float)h - 16.0f / 9.0f : 1.0f;
    if (a > 0.0f && d < 0.018f && d > -0.018f) {
        c->x[1] = (uint32_t)(int32_t)((float)h * a + 0.5f);
        if (a < 16.0f / 9.0f) c->v[0].f[0] = vert_plus(c->v[0].f[0], a);
    }
    f_7100cba90c(c);
}
/* f_7100cba97c(camera x0, fov s0): the vertical angle alone (3 callers: the scenes' cameras move it every frame). On a camera
 * whose aspect is the screen's (set above), narrower than 16:9, it grows as above. */
void f_7100cba97c(Ctx *c);
void h_camera_fov(Ctx *c) {
    float a = aspect_want(), ca;
    uint64_t cam; memcpy(&cam, (void *)(uintptr_t)c->x[0], 8); memcpy(&ca, (void *)(uintptr_t)(cam + 0x3d8), 4);
    if (a > 0.0f && a < 16.0f / 9.0f && ca == ca && ca - a < 0.001f && a - ca < 0.001f) c->v[0].f[0] = vert_plus(c->v[0].f[0], a);
    f_7100cba97c(c);
}
