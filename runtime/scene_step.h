/* The step of a fixed-step frame (game_shim_eh.c h_frame_clock), in 30 Hz frames (#47). A cutscene's long sounds run on real
 * time, but the timeline starts them and its other sounds, so a scene that lags or rushes takes them along. On time the step
 * is even, so jitter in the measured length does not move the picture. *owed is how far the scene is behind real time: up to
 * one even step stays owed, past that an eighth of the rest goes each frame, at most 2.0 a frame, so a stall is paid without
 * a burst and a machine below the frame rate still keeps real time (down to 15 fps; slower, it lags by at most 90 frames). */
#ifndef SCENE_STEP_H
#define SCENE_STEP_H
static float scene_step(float *owed, float due, float even) {
    float o = *owed + due - even, k = even;
    if (o > 90.0f) o = 90.0f;                      /* a window held for minutes must not fast-forward the scene for minutes */
    if (o > even) k += (o - even) / 8.0f; else if (o < -even) k += (o + even) / 8.0f;
    if (k > 2.0f) k = 2.0f;
    *owed = o + even - k;
    return k;
}
/* The step of the ride set pieces (flag 16): the spider ride's prompt code (eva142, 0x7100867380) works only when each logic
 * step advances exactly 1.0. Above that a window's end is jumped over and the ride hangs before the parking lot; below it a
 * model has the ride reach its death cut (one run at 0.9 passed). So the step never changes, and a whole even step owed runs both logic steps of a console frame (*one = 0, even doubled).
 * half: the last frame ran the first kind alone (h_step_kind), so a pair can't start here. Ahead of real time at most one
 * even step is kept: only a frame rate above 60 gets there, and it runs slightly fast rather than slow down. */
static float scene_even(float *owed, float due, float even, int *one, int half) {
    if (*one && !half && *owed + due - even >= even) { *one = 0; even *= 2.0f; }
    float o = *owed + due - even;
    *owed = o < -even ? -even : o > 90.0f ? 90.0f : o;
    return even;
}
/* A single step above 30 fps: the kinds take turns (game_shim_eh.c h_step_kind). half is scene_even's; is this one the second? */
static int scene_second(int *half, int one) {
    int second = one && *half;
    *half = one && !*half;
    return second;
}
/* The frame of a movie scene is the movie's frame times 2 only while flags 28 and 30 are set (0x7100707840), so with 28 clear
 * its subtitles and cues run at half speed to the scene's end (#72). fps / base == 2 is the test of the scene's start. */
static int movie_flag_lost(unsigned flags, int movie, int fps, int base) {
    return movie && base && fps / base == 2 && (flags & 0x50000000u) == 0x40000000u;
}
#endif
