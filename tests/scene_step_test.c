/* runtime/scene_step.h: steady frames take the even step, a hitch is paid gently, a stall in full, a slow machine keeps real
 * time; the ride's steps stay exact and its two kinds alternate. The write of bit 27, the reset on a seek and the choice of
 * flag 16 are in game_shim_eh.c, for the game's runs to show.   clang -O2 -Wall -I runtime tests/scene_step_test.c -o build/scene_step_test */
#include <math.h>
#include <stdio.h>
#include "scene_step.h"

static int bad;
static void check(const char *what, int ok) { if (!ok) { printf("FAILED: %s\n", what); bad = 1; } }

/* A frame of the ride at cutscene_fps 60, as h_frame_clock and h_step_kind run it: the steps it takes. Each must be exactly
 * 1.0, and the kinds (*kind: 1 first, 2 second) must alternate as on the console, through pair frames too. */
static int ride_frame(float *owed, int *half, int *kind, float due) {
    int one = 1;
    float k = scene_even(owed, due, 0.5f, &one, *half);
    check("ride: a step of exactly 1.0", (one ? 2.0f * k : k) == 1.0f);
    int second = scene_second(half, one);
    check("ride: the kinds alternate", !second == (*kind == 2));
    *kind = one && !second ? 1 : 2;
    return one ? 1 : 2;
}
/* steps a second on the ride at a steady frame length */
static double ride_rate(double ms) {
    float owed = 0.0f;
    int half = 0, kind = 2;
    long steps = 0;
    for (int i = 0; i < 100000; i++) steps += ride_frame(&owed, &half, &kind, (float)(ms / (1000.0 / 30.0)));
    return steps / (100000 * ms / 1000.0);
}

int main(void) {
    float owed = 0.0f, k, top = 0.0f;
    for (int i = 0; i < 1000; i++) check("on time: the even step", scene_step(&owed, 1.0f, 1.0f) == 1.0f);
    for (int i = 0; i < 1000; i++) check("jitter within a frame: the even step", scene_step(&owed, i & 1 ? 1.2f : 0.8f, 1.0f) == 1.0f);
    owed = 0.0f;
    k = scene_step(&owed, 3.0f, 1.0f);                     /* a 100 ms frame at 30 fps */
    check("a hitch is no burst", k < 1.2f);
    owed = 0.0f;
    k = scene_step(&owed, 9.0f, 1.0f);                     /* a 300 ms stall */
    for (int i = 0; i < 200; i++) { k = scene_step(&owed, 1.0f, 1.0f); if (k > top) top = k; }
    check("a stall is paid down to a frame", owed < 1.01f && fabsf(k - 1.0f) < 0.001f && top < 2.0f);
    owed = 0.0f;
    for (int i = 0; i < 1000; i++) k = scene_step(&owed, 1.5f, 1.0f);   /* 20 fps in a 30 fps scene */
    check("a slow machine keeps real time", fabsf(k - 1.5f) < 0.001f && owed < 6.0f);
    owed = 0.0f;
    for (int i = 0; i < 100000; i++) k = scene_step(&owed, 100.0f, 1.0f);
    check("the debt is capped", owed <= 90.0f && k <= 2.0f);

    int one = 0, half = 0, kind = 2, steps = 0;
    owed = 0.0f;
    check("ride, cutscene_fps 30: the step is the game's at any lateness", scene_even(&owed, 1.2f, 1.0f, &one, 0) == 1.0f && fabsf(owed - 0.2f) < 0.001f);
    owed = 0.0f;
    for (int i = 0; i < 1000; i++) scene_even(&owed, 0.0f, 1.0f, &one, 0);
    check("ride: ahead by at most an even step", owed == -1.0f);
    for (int i = 0; i < 1000; i++) check("ride, cutscene_fps 30: never a pair", scene_even(&owed, 3.0f, 1.0f, &one, 0) == 1.0f);
    check("ride: the debt is capped", owed == 90.0f);
    one = 1; owed = 0.5f;
    k = scene_even(&owed, 0.5f, 0.5f, &one, 1);
    check("ride: no pair in the middle of a pair", one == 1 && k == 0.5f);
    k = scene_even(&owed, 0.5f, 0.5f, &one, 0);
    check("ride: a whole frame owed runs a pair", one == 0 && k == 1.0f);

    owed = 0.0f;
    for (int i = 0; i < 1000; i++) steps += ride_frame(&owed, &half, &kind, 0.5f);
    check("ride on time: one step a frame", steps == 1000 && owed == 0.0f);
    check("ride: 60 steps a second at 60 and 41-60 fps", fabs(ride_rate(1000.0 / 59.94) - 60.0) < 0.01 && fabs(ride_rate(20.0) - 60.0) < 0.01 && fabs(ride_rate(1000.0 / 41.0) - 60.0) < 0.01);
    check("ride: a pair every frame at 30 fps and below", fabs(ride_rate(1000.0 / 30.0) - 60.0) < 0.01 && fabs(ride_rate(40.0) - 50.0) < 0.01);
    unsigned r = 1;
    owed = 0.0f;
    for (int i = 0; i < 1000000; i++) {                    /* any frame lengths, a stall now and then, a menu's reset */
        r = r * 1103515245u + 12345u;
        float due = (r >> 16 & 0xff) / 128.0f + (r >> 8 & 0xff ? 0.0f : 30.0f);
        if (!(r >> 24 & 0x3f)) { owed = 0.0f; due = 0.5f; }
        ride_frame(&owed, &half, &kind, due);
        check("ride: the debt stays in its range", owed >= -1.0f && owed <= 90.0f);
    }
    check("movie scene without flag 28", movie_flag_lost(0x40000000u, 1, 60, 30));
    check("flag 28 set: nothing to do", !movie_flag_lost(0x58000000u, 1, 60, 30));
    check("not a fixed-step frame: nothing to do", !movie_flag_lost(0x00000000u, 1, 60, 30));
    check("no movie: nothing to do", !movie_flag_lost(0x40000000u, 0, 60, 30));
    check("a scene the game runs without the flag", !movie_flag_lost(0x40000000u, 1, 60, 60));
    check("an empty header", !movie_flag_lost(0x40000000u, 1, 60, 0));

    /* Arbitrary frame rates (90, 120, 144, 165, 240, 360): even step is 30 / rate, jitter stays even, slower or faster
     * real rates keep real time and never go non-positive. */
    static const int rates[] = {75, 90, 100, 120, 144, 165, 180, 240, 360};
    for (size_t ri = 0; ri < sizeof rates / sizeof rates[0]; ri++) {
        int hz = rates[ri];
        float even = 30.0f / (float)hz;
        owed = 0.0f;
        for (int i = 0; i < 1000; i++) {
            float jitter = (i & 1 ? 0.25f : -0.25f) * even;
            check("arbitrary fps: jitter within a frame keeps the even step", scene_step(&owed, even + jitter, even) == even);
        }
        /* GPU slower than target (e.g. 85 fps at target hz): keeps real time */
        owed = 0.0f;
        double sum = 0.0;
        float slow_due = 30.0f / 85.0f;
        for (int i = 0; i < 2000; i++) { k = scene_step(&owed, slow_due, even); check("arbitrary fps: positive step", k >= 0.03125f); sum += k; }
        check("arbitrary fps: slower GPU keeps real time", fabs(sum - 2000.0 * slow_due) < 6.0);
        /* Faster than target (e.g. sudden jump to 500 fps): stays positive and keeps real time */
        owed = 0.0f;
        sum = 0.0;
        float fast_due = 30.0f / 500.0f;
        for (int i = 0; i < 2000; i++) { k = scene_step(&owed, fast_due, even); check("arbitrary fps: fast step positive", k >= 0.03125f); sum += k; }
        check("arbitrary fps: faster rate keeps real time", fabs(sum - 2000.0 * fast_due) < 6.0);

        /* eva142 (scene_even) under display.c's 60 Hz next_due_tick cadence at this pace rate: exact 60 steps/s, 0 pairs */
        owed = 0.0f;
        half = 0; kind = 2;
        long long prev = 0, total_steps = 0, pairs = 0;
        for (int i = 0; i < 6000; i++) {
            long long c = prev * 60 / hz + 1;
            long long next = (c * hz + 59) / 60;
            float dt_due = (float)((double)(next - prev) * 30.0 / (double)hz);
            prev = next;
            int s = ride_frame(&owed, &half, &kind, dt_due);
            if (s == 2) pairs++;
            total_steps += s;
        }
        double secs = (double)prev / (double)hz;
        check("ride at arbitrary frame_rate: exact 60 steps/s, no pair frames", pairs == 0 && fabs((double)total_steps / secs - 60.0) < 0.01);
    }

    /* Frame rates below 60 (30, 40, 45, 50): 30 uses native 2-pass (even = 1.0); 31..59 uses 1-pass with even = 30 / rate.
     * Both keep real time on time and when dropping down to 15 fps. */
    static const int low_rates[] = {30, 40, 45, 50};
    for (size_t ri = 0; ri < sizeof low_rates / sizeof low_rates[0]; ri++) {
        int hz = low_rates[ri];
        int one_pass = hz > 30;
        float even = one_pass ? 30.0f / (float)hz : 1.0f;
        owed = 0.0f;
        for (int i = 0; i < 1000; i++) {
            float jitter = (i & 1 ? 0.25f : -0.25f) * even;
            check("low fps: jitter within a frame keeps the even step", scene_step(&owed, even + jitter, even) == even);
        }
        /* Dropping to 20 fps keeps real time (30/20 = 1.5 30-Hz frames per frame; steady debt is 8*due - 7*even) */
        owed = 0.0f;
        double sum = 0.0;
        float slow_due = 30.0f / 20.0f;
        for (int i = 0; i < 2000; i++) { k = scene_step(&owed, slow_due, even); sum += k; }
        check("low fps: dropping to 20 fps keeps real time", fabsf(k - slow_due) < 0.001f && fabs(sum + owed - 2000.0 * slow_due) < 0.1 && owed < 8.5f);
        /* Dropping to 15 fps keeps real time (30/15 = 2.0 30-Hz frames per frame, ceiling) */
        owed = 0.0f;
        sum = 0.0;
        float slow15 = 30.0f / 15.0f;
        for (int i = 0; i < 2000; i++) { k = scene_step(&owed, slow15, even); sum += k; }
        check("low fps: dropping to 15 fps keeps real time", fabsf(k - slow15) < 0.001f && fabs(sum + owed - 2000.0 * slow15) < 0.1 && owed < 13.0f);
        /* display.c next_due_tick at low rate: iv == 2 -> 30 Hz, iv == 4 -> 15 Hz */
        long long prev = 0, ticks30 = 0, ticks15 = 0;
        for (int i = 0; i < 300; i++) {
            long long next = (30 >= hz) ? prev + 1 : (((prev * 30 / hz + 1) * hz + 29) / 30);
            prev = next; ticks30++;
        }
        check("low fps: interval 2 paces at 30 Hz", fabs((double)ticks30 / ((double)prev / (double)hz) - 30.0) < 0.1);
        prev = 0;
        for (int i = 0; i < 150; i++) {
            long long next = (15 >= hz) ? prev + 1 : (((prev * 15 / hz + 1) * hz + 14) / 15);
            prev = next; ticks15++;
        }
        check("low fps: interval 4 paces at 15 Hz", fabs((double)ticks15 / ((double)prev / (double)hz) - 15.0) < 0.1);
    }
    if (!bad) printf("scene_step: ok\n");
    return bad;
}
