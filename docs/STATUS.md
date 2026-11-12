# Witch Time status

Stage 5 (2026-10-24): the title, the cutscenes (VP9 movies through FFmpeg), the prologue and **the first fights** run, with
sound and scripted pad input (punch, kick, dodge, jump, movement: Verse 1 cleared, Verse 2 reached), without a "not
implemented" stop in 1,500 s of the prologue and 1,100 s of chapter 1 (tessellation now drawn, no WT_VK_MISSING=warn). 30 fps movies; **chapter-1 fights about 59 fps**
(were 44-52: a texture walk on every NVN flush, see Performance). Vulkan validation (core, sync, GPU-assisted): clean apart from benign / known items, listed below.

## What builds

`wtbuild setup --dump <dump>` runs the steps and they pass (check is a developer's run; `setup` skips it):

| step | result |
|---|---|
| deps, verify, convert, mods | the dump is the expected build (main build id `b47cc79b...`); main, sdk, subsdk0 -> ELF, segment hashes ok |
| tables | `runtime/gen/*` from the player's sdk/main: layout, 600 imports, 86 lifted SDK functions (NVN texture storage and the GPU kind chooser among them), 90 of 105 NVN device answers |
| lift | 55,277 functions, 5.1 M instructions -> 407 C files, 5 s |
| compile | all 409 files compile, 0 failures (about 12 min with 15 jobs on a loaded machine; 47 large functions fall back to lower optimisation) |
| link | `build/game.so` (154 MB) + `build/witch-time-game` (4.4 MB) + `libwt_glslang.so` |
| check | `tools/lifter-check` (developers; setup skips it): B3's main's 607 words in test_ext's and test_b3's forms against Unicorn, 300 trials each: one failure, `sqrdmulh` by element when both 32-bit lanes are INT32_MIN (`v_ielem` in runtime/rt_ext.h, open); `--synthetic 2000` passes |

Tools: `wtbuild pkz list` (`.pkz` entries; `witch-time-game --pkz-dump` extracts and checks every entry, Kraken
included, section "RomFS archives"), `wtbuild dat list | extract` (DAT), `wtbuild nvn-procs` (NVN entry points of the
sdk); imports vs handlers: `runtime/gen/imports_all.txt` and a run with `WT_STRICT=report` (docs/RE_NOTES.md section 2).

## How far it boots (stage 2, 2026-10-23)

```
cd build && mkdir -p run && cd run
WT_FOCUS_NOTIFY=0 WT_VK_MISSING=warn WT_ROMFS=$PWD/../../targets/romfs WT_USERDATA=$PWD/userdata \
    ../witch-time-game ../../targets/exefs/main.elf 240
```

(argument 2 = seconds before the runtime stops it; the log goes to the console, the first calls of every import to
`boot_trace.txt`.) The process, its window and its audio stream are named **Witch Time** (app id `witchtime`), so they do not
clash with B2recomp: `pgrep -x "Witch Time"` finds it. The executable is `build/witch-time-game` (`witch-time-game.exe` on Windows; it was
`game_native`, the same name as B2recomp's, until 2026-10-24, then `b3-game` until 2026-10-27).

- `WT_FOCUS_NOTIFY=0`: turns off the applet model's focus tracking (nnmisc.c), so the game is never told it lost the
  focus. Losing the focus no longer suspends the game either way (that freeze silenced it whenever another window was in
  front; removed 2026-10-29).
- `WT_VK_MISSING=warn`: without it the Vulkan back end stops at the first operation it cannot do (see the blockers below);
  with it those are skipped and logged once each.
- Test aids used for the results below: `WT_SHOW=0` (window off screen), `WT_SHOT_EVERY=<n> WT_SHOT_DIR=<dir>` (a .bmp
  every n frames), `WT_SCRIPT="30:Z 36:Z 42:X ..."` (timed keys: Z/X = A/B, ! = Plus, u/d = up/down).

What runs now, measured on the developer's PC (RX 9070 XT, RADV), Vulkan back end:

1. Everything of stage 1 (image, `DT_INIT`, constructors, `nnMain`, RomFS, 72 `.pkz`, threads, NVN device / queue / pools).
2. **NVN texture setup**: storage size / alignment / class / sparse layout run the sdk's own NVN 13.3.2 code, found by name
   (section "NVN" of `docs/RE_NOTES.md`); self-test 22,396 cases, 0 failures (`WT_SDK_SELFTEST=1 RECOMPS_NVN_SELFTEST=1`).
   About 2,300 textures, 40,000 buffers, 7,900 shader programs (7,830 graphics, 74 compute) are set up.
3. Wwise starts (IOThread, BankManager, EventManager, Opus decoding threads), 23 guest threads in all.
4. **Frames are presented**: the loading emblem (frame ~15), then the **title screen** at about frame 600 (~20 s):
   "BAYONETTA 3", "Press any button", the copyright line and the game version **1.2.0**.
5. With scripted input: title -> **main menu** (Story / Options) -> difficulty -> Naive Angel Mode help and choice ->
   brightness -> auto-save notice -> **Story starts**: the opening movie `movie/ev0000.webm` cannot be played (no decoder on
   Linux, a failing placeholder) and is skipped; the game goes on into the prologue chapter "Phenomenal Affirmation":
   Plus opens the in-game **pause menu** (play time counting) and Photo Mode works.
6. 60 fps on the title and in the prologue (vsync); 240 s runs end on their timer, no crash.

**Nothing of the 3D world is visible**: in the prologue the screen is black behind the menus (57 draws per frame, about
5 % of draws skipped). All 2D UI (menus, text, help pictures, pause menu, photo mode) renders. No sound (see below).

## Stage 3 (2026-10-23): the 3D world, shader translation, Vulkan gaps

Stage 3 was cut short: the PC is in use, so after the first runs below **no game run was allowed**. Everything after
those runs was checked offline only (builds, the translator check below). What a run showed before the stop:

- **The Vulkan back end skipped every indirect draw** (`DrawArraysIndirect`, `DrawElementsIndirect`, `MultiDraw...Indirect`,
  `...IndirectCount`, 261,000 calls in 240 s): it had no case for the front end's `OP_DRAW_INDIRECT`. Implemented (one Vulkan
  draw per record, the records read on the CPU once compute output there has landed). Two 60 s runs and one 150 s run
  after that: no crash, the "DrawIndirect not implemented" report gone.
- **"3D black" in chapter 1 is a loading screen that never ends.** Zoomed in, the faint dot bottom right is the loading
  emblem, dimmed: after Story (or Continue) the game stays on that screen; the pause menu works during it, which is why
  stage 2 took it for gameplay. Thread samples (gdb) during it: no deadlock; the main thread presents and waits on GPU
  syncs, the loader and file threads are idle. What it waits for is not known yet.
- One frame's pass list (`WT_VK_DUMPTEX=<frame>,1x1,0`): about 65 draws, a 17-draw depth pass, a 16-draw shadow map, a
  bloom chain, no scene pass: consistent with GPU-driven drawing whose culling compute (atomics) did not run.
- **Audio**: Wwise opened its output with SDK 13's event-less `OpenDefaultAudioOut(AudioOut*, const AudioOutParameter&)`,
  which had no handler. Added; a run confirmed sound (48 kHz stereo on PipeWire, buffers appended and released). Whether this
  unblocks the chapter loading was not observed (the run was interrupted).
- One 150 s run presented only 518 frames (about 3.5 fps), two later 60 s runs 2,300+ frames (with and without the audio
  output): the slow run is not explained.

Offline, `tools/mxcheck.c` translates every stage a run dumped and compiles it with glslang (the same path as the Vulkan
back end):

```
WT_SHADERDUMP_ALL=<dir> ...game run...                       # once: s<program>_<index>.mxd, the game's code: keep out of the repo
clang -O1 -I runtime tools/mxcheck.c runtime/mx_translate.c -lglslang -lglslang-default-resource-limits -lm -o build/mxcheck
build/mxcheck <dir> [-v] [-o <dir for the GLSL of failing stages>]
```

On the 7,838 vertex / fragment / compute stages of a run: **5,627 compiled at the start of stage 3, 7,838 now**. Added:
`SHFL` (warp shuffle on `subgroupShuffle`, 32-lane warps inside 32/64-wide subgroups; a fragment stage reads its quad with fine derivatives and the lane from `gl_FragCoord`, issues 66 and 54, from a snapshot its dispatcher takes at the loop header: the loop is wave-uniform, `while (subgroupAny(pc != 0u))`, so a pixel that reached `EXIT` or was killed stays in it with its registers frozen and readable, as a Maxwell thread's are; a `SHFL` that is not first in its case starts one, entered through the header; issue 66's white hit flash on AMD's Windows driver, where a neighbour that is not executing the case reads as garbage), `KIL` as a flag in the dispatcher (the pixel runs on to `EXIT` as on Maxwell; the discard comes after the loop: issue 26's hang) and a discard, so a demote, in a stage without one, `VOTE` / lane id / lane masks on KHR
subgroups, `FSWZADD` (with `.NDV`), `BAR.SYNC`, `ATOM` / `RED` / `ATOMS` (global through per-op helpers on the storage
blocks, CAS loops where GLSL has no atomic), image accesses whose handle register was loaded from an image slot (all 799 of
them), texel offsets from registers (lowered), `VMNMX`, `SAM` / `RAM`. The 29 programs with **tessellation** stages followed
later (see "Tessellation", 2026-10-24); geometry stages are still not covered (none in the dump).

Vulkan back end, built but not run: **storage images** (417 fragment and 4 compute stages store to images; the back end
ignored `nvnCommandBufferBindImage`): handles per stage, STORAGE usage on colour images whose format allows it, views, the
descriptor layouts (per pipeline, universal, compute), GENERAL layout, barriers after a storing pass or dispatch. **Depth
bounds test** as dynamic state.

Alpha to coverage is dynamic state too (extended dynamic state 3), except on AMD's Windows driver for RDNA 2 and older (driverInfo
"AMD proprietary shader compiler") without pipeline libraries: that driver applies it while the dynamic state says off (issue 50, an
RX 580: a dithered, washed-out world without Bayonetta's body), so there it is part of the pipeline key. `WT_VK_DYN_A2C=0 / 1`
forces either way on whole pipelines (`WT_VK_GPL=0`); `WT_VK_TEST` bit 1024 (test) applies it to every draw, as that driver does.

### Game runs of 2026-10-24 (the list above, validated)

Run: from the save, `WT_PRESS=Z,25,3` (A every 3 s), Vulkan back end, no `WT_VK_MISSING`. What renders now (pictures of
the developer's run: title, Bayonetta falling through the sky at the start of chapter 1, a summoned demon sequence):

1. **The chapter-1 load finishes.** The "endless loading screen" of stage 2 was the scene rendering every lit pixel as
   Inf: the lighting divides by an exposure value that three **draws without render targets** compute (a luminance
   reduction over 90x51 / 90x1 whose fragment shaders only store to buffers). The back end skipped such draws and, for the
   graphics stages, also bound stored-to buffers as stream-ring copies (the writes were lost). Both fixed; found with the
   pass dump (`WT_VK_DUMPTEX`), the draw log (`WT_VK_DRAWLOG`, now with storage ranges and guest-memory writes) and a
   write watch on the exposure buffer (`WT_VK_WATCHFS`, test only).
2. **Frame rate**: 60 fps (vsync) on the title and in chapter 1 (16,841 frames in 300 s). The 3.5 fps case had two causes:
   (a) compute output is copied back to guest memory every frame (about 16 MB) and the compute buffers were in device-local
   host-visible memory (BAR), which the CPU reads at a few tens of MB/s: 300 ms a frame; now host-cached memory (4 ms);
   (b) after a translator version change (`MX_VK_VERSION`) every stage is translated and compiled again while the loading
   screens are held for the workers: the first run after such a change takes minutes to reach the title (one-time).
3. **Indirect draws**: the culled scene draws (96 draws in the depth pass where there were 17); indirect records written
   by compute are read through the compute buffer's mapping (no landing of every buffer).
4. **Storage images**: exercised (fragment program #1693 stores to an image every frame), no stop, no crash. Not checked by
   the Vulkan validation layer: it is not installed on this PC (`vulkan-validation-layers`).
5. **Depth bounds**: used by the game (stage 2 stopped on it), runs now; not checked picture by picture.
6. **Title background and glow**: the backdrop is the 3D demon of the title; the "Press any button" glow is no longer
   blown out (both were the Inf exposure).
7. **No "not implemented" stop** in 300 s without `WT_VK_MISSING=warn`; 0 skipped draws.
8. Audio: Wwise output plays (stage 3, first half).

Still open from the list: the translated instructions are only checked by these pictures (no reference images of the
console), the texel-offset lowering's implicit-LOD shortcut, `SAM` / `RAM` as no-ops.

## Stage 4 (2026-10-24): movies, the prologue

- **Movies on Linux**: `nnmovie_mf.c` keeps the movie:: protocol (Extractor / Decoder vtables, buffers, events) and uses
  FFmpeg underneath on Linux (libavformat on the bytes the game's reader returns, libavcodec per selected track, swscale to
  the tight NV12 the game uploads, swresample to s16 for audio tracks); Windows keeps Media Foundation (only its calls moved
  into `src_read` / `src_seek` / `src_close`; **the Windows build was not rebuilt**). B3's movies are VP9 1600x900 30 fps
  without an audio track (the soundtrack comes through Wwise).
- **Movies on Windows (2026-10-31, #68)**: now FFmpeg too, in software, from DLLs in `prebuilt\`; Media Foundation is left to the
  album's clips. A movie that cannot start is not a cosmetic loss: the game gives up on it, and without `ev0000` New Game drops
  Bayonetta into the empty sky of the stage for ever. Reproduced frame for frame on Linux with `avcodec_find_decoder` returning
  NULL, and with `WT_MOVIE=0`; a movie that fails after its first sample does not do it. Media Foundation's VP9 depends on what
  the PC has installed, so that report's PC probably lacked it (it sent no game log, so not proven). The Windows build of this
  change under Wine, which has no such codec: New Game plays `ev0000` with its subtitles. `ev0000` decoded with the Windows DLLs:
  803 fps on 16 threads, 188 fps on one core (`--disable-autodetect`, so no GPU decoding; nasm is required, CI checks it).
- Two things kept them from playing: `nn::os::TryWaitEvent` had no handler (B3's player polls its decoder events with it:
  87,778 stub calls), and B3 allocates the Extractor / Decoder objects with its heap-aware operator new, so their deleting
  destructors must call the game's `operator delete` (free() refused the block: a guest trap at the end of the first movie).
- A 480 s run from the save (A every 3 s): `ev0000` (163 s, with the game's subtitles), `ev0005`, `ev0010`, `ev0011` play;
  the prologue goes on through in-engine scenes (the hexagon city, Bayonetta and Viola, an arena with lava, close-ups,
  "You have a job to do, soldier!"). The summoned-demon sequence of stage 3 no longer holds it (it was waiting for the
  opening movie). 18,149 frames, 5 M draws, 0 skipped.
- **Frame rate**: 60 fps in-engine; 30 fps while a movie plays (the movies' rate) and in one heavy scene (983 draws). Indirect
  records in a compute buffer cost a GPU wait only after new GPU writes (was ~150 waits a frame: 27-49 fps).
- **Vulkan validation**: not run at the time (the layer was installed later: see the next section).

## Vulkan validation (2026-10-24, layer 1.4.363, RADV on an RX 9070 XT)

`WT_VK_VALIDATE=1 VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`; the messages are counted per VUID and the sorted totals
printed at exit (`vk-validation summary:`). Raise the layer's cap of 10 per ID with
`VK_KHRONOS_VALIDATION_DUPLICATE_MESSAGE_LIMIT`. Each pass ran from the save through the title, the opening movie and the
prologue (300-420 s, 9,000-12,600 frames, A every 3 s):

| pass | settings | before | after |
|---|---|---|---|
| core | default | 1 leak at `vkDestroyDevice` (14 objects) + benign | benign only |
| sync | `VK_KHRONOS_VALIDATION_VALIDATE_SYNC=true` | `SYNC-HAZARD-WRITE-AFTER-WRITE` (vkCmdBlitImage twice into one image) | benign only (10,812 frames) |
| GPU-AV | `VK_KHRONOS_VALIDATION_GPUAV_ENABLE=true` | `SharedMemoryDataRace-*` in 2 compute programs | the same 2 (known, below) |

Fixed:
- The loading bar / first-run screen blitted over the presented frame (and the bar over its track) with no barrier
  between the transfer writes: `xfer_wait()` before each.
- Shutdown now frees the compute buffers' landing buffers, the read-back free list, the kept vertex buffers and the
  occlusion query pool (the 14 leaked objects).
- Translator: an 8 / 16-bit `STS` was a read-modify-write of the 32-bit word, so lanes storing neighbouring bytes in
  the same instruction lost all but one; now `atomicAnd` + `atomicOr`. `MX_VK_VERSION` 10 (all stages are translated
  again on the next run: one warm-up).
- Under validation, shader modules carry the name `program #<n> stage <s>` (GPU-AV prints it), and
  `nvn_vk: shader module 0x... = ...` is logged for each.

No message about the paths added in stages 3-4 (storage images, depth bounds, passes without attachments, indirect
draws read on the CPU, the movie's NV12 uploads).

Left, judged benign or known:
- `Undefined-Value-ShaderOutputNotConsumed-DynamicRendering` (warning, ~400,000 per run): a fragment stage writes a
  colour output and the pass has no attachment there (depth-only and target-less passes). Vulkan discards it.
- `SharedMemoryDataRace-RaceOnStore` in program #7836 (compute, 64 wide): every invocation stores 0 to `smem[0]` at
  entry, the same value from the same instruction. The game's own shader does it the same way; harmless.
- `SharedMemoryDataRace-RaceOnLoadStoreVsAtomic` in program #7839: init by invocation 0, `barrier()`, `atomicMin/Max`,
  `barrier()`, loads. Correct as written, but the translator puts the barriers inside its `for (;;) switch (pc)` dispatch
  loop, so invocations reach the same barrier in different loop iterations. Per SPIR-V that is not uniform control
  flow, so GPU-AV cannot pair the barriers. It works on RADV, where a 64-wide group is one wave. A structured control
  flow for stages with `BAR` would fix it (not done).
- `WARNING-Setting-Limit-Adjusted`: GPU-AV itself.

## Stage 5 (2026-10-24): the prologue goes on into chapter 1's cutscenes

- **The prologue stopped on a still of Viola** (about 640 s from the save, frame ~19,000): `ev0011` is opened and
  pre-rolled while `ev0010` plays, then taken in one burst when `ev0010` ends. B3's player [game 0x7100cd6cb4] keeps
  decoded frames in a pool of 16 nodes. A frame acquired while the pool is empty [0x7100cd7244] is neither queued nor
  released, so it is lost and its output buffer leaks. This decoder has no decoding time and offered 48 output buffers
  (SSBU's number), so the burst overran the pool, the picture stopped after 16 frames and the decoder had no free buffer
  left. **16 video output buffers** now (the codec stalls before the pool runs out). The earlier movies were losing frames
  the same way (300 frames per 18.7 s of movie time before, 300 per 10 s now).
- A 900 s run from the save (A every 3 s): `ev0000`, `ev0005`, `ev0010`, **`ev0011`, `ev0020`, `ev0021`, `ev0022`**
  play, through the in-engine prologue scenes and into chapter 1's New York opening (the streets, the pizza truck).
  31,336 frames, 5.4 M draws, 0 skipped, no stop.
- Linux: `libopus-0.dll` maps to `libopus.so.0` (`nn::codec`'s HardwareOpusDecoder reported a work buffer of 0 bytes;
  Wwise does its own Opus, so this was not the stall).
- Test aids: `kill -USR1 <pid>` (Linux) prints each thread's guest thread name, current guest function, LR and x29 frame
  chain (the hang above was found with it; ptrace is not allowed here). `WT_MOVIE_LOG` lines now name the decoder and
  the guest caller.
- `WT_SKIP_MOVIES=1` takes another path: the A presses reach the menu and open **Chapter Select, "Prologue: A Chaotic
  Encounter"**. After the (cut) movies `ev0020`..`ev0026` it holds one camera view of water and buildings at 563 draws a
  frame for over 4 minutes (not looked into; cutting movies short may itself be the cause).

### Gameplay (2026-10-24)

- **The first fights run.** Run `long1` (1,500 s, A every 3 s) took the Chapter Select route: Prologue "A Chaotic
  Encounter", movies `ev0020` .. `ev0026` at full length, then gameplay at ~760 s (frame ~23,000) with the combat HUD
  (Verse 1, the Punch / Kick / Shoot / Jump / Dodge prompts, combo points) against Homunculi and a giant Homunculus.
  With A alone Bayonetta keeps dying ("THE WITCH HUNTS ARE OVER", retry) in Verse 1.
- **Input beyond A works.** Run `fight1` (1,500 s, the same A presses plus an `WT_SCRIPT` of 1,716 steps from 700 s:
  `V`=Y, `C`=X, dodge `S`=ZR, jump `X`=B (the game's controller page shows X = punch, Y = shoot, A = kick; the labels
  first written here had them swapped), left stick right / left, punch+dodge): Verse 1 is cleared (1,525
  combo points x1.75 on one shot) and the fight goes on into **Verse 2** (another giant Homunculus, another arena).
  52,448 frames, 32 M draws, 0 skipped, no stop. `SCRIPT_MAX` is 4,096 steps now (was 256).
- Screenshots: `/tmp/shots/b3_first_fight_verse1.png`, `b3_verse1_start.png`, `b3_verse1_combo.png`, `b3_verse2.png`,
  `b3_verse2_fight.png`.

## Performance (2026-10-24): chapter-1 fights at 60 fps

**Benchmark**: the save as it is now (the user's chapter-1 progress) → Chapter Select → chapter 1 → Verse 1 on the grass
field (from about frame 19,400), A every 3 s plus the fight script of stage 5 from 560 s, 1,100 s, `WT_AUDIO_MUTE=1`,
`WT_VK_MISSING=warn` (the chapter's tessellated draws are skipped: blocker 3). Numbers over the 300-frame windows from
frame 19,800 to the end of the run (`nvn_vk:` / `nvn_perf:` / `nvn_vk_timeline:` lines). The runs use a copy of the save
(`/tmp/b3_userdata_<run>`): the game autosaves.

| run | build | fps avg (min) | 1 %-low fps | frame time | render thread a frame (batch start → last submission) |
|---|---|---|---|---|---|
| base1 | before | 44.5 (30.5) | 31.1 | 23.3 ms | - |
| base2 | before | 49.4 (35.7) | 32.5 | 20.6 ms | 15.1 ms |
| base3 | before (profiled 90 s) | 52.1 (43.1) | 35.0 | 19.3 ms | 13.1 ms |
| fix2 | flush index | **59.3 (50.6)** | **47.4** | **16.9 ms** | 12.4 ms |
| o2a | flush index, host -O2 | 59.4 (51.9) | 49.3 | 16.8 ms | 11.9 ms |

(Run `fix1`, the same build as `fix2`, ran 17-24 fps from 640 to 810 s with every thread slowed alike, sampler included, and 59.2 fps
from frame 22,800 to the end: taken as outside CPU load and left out.)

- **The cause**: `WT_LPROF` (below) found **83-96 % of the busy time of the game's MainDraw and Flush threads in
  `nvn_gl_memory_written`**: every `nvnMemoryPoolFlushMappedRange` / `nvnBufferFlushMappedRange` walked every texture ever
  made (the list is never unlinked) to mark the ones it overlaps; B3 flushes many ranges a frame. Now a span index of the
  live textures sorted by address (binary search, running maximum end), rebuilt when a texture is made or finalized;
  `WT_TEXSPAN_CHECK=1` compares every lookup with the walk (240 s, no difference). After it, MainDraw is 12 % busy.
- **Tried, not kept**: (1) the kept vertex buffers of finding 140 (`vrc_*`) for streams no compute buffer overlaps (they are
  off whenever any compute buffer is live, i.e. always in a B3 fight): the ring copies fell from 29 to 7 MB a frame, but the
  render thread per draw did not move (13.2 against 13.6 us: the write-watch questions cost what the copies saved), so the
  change was dropped. (2) The host sources at -O2 (`--host-opt=O2`): 13.8 against 13.6 us per draw, the same within the
  noise; the default stays -O1.
- **What is left on the render thread** (about 60 % busy in a fight): memcpy (46 %: compute buffers filled from guest
  memory at a dispatch, texture uploads, the copy-back of compute results), `cb_keep_back`, `draw_prepare`. The dips
  (min 50-52 fps windows) were not looked into one by one.
- **`WT_LPROF=<start s>,<seconds>[,<file>]`** (Linux, test only, `runtime/lprof.c`): every thread is sampled with a
  real-time signal (SDL3's threads block SIGPROF), PC + host call chain; `wtbuild lprof <file>` lists the busy
  threads (innermost frame not waiting in libc) with their self / inclusive functions (the symbol tables of
  witch-time-game, game.so: `f_<guest address>`). Lifted code has no unwind tables: its chains stop at the innermost lifted function.

## Tessellation (2026-10-24)

B3 has 29 tessellation programs, each a separate NVN program per stage (bound by stage mask): 29 control and 29
evaluation stages. All are the same kind: the GLSLC tessellation info of the control section (+0x738 primitive mode,
+0x73c spacing, +0x740 point mode, +0x744 vertex order, OpenGL enums) says GL_TRIANGLES, GL_EQUAL, no points, GL_CCW;
the control stages' program headers say 3 output vertices and 6 patch attributes (the levels).

- **Translator** (`mx_translate.c`, after suyu's recompiler): ALD / AST with the vertex register (bits 39..46) on input
  arrays, the control stage's outputs at `[gl_InvocationID]` (ALD.O reads them back; in the evaluation stage .O reads its
  inputs), patch attributes 0..3 / 4..5 = outer / inner levels, `gl_TessCoord` (0x2f0 / 0x2f4), invocation info =
  `gl_PatchVerticesIn << 16`, `ISBERD` (a move), `VMAD`, `VSETP`; `layout(vertices = N)` and `layout(triangles,
  equal_spacing, ccw)`; the built-in blocks redeclared to match the neighbours. Generic patch attributes fail (none used).
  `tools/mxcheck`: **7,896 / 7,896 stages** translate and compile (the 58 tessellation stages included); the other 7,838 give
  byte-identical text (no `MX_VK_VERSION` change; the tessellation stages have a hash salt of their own).
- **Vulkan**: NVN primitive 14 with both stages: a whole pipeline (no libraries) per key + stages + patch size, made at
  the first draw (`tess_pipe_get`, not in pipes.bin or the workers), `PATCH_LIST`, `patchControlPoints` = the NVN patch
  size (`OP_PATCHSIZE`, ignored before), domain origin `LOWER_LEFT` (the viewport is flipped the OpenGL way);
  `draw_prepare` binds samplers, images, buffers and descriptors for the stages {0, 1, 3, 4}; forced outputs chain
  fragment <- evaluation <- control <- vertex. The glslang shim takes stages 3 and 4.
- **Runs** (chapter 1 by Chapter Select, the fight script, muted, **without `WT_VK_MISSING=warn`**): 1,100 s, 46,666
  frames, **0 skipped draws** (the earlier runs with `warn` skipped 4,552-5,585), no stop. One tessellated pipeline (programs
  #3945 / #3950 / #3951 / #3946, patch size 3), about 2,700 tessellated draws, one a frame in runs from frame ~17,600.
  The fight: 59.3 fps (min window 55.5, 1 %-low 48.7), as before.
- **Validation** (core, and core + sync, over the same route): only the known
  `Undefined-Value-ShaderOutputNotConsumed-DynamicRendering` warning; the first run found
  `VUID-RuntimeSpirv-OpVariable-08746` (built-in block mismatch vertex -> control), fixed.
- Test aids: `nvn_vk: tessellated pipeline <n> at frame ...` and `tessellated draws in frames a..b: n` lines, the totals at
  exit; `WT_VK_TESS_ALT=1` leaves the tessellated draws out on odd frames, `=2` draws them magenta there (with
  `WT_SHOT_BURST=<period>,2` for pairs of pictures).

## Magenta textures: invalid ASTC blocks (2026-10-24)

The user's hand run showed bright magenta on clothes (Luka's shirt), Bayonetta's guns, heels and accessories, the purple
light pillar and the Verse results' ink splashes. The GPU here (RADV, RX 9070 XT) has no ASTC, so `tex_decode.c` decodes B3's
ASTC textures on the CPU. It gave every **invalid** block the specification's error colour, opaque magenta. B3's chapter 1
has about 105,000 of them (`WT_ASTC_LOG=1`): **all-zero blocks** (a reserved block mode), in ASTC 6x6 textures that
are bound in many places (the same counts, image after image: 7 % of the 1024x1024 level, whole 32x32 and 2x2 levels).
Tegra returns transparent black for them (yuzu PR #8910 made its decoders do the same), so nothing shows on the console.

- **Invalid blocks are transparent black now.** Same route and scene, before / after: `/tmp/shots/astc_before_27000.png`
  (Verse 1 results: magenta ink, shirt, guns), `/tmp/shots/astc_after_25500.png` (results: white / black ink),
  `/tmp/shots/astc_after_chars.png` (Bayonetta close and in the fight: gold trim, no magenta). Chapter 1, 1,100 s without
  `WT_VK_MISSING=warn`: no stop, 0 skipped draws, fight 59.7 fps (min window 56.5).
- Found on the way: **HDR colour endpoint modes** (2, 3, 7, 11, 14, 15) also became magenta; Tegra decodes them in these
  formats (full profile). Now decoded (LNS -> FP16, clamped into the 8-bit images, per partition and channel). B3's
  chapter 1 has none (the counter says 0), so this did not cause what the user saw.
- `tools/astccmp.cpp` compares the decoder with ARM's astc-encoder (fetched separately) on random blocks of all 14
  footprints: 72,524 valid blocks (22,840 with HDR endpoints), none more than 1 apart (the decoder before: 22,828 apart).
  It skips blocks either side rejects, so it does not test what an invalid block becomes.
- The exit log has `tex_decode: ASTC blocks decoded on the CPU with HDR endpoints: N; invalid (transparent black): M`;
  `WT_ASTC_LOG=1` (test) names each image with invalid blocks (format, size, count, the first non-zero one).
- **Correction (2026-10-25):** most of those all-zero blocks were not in the game's data; they came from the layout bug
  below (the padding GOBs of sheared textures). Transparent black stays the right answer for the real ones.

## Block width of packaged textures: medal-screen flicker, gun texture (2026-10-25)

The user's hand run of ded5dc9 showed the fire/ink panel of the Verse medal screen flickering, and Bayonetta's guns with
blue bands and patchy gold. Both came from one bug: **byte 1 of the packaged texture layout is the block width** (log2
GOBs) and `tex_layout` ignored it. B3's ASTC 6x6 effect atlases and Bayonetta's gun texture have byte 1 = 4 (layout
`03 04 00 00 07 00 01 00`): a block of 16 x 8 GOBs, one 64 KB tile.

- **The rule, read off the game's memory** (`WT_TEX_RAWDUMP`): a level at least one block wide has its rows padded to a
  multiple of the block width; a narrower level is not padded at all. 1024x1024 ASTC 6x6: rows of 43 -> 48 GOBs, 22 -> 32,
  then 11, 6, 3, ... as they are; levels at 0, 589,824, 851,968, 897,024, 909,312, ... (every level of the dumps now
  decodes with 0 invalid blocks). The layer stride counts the block width too. `tools/astccmp` checks these offsets on
  every run.
- **Flicker**, same scripted route, the medal screen (214 frames, right half of the picture, `/tmp/re/gaptest.py`): with
  byte 1 ignored, 205 frames change and change back (A-B-A); now 14. The change over 1 frame fell from 7.7 to 3.7 (over 8
  frames: 10.7 and 9.4), so what moves now is the ink's animation. Pictures: `/tmp/shots/flicker_before_after.png` (six
  frames each, top before), `/tmp/shots/flick5_seq.png`.
- **Invalid ASTC blocks** in a 400 s run: 92,237 with byte 1 ignored, 2,083 now. Those are all in one building normal map
  (ASTC 10x10, 1024x1024, plain layout) whose unused areas are all-zero blocks; astc-encoder rejects them too.
- **Guns**: their colour texture is ASTC 6x6 sRGB 1024x1024 with byte 1 = 4. Before: sheared bands, 2,183 invalid
  blocks; now clean: `/tmp/shots/gun_albedo_old_vs_new.png`. The texture itself is a saturated purple body with gold
  filigree and a gold-ringed disc, so the guns stay purple and gold. In the game (Tokyo boss cutscene):
  `/tmp/shots/guns2_9936_crop.png`, solid purple and gold, no bands. Not reproduced here: the user's Verse 1 close-up
  (another save point); a look by hand there is the check left.
- **The decoder on real data**: all 3,010 ASTC images of a 380 s run (`WT_ASTC_DUMP`, 11 formats, 4x4 to 10x10, linear
  and sRGB) compared block by block with astc-encoder (`astccmp --file`): none more than 1 apart, and every block this
  decoder rejects the reference rejects too.
- **Speed**: the same scripted 700 s run on this save (it ends in the heavy Tokyo scenes, not the chapter-1 fight) from
  frame 19800: 44.1 fps before, 35.0 and 42.8 in two runs after. The difference is a few windows with stalls of about
  160 ms (p99), at different frames in each run and with no texture uploads in them; the median frame time is the
  same. The chapter-1 fight (59.7 fps) needs the older save and was not run again.

## Compute buffers: a full table (2026-10-25)

The user's later save (Tokyo chapter) stopped with "more than 256 storage buffers written by compute in one batch".
A full table now lands its least recently used kept buffer and reuses the slot (72 times in 700 s there), and the table
holds 1,024. The heavy Tokyo scenes ran at 35-44 fps then; see the next section.

## Performance in Tokyo: Shibuya and the heavy Tokyo scenes (2026-10-25)

**Benchmarks** (`WT_AUDIO_MUTE=1`, each run on a copy of a save; per-frame times from `WT_FRAMETIMES`, the
statistics over the frames the game paces at 60 = present interval 1):
- *Shibuya*: the user's save (Continue → Tokyo: Shibuya street fight), the stage-5 fight pattern from 40 s to 330 s,
  frames 1500 to the end.
- *Tokyo (old route)*: a copy of the earlier save (`bench_bw`'s), the stage-5 script from 560 s, 700 s, frames 19800 to
  the end: the heavy scenes measured at 35-44 fps before.

| | avg fps | 1 %-low | p50 / p90 / p99 ms | frames over 20 / 50 / 100 ms | frames at 60 |
|---|---|---|---|---|---|
| Shibuya, before | 57.4 | 38.8 | 16.8 / 20.6 / 25.8 | 1814 / 12 / 1 | 82.5 % |
| Shibuya, after | **59.9** | **51.7** | 16.7 / 17.6 / 19.3 | 146 / 4 / 0 | **98.0 %** |
| Tokyo, the changes switched off | 51.8 | 40.2 | 18.5 / 21.5 / 24.8 | 3667 / 64 / 59 | 49.8 % |
| Tokyo, after (without the bit split) | 59.7 | 53.9 | 16.7 / 17.6 / 18.6 | 29 / 13 / 8 | 98.8 % |
| Tokyo, after (all) | **59.5** | **52.9** | 16.7 / 17.6 / 18.9 | 98 / 15 / **0** | **98.4 %** |

("Switched off": `WT_VK_VRC=0`, `WT_TEXDEC_THREADS=0` and an always-sync test switch that was not committed; the live
list stays. Runs of the same build differ by about ±1 % at 60 and in where the trains of long frames come; another
Tokyo run of the last build: 59.8 fps, 98.5 % at 60, one frame over 100 ms (104, below).)

What it was, in order of the commits:
- **The game's 30 fps is the game's.** Its main loop picks a frame-rate mode each frame (`7100670b80` → `7100d02de0`,
  applied at `7100cfd9b8` from the table 1 / 2 / 4) from its own state flags: interval 2 on the title, in menus and in
  cutscenes (flags `50000208`, `50000008`). The user's 30.0 fps stretch (frames 5100-11500 of their log) is such a span.
  Not changed; letting those run at 60 would change the game (its logic steps by the mode).
- **Kept vertex buffers while compute is live** (`vrc_cb_touch`): every vertex stream went through the stream ring because
  B3 dispatches compute every frame: 64 MB copied a frame, the render thread's largest cost. Now 5.6 MB. (The
  Performance section above dropped the same idea in chapter 1, where the write-watch questions cost what the copies
  saved; in Tokyo the copies are three times larger and it pays.)
- **ASTC decode on worker threads** (`decode_par`): the 45-170 ms stalls of Shibuya were new 1024x1024 ASTC textures
  decoded on the render thread (15-20 ms each, several at once when the game streams an area in). A 1024x1024 texture now
  takes 5.3 ms. `WT_TEXDEC_CHECK=1` compared 1,301 parallel jobs with one thread's decode: no difference.
- **Live compute buffers in a list** for `buf_ref`'s searches (the walk over the table was 17 % of the render thread).
- **Buffer copy / update / clear wait for compute only when they overlap it** (`cb_sync_needed`): each of these CPU-side
  commands waited for the GPU and landed every live compute buffer; B3 issues a clear and four 32-byte copies a frame, so
  every frame paid a GPU wait and 4-24 MB of copies. The render thread in Shibuya: 10.4 → 3.9 ms a frame.
- **The 150-170 ms trains in Tokyo** (the "160 ms stalls"; 59 frames over 100 ms in a 700 s run, about every 6 frames
  while a train lasts): the main thread in the game's `f_7100b382c0`, called from a deep recursion of `f_71007e1fe4` (a
  tree walk): it splits a 32-bit value into its even and odd bits with three bit reversals and a 32-step loop, 171.6 ns a
  call lifted. New: `lifter/recomp_eh.py` `HOST_OVERRIDES` sends every direct call of a chosen game function to a host
  function (`runtime/game_shim_eh.c`); `h_morton_split32` takes 1.7 ns. `WT_HOSTFN_CHECK=1` (test): 200,000 values
  through both, 0 differ.
Screenshots of every run (`/tmp/shots/<run>/`) compared: no visual difference (the band of purple glyphs at the clouds of
erasure is the game's: it is also there with `WT_VK_VRC=0`).

**Left** (Tokyo, 700 s runs of the last build):
- Trains of 45-50 ms frames (7-25 frames a run): the same tree walk, `f_71007e1fe4` itself now (about 35 ms a frame), a
  large lifted function with float math. Not changed: the register cache (`RECOMPS_REGCACHE`, off since finding 91) or a
  host version of the walk would be the next step.
- One frame of 80-105 ms every 37.3 s (2,236 frames), each the first frame of a run of tessellated draws (the
  tessellated scene coming back into view); not the pipelines (three, made early). The game's main and job threads do
  more work in that frame (buffers made, `nvnBufferInitialize` under a game mutex, lifted code); not attributed further:
  the sampler gets about 25 samples a thread over three such frames. A table lock split 64 ways (`nvn_stub.c nobj`) was
  tried: no measurable change, not kept.
- Tools added on the way: `WT_FRAMETIMES`, `WT_HITCH` with a 19 ms threshold for late frames, `WT_LPROF` with 4M
  samples and the presented frame of each sample (`@<frame>`; `wtbuild lprof` reads both forms).

## Chapter 3 spider ride: render thread (2026-10-30, #45)

Same stretch of the ride, about 2,300 draws: replay 30.5 -> 16.3 ms a frame.
- **Page filter in `buf_ref`** (`cb_pf_*`): 20,000 ranges a frame each walked the live and about 170 kept compute buffers, 16
  ms a frame. A hashed bit per page of those buffers answers 98.6 % of the ranges without a walk. `WT_VK_CBPF=0` off,
  `=2` test: the walks run as well and a miss prints `nvn_vk_cbpf: FAILED`; none in 17 M ranges on the ride and 63.7 M in Shibuya.
- **ASTC decoded ahead of the first draw** (`nvn_front_tex_prep`, `WT_TEXPREP_THREADS`, default 3): at the draw, decodes
  took 5.5 s of the render thread in 100 s of the ride, while 94 % of those textures had been known for 30 frames or more.
  Now 0.73 s; `tex_ahead` makes the image when the decode is done, in fights too. A job keeps a copy of the bytes it
  decoded and its result is used only if the memory still equals them. `WT_TEXPREP_CHECK=1` (test): 3,110 textures,
  no level differs from a decode at the use. Shibuya 170 s from frame 1500: frames over 34 ms 11 -> 0, worst 59 -> 22 ms.
- ASTC stays in the game data and BC3 is what GPUs without ASTC store by default. Build-time conversion was first turned down
  here (about 41 GB, a count that repeats every shared texture); it is now an option, see "Converted textures" below.

- **The tree walk `f_71007e1fe4`** (the steady 46 / 90 ms frames here, the 45-50 ms trains in Tokyo): a quadtree over two
  1024x1024 byte maps. A leaf is 1 where they differ, and a node draws a quad for each child that returned 1 unless all four
  did. A node whose leaves are all equal therefore draws nothing and writes only its stack, so `h_quad_walk`
  (`quad_walk.h`) reads its result off the maps. A walk: 47 -> about 0.01 ms; mixed nodes stay lifted, so the quads are the
  game's. Ride from frame 2400: frames over 60 ms 4 -> 0, worst 105 -> 45 ms. `WT_QUADWALK=0` off;
  `WT_QUADWALK_CHECK=1` (test) runs the lifted walk too: on the ride 239 uniform nodes in 10 walks, none differ and none drew a quad.

**Left**: about 74 textures at the ride's start are first seen in the frame that
draws them (one frame of about 0.6 s). `WT_LPROF` frames are 2 ahead of `WT_FRAMETIMES` frames.

## Converted textures: BC7 sidecar (2026-10-31, #54, #35)

`bc7_textures = on` (the default; `WT_BC7_TEXTURES=0|1` wins) takes ASTC textures from `build/cache/bc7`, which `wtbuild
textures` makes from the player's own dump. The launcher runs it after a build, unless the option is off or the drive has
under 19.5 GB free; without a pack the game decodes as before. It began as an opt-in; the default changed the same day. Everything below was measured on a Ryzen 7 7800X3D and an RX 9070 XT
(RADV, no ASTC): 4 cores with `taskset` (cpus 0-3, with 8-11 for their threads), never on a Steam Deck.

**Census** (all 72 archives; a raw scan for texture headers found nothing the walk missed): 44,456 records, 24,822 different
textures, 16,828 of them ASTC (4.89 GiB). The 41 GB was every record counted: 42.6 GB as BC3. Keyed by content it is 20.5 GB.

| block | textures | ASTC MiB | as 8 bpp MiB |
|---|---|---:|---:|
| 10x10 linear / sRGB | 4,970 / 3,039 | 1,237 / 980 | 5,995 / 4,775 |
| 8x8 linear / sRGB | 3,092 / 4,877 | 997 / 983 | 3,965 / 3,892 |
| 4x4 linear / sRGB | 316 / 458 | 307 / 389 | 306 / 388 |
| 6x6, 5x5, 12x12 | 76 | 119 | 187 |

Not ASTC: 7,994 textures, 3.25 GiB, stored as they are (BC3, BC6H, BC1, RGBA8).

**The formats.** 420 textures by block size, alpha and kind (normal maps, colour with alpha), weighted back to the whole set;
"worst 1 %" is the mean over textures of their worst 1 % of blocks. Against the ASTC decode:

| encoder | size, whole set | LZ4 / zstd of that | PSNR | worst 1 % | Mtexels/s per thread | whole set, 16 threads |
|---|---:|---:|---:|---:|---:|---:|
| runtime BC3 | 20.5 GB | 0.46 / 0.35 | 38.3 | 29.3 | 106 | about 2.5 min |
| ISPC BC3 | 20.5 GB | 0.47 / 0.35 | 39.3 | 30.7 | 330 | about 2.5 min |
| ISPC BC1 opaque + BC3 alpha | 13.1 GB | 0.73 / 0.57 on BC1 | 39.3 | 30.4 | 407 | about 2.5 min |
| ISPC BC7 very fast | 20.5 GB | 0.77 / 0.65 | 50.2 | 41.0 | 3.2 | 6.9 min (measured) |
| ISPC BC7 basic | 20.5 GB | 0.78 / 0.66 | 51.9 | 42.4 | 0.80 | about 20 min |
| ISPC BC7 slow | 20.5 GB | 0.78 / 0.67 | 52.2 | 42.7 | 0.22 | about 100 min |

Only the BC7 very fast time is measured; the others are its 2 minutes of unpacking and decoding plus the rate above. BC4 and
BC5 save nothing that counts: textures BC4 could hold add 0.01 GB to the BC1 policy's saving, and BC5 is the same 8 bpp as BC7.

**Pick: BC7, very fast, LZ4.** The same memory as today's BC3, 12 dB better, and the worst blocks 11.7 dB better; the next
setting up gains 1.5 dB for 4 times the wait. BC3 would save 6 GB of disk but gives nothing to look at. ASTC stays the source: nothing of the game's files changes. zstd would save another 2.5 GB and needs a
library in the runtime; LZ4's decoder is 25 lines. `texture-compressor` (pure Rust) was rejected on these textures: its Balanced
preset writes blocks of 4.5 dB, its Fast preset lost 34 dB on one of the 420, checked against a second decoder.

**The pack.** 16,828 textures, 15.7 GB (20.5 raw), plus 3.2 GB while it runs (`ba`, the largest family). 412 s on 16 threads,
713 s on 4 cores with 8 threads, 5,260 s of CPU; about 25 minutes on a Deck is an estimate from its clock. Those are all threads; `textures` now defaults to half of them (`ba`: 93 -> 120 s on 16 threads, 168 -> 222 s on 4 cores, so about 16 minutes there and 33 on a Deck). Every level of every
texture read back from the pack: pooled BC7 45.9 dB, runtime BC3 36.5 dB; top levels 50.1 and 39.4 dB. 227 of about 170,000 levels
are under 30 dB, all of them better than BC3 there.

**At run time**, the 10,000 frames of the chapter 3 ride, cold pack (page cache dropped):

| | render thread at a texture's use | workers | worst frame | frames over 100 ms |
|---|---:|---:|---:|---:|
| 8 threads on 4 cores, decode / pack | 1,435 / 336 ms | 25.7 / 1.9 s | 833 / 199 ms | 1.64 / 0.33 s |
| 4 threads on 4 cores, decode / pack | 2,332-2,532 / 338-507 ms | 25 / 1.7-2.2 s | 899-977 / 130-244 ms | 1.6-1.9 / 0.3 s |
| 16 threads, decode / pack | 1,084 / 346 ms | 24.9 / 2.8 s | 633 / 167 ms | 0.94 / 0.29 s |

A job: 8.2 ms of decode and encode, 0.6 ms of hash, read and LZ4. Shibuya: workers 10.0 -> 0.9 s, at the use 1.8 -> 0.2 s, worst
188 -> 119 ms. Sustained fps does not change (52-55 here): this removes stalls, it does not raise a frame rate a GPU holds down.

**#35.** The three workers do not hurt on 4 cores, they help: `WT_TEXPREP_THREADS=0` against 3 on 4 threads is 9.0 -> 2.5 s of
the render thread, worst frame 2.6 -> 1.0 s. Fps stays 52-54 either way, so they do not explain a Deck topping out near 38 fps,
though this CPU runs at about twice the Deck's clock and its GPU is another class. No log from the reporter yet.

**How it works.** The key is a hash of the texture's bytes (`tex_pack.c`), which a worker takes in place of decoding, so the
check that memory has not changed is the old one. Every texture the ride (3,300) and Shibuya (1,720) made was found but the prompt atlas
`prompts.c` paints, which goes the old way: BC3. A hit stores BC7 in the image; if the game later changes that memory,
`tex_encode_bc7` (mode 6) fills the same format. `WT_TEXPACK_REDO=1` forces that on half the textures: 793 in Shibuya, the
picture holds. `WT_TEXPACK_CHECK=1`: 48 of the ride's levels under 30 dB, worst 20.7 dB, on tiny mips; `WT_TEXPREP_CHECK=1`
still 0 differ. Mods: a texture a mod changes has other bytes, is not found, and is decoded as before; nothing is corrupted.
A GPU with ASTC never looks at the pack.

The launcher's Remove, Continue, the chain after a build, and "off stays off" were run for real (a headless KWin, a stub for
`setup`); so was a `graphics.ini` without the key. The branch for a drive without the room was not.

#46: the mod's 4,442 paths each name an entry of the archive of that name, so "0 replace the game's" is expected: originals live
in the `.pkz`. With all those paths in place, as unchanged copies of the game's files, the game opened none of them in a Shibuya
run. That run's picture was broken, smaller parts of the tree were not; seen once, not repeated, not understood.

**Left**: no measurement on a Deck; Windows: `wtbuild.exe` cross-built and its tests run under Wine, not on Windows, and the MSVC
`cargo test` of release.yml has not run. The launcher does not know the GPU, so one with ASTC (most Intel) gets a pack it never
reads: Remove gives the space back. A stopped conversion redoes its family of archives. A mod's textures are not converted.

## 60 fps menus and cutscenes (2026-10-25)

`cutscene_fps = 60` (graphics.ini; `WT_CUTSCENE_FPS=60` wins; default 30, as on the console) runs the title, menus and
cutscenes in the game's 60 fps mode.
- **How the game times them**: the main loop (`7100670b80`) asks each frame for a frame-rate mode from its state flags
  (0 = 60, 1 = 30, 2 = 15; set by `7100d02de0`, applied at `7100cfd9b8`). Its per-frame clock `7100c1f600` keeps the
  frame's step at clock+0xe0: in gameplay a ratio of the measured frame time, in mode-1 scenes (flags 28 and 30) a fixed
  1.0, and each frame of such a scene runs two logic steps (see "One logic step" below), 60 a second at 30 fps. Forcing only
  the mode played the same cutscene in 46.6 s instead of 92.1 s.
- **What is done**: two host overrides (`lifter/recomp_eh.py` `HOST_OVERRIDES`, `runtime/game_shim_eh.c`):
  `h_fps_mode_set` makes a 1 a 0, and `h_frame_clock` replaces the fixed step while such a scene runs at 60 by the last
  frame's measured length (the clock's own, +0xf0) in 30 Hz frames, 0.5 on time, more for a late frame, so the
  scene keeps real time when frames are late (at most 1.0 at first; see "Late frames" below; a measured gameplay
  step is left alone). (A fixed 0.5, the first version: 97.9 s for the cutscene at 56 fps in 4K.) The game's frames-to-seconds conversion (`7100cb4f54`) reads the mode it now has. Each change of the
  mode the game asks for is logged with the wall-clock time ("fps mode:" lines).
- **Measured** (Shibuya save, the cutscene that starts after 75 s on the street; `WT_FRAMETIMES` has the wall clock
  per frame): 30: 2,758 frames in 92.07 s; 60: 5,496 frames in 92.33 s, 59.5 fps, p99 22.0 ms (fixed half step); with the
  measured step 91.94 s at 59.7 fps, and 92.03 s at 56.4 fps in 4K (resolution scale 2.4). Screenshots at equal
  wall-clock times after the start (+1 .. +20 s, `/tmp/shots/cutscene_30_vs_60.png`): the same shots, poses and subtitle
  lines. Consecutive frames at 60 all differ (no repeated 30 Hz poses). Title screen and the main menu's entry at equal
  times: the same (`/tmp/shots/title_30_vs_60.png`). The pause menu is mode 0 anyway (not changed). The prologue movie
  (`ev0000.webm`, VP9, 30 fps): movie time against wall clock, 160.0 s -> 160.11 s at 30 and 160.09 s at 60, all 4,900
  frames: movies keep their rate (the game's player times them by the system clock).
- **Gameplay the game asks 30 for (2026-10-30, #45)**: the chapter 3 spider ride is mode 1 from state bit 16 (flags
  `0x00810000`), bits 28 and 30 clear, so its step is measured; cutscenes are `0x50000008`. `cutscene_fps = 60` and
  frame_rate above 60 forced it to 60 too, and its frame counters ran at twice the console's rate (the reported third
  jump prompt of under a second, not reached by a test: a blind script dies on the ride). Only fixed-step scenes are
  forced now; the ride runs at 30 as on the console.
  The ride's scripted parts do have the fixed step (`0x50810008`), so they were still forced: the ride jumped between 30
  and 60 and the third prompt stayed short. Flag bit 16 kept them at 30 at first; the user passed every prompt by hand.
- **One logic step (2026-10-31, #45)**: the prompt's cause. The main loop (`7100670da4`) runs a second logic step (pads,
  `7100671600`, no drawing) in a frame of a fixed-step scene, so a 30 fps scene has 60 steps of 1/60 s a second. Forcing 60
  kept both: 120 steps a second of half the size. Anything scaled by the step kept real time, but whatever counts steps ran
  twice as fast, in cutscenes and menus too. That is why no census found one counter. So above 30 fps such a frame takes one
  step of twice the size: `h_main_pass` (the override of `7100b20a20`, called from `7100670d88` just before the loop tests
  bit 28 of w21) clears the bit, and `h_frame_clock` doubles the step: one step of 1.0 a frame at 60, as gameplay.
  Flag 21 still keeps the second step: it repeats it until the scene player reaches a frame it seeks (`7100bb2350` clears the
  flag). No log has shown that flag yet, so this path has not been run. The third jump is probably in the scripted part
  `0x50810008` at updates 7654-7776 of the hand run: three motions of about 0.9 s, with a slow motion of 0.05 at the third
  (read off the census windows, not seen on the screen).
  - **A movie scene's subtitles (#72)**: the reporter's video has `ev0011` alone at 30 ticks a second against its 60 fps cues:
    the delay grows from 20 s to 40 s and the last line never shows. The picture and the voices keep real time. In a movie
    scene `7100bbccb8` sets the frame to the movie's frame [`7100cdaef0` +16] times the period, `max(scene +32,` `7100707840)`,
    and that is 2 only with flags 28 and 30. Clearing 28 by hand at the start of `ev0000`: 30.0 ticks a second for the 73 s
    measured, and the game asks for mode 0. `71007077d0` writes the flag, from a scene's start and its end. Not reproduced
    on Linux: 60.0 ticks a second at v0.4.5 and `bd2923e`, cutscene_fps 60 and 30, New Game and the reporter's save through
    Chapter Select; at the handover the old scene's end runs first. So `keep_movie_flag` sets the flag again and logs a line.
    Why the reporter's PC lost it is not known, nor that it did: v0.4.5's Media Foundation path could not be run here.
  - **The two kinds of step (#65, #67, #69)**: the two steps of a console frame are not the same. Bit 27 of the flags is set
    at `7100670d0c` before the first and cleared at `7100670d90` after it. In the first the timeline update `7100bbcb94`
    recomputes its pause bits 5 and 31; only there does a seek start. In the second it latches what was asked for: skip
    (bit 10), branch (0), chain (15), ProgressAct (19), bits 8, 25 and 28 (`71007078a0`, callers `7100bbc3b4` and
    `7100bbcc00`; `7100707860`, caller `7100bbcc0c`). v0.4.5 dropped the second step, so nothing latched: eva142's
    ProgressAct at cut 6 frame 294 was lost and the ride ran into cut 7, its death cut, and in chapter 1's ev1000 chain the
    second scene's Skip stuck (tf `00400400`) and it played on behind black. Both at 60 and 120, neither at cutscene_fps 30.
    A scene skipped from its pause menu needed the same latch, so its frames keep one step now. Skipped there, the ride's
    first scripted part still ends at once: `00c10000` three frames after the menu at 60 and 120.
    Now `h_step_kind` (the override of `710067037c`, called at `7100670d18` just after bit 27 is set) clears the bit in
    every other frame, so the kinds take turns one a frame. On time the logic steps are the console's. The frames'
    once-a-frame jobs (`7100671520`, `71009567f0`, `7100b20c20`) and the fades' lengths stay as they were (read from the
    code). Forcing the latch on every step (a test build) passed too, but latches a request made in a second step one step
    early. The parity resets with flag 21, as the console's next frame starts with the first kind. The menu's frames are no
    exception now: their steps are the console's too. In v0.4.5 they ran 1.0 against 0.5 due, so the steps after the menu
    were 0.0, 0.125, 0.23. A menu frame owes nothing, and a pause drops what was owed.
  - **The ride's steps are exact (#67, #69)**: eva142's prompt code (coroutine `7100867380`, evaluator `71007acb24`,
    sequencer `7100bb87dc`) works only when each logic step advances exactly 1.0. Above it a window's end call is jumped
    over, +580/+588 stay set and the coroutine hangs before the parking lot (50 fps, step 1.203). Below it a model jumps to
    cut 7 (one run at 0.9 passed). ProgressAct and condition events exist only in eva142, among all 217 ControlSeq files. So with flag 16 the step
    is the game's in every mode (`scene_even`, `scene_step.h`), and a frame that owes a whole even step runs both logic
    steps of a console frame; pairs start only where the turns allow one. Elsewhere late frames keep `scene_step`.
    Flag 16 is not only eva142: 17 sites set it, the Gomorrah and Malphas rides among them. Fixed-step frames of those
    (not measured) take this step too, so a machine below 30 fps lags there instead of keeping real time.
    Above 30 fps debt from a stall on the ride is paid, not forgiven: an unpressed prompt is 245 steps, 4.08 s at 60 fps, as
    the evaluator slows the event to 0.05, and pairs can at most halve that in real time (a model: 3.85 s after 236 ms, the
    worst of #67's log; 3.08 s after 1 s). Forgiving it would leave the ride behind the sound for good. At cutscene_fps 30 a
    frame already runs both steps, so nothing can pay it and the next plain frame drops it, as on the console.
  - **Rates**: cutscene_fps = 60 runs the whole ride at 60, the plain frames (`0x00810000`, measured) too. Above 60 the
    ride is not forced: mode 1 is 2 ticks of frame_rate, 60 fps at 120 and 45 at 90, where pair frames make up the 60 steps
    a second. A counter named in `fps_counters.txt` would not let the ride run at frame_rate: the steps have to be 1.0.
  - **Measured** (the bot through the crane event, 24.5 s, a build with test logging): 60 three times, cutscene_fps 30,
    frame_rate 120 and 90, 50 fps twice, 41 and 25 fps, a 300 ms and a 1 s stall, a pause and resume four times. All end on
    ProgressAct at cut 6 frame 294, no cut 7, and the steps take turns with no break. Every step with flag 16 is 1.0 but
    the first at 120 and 90, before any prompt. 60 steps a second down to 41 fps; at 25 fps 49.9, as a frame runs at most two.
    A missed prompt still goes to cut 7, twice. The ev1000 chain at 60: all three scenes skip in 6 frames from their menu's Skip
    and the next scene starts. A normal build, one run of each at 60, by its pictures: the parking lot, and the scene after
    the chain. A late press at 60 and random frames of 0-30 ms more (622 pairs, over the prompts) pass too. Not run: the
    chain at 30 and 120, other single scenes' skips, the held fader, real time off the ride, a seek, a New Game, the title loop.
  - **Measured** (v0.4.5; scripted ride with bot, 170 s at 60): 58-60 fps from the ride's start on, 0 Vulkan failures. The Shibuya
    cutscene: 91.94 s at 60 (91.95 s before). The title screen's loop until the game goes on: 40.00 s at 60, 39.96 s at 120,
    40.03 s at 30, 39.95 s before (it shows a random character, so its pictures differ from boot to boot). Logic steps
    (`f_7100671600`) against frames under gdb over 40 s from the start: 1,978 / 1,978 at 60, 4,300 / 4,301 at 120, 1,981 /
    1,068 at 30, so 60 has the steps of 30. The 30 run has 1.85 a frame because the frames before the title have one.
  - **Left**: the ride's plain frames run 60 steps a second at 60, 30 on the console. Counters there that count steps
    run twice as fast, as in any gameplay at 60: in the hand run only `71007a01d4` is long (400 steps, HUD-like code, unread).
    Frames -> seconds `7100cb4f54` (107 callers of `7100cb4f20`) reads mode 0 in a forced scene: unread, as before.
- Not checked by ear: audio is muted in the test runs; the cutscene's timeline keeps wall-clock time, which is what
  the voice and lip sync follow.
- **Late frames (2026-10-30, #45, #47)**: the sound runs on real time and never waits, but a frame over 33 ms lost its
  excess for good at 60, and at 30 every late frame did (a fixed step). A reporter's log has 0.5-2 s of excess per 1-3 minute mode-1 scene, an
  upper guess for cutscenes: it has no flags, and some of those scenes may be gameplay (below), which is measured. `h_frame_clock` now gives every fixed-step
  scene but the ride (see "One logic step"), at 30 too, the measured step and keeps what a stall owes, paid at up to 2.0 a frame (at most 90 30 Hz frames
  owed; reset on entering a scene). The Shibuya cutscene with a 300 ms stall every 120 fixed frames (test build only):
  at 60 104.64 s -> 91.83 s, at 30 98.58 s -> 92.33 s; at 60 without stalls 91.74 s. Muted; steps above 1.0 not looked
  at by eye; the frame counters still run per frame (2,758 at 30 before, 2,588 now with the stalls).
- **First-run freezes in cutscenes (2026-10-30, #54)**: preparation made only key 0 of each vertex stage, and skinned draws
  ask for key 4 (attribute 2 as integers), so that variant was compiled at its first draw and `pipe_get` waited for it
  with no limit. Shibuya cutscene, empty Vulkan and Mesa caches: one frame of 8.4 / 9.4 s (vertex `c1ea34cfa9121fda`,
  fragment `23e8b1d266b98373`, depth only), 3 s paid back, 97.2 / 98.2 s instead of 91.75 s. Key 0 holds 9 ms, key 4
  8,243 ms. `job_prep` now also makes key 4 for the 657 vertex stages with `dep & 4`: no such frame, 91.78 / 91.77 s,
  holds 0.22 / 0.17 s (8.8 / 9.8 s before), preparation 66-72 s (56 s before), 7 % more cache, a normal restart 91.76 s.
  The marker is `prepared_v13_int2`: existing players prepare once more. RX 9070 XT, RADV, muted, one cutscene. A stall of about
  2.3 s once a run, outside the cutscene, is not a pipeline wait and not explained yet.
- **What the sound does, measured (2026-10-30, #47)**: a reporter found v0.4.2 worse, the sound following the picture (after a
  rebuild they no longer could; in their clip a 333 ms freeze of the picture does not stop the sound). Mixer
  output captured whole (`WT_WAV` with a longer limit) against a run without stalls, Shibuya cutscene at 30, 300 ms stalls:
  no gaps and no change of speed in any run, every offset a whole number of Wwise's 10.67 ms ticks. With the old fixed step
  the sound stays on real time for 75 s while the picture falls up to 5 s behind, then the timeline starts the next stream
  about 4.8 s late, and about 6 s late after that. So long streams run on real time, but the timeline starts them, and it starts its
  other sounds too; the catch-up is right, a racing timeline takes those along. The step at clock+0xe0 is not a playback speed: the game's
  sound code reads it only for delayed stops, one listener fade and a stage parameter.
  `scene_step` (`runtime/scene_step.h`) replaces the 2.0 burst and the frame-by-frame measured step: even on time, up to one
  even step stays owed, past that an eighth of the rest a frame. No stalls at 30: 98.7 % of frames take exactly 1.0, the
  longest step 1.02, 91.98 s. 80 ms stall every 30 frames: longest step 2.0 -> 1.31, frames at 1.5 or more 168 -> 0, length
  92.31 s -> 92.35 s, later streams about 40-60 ms later than v0.4.2. 300 ms stalls: 92.39 s at 30, 92.01 s at 60.
  Linux, not by ear, not run on Windows; reporters' Windows logs show the output at real time, dips only at silent loads and pauses.

## Resolution scale (2026-10-25)

`resolution_scale` (graphics.ini; `WT_VK_SCALE` wins; 0.25 .. 3.0, Vulkan) now scales B3's picture. The scale is relative
to B3's window texture, 1600x900: 1.2 = 1920x1080, 1.6 = 2560x1440, 2.4 = 3840x2160 (3.0 = 4800x2700). The picture goes to
the window at the window's size (graphics.ini `width` / `height`, `WT_WINDOW=<w>x<h>`; above twice the window it is
halved in 2:1 steps first).
- **What was missing**: the screen class (the images made at the scale) took only 1600x900 and its halves. B3 draws its
  scene into 1600x896 targets (1440x816 when its dynamic resolution steps down; depth-stencil, R11G11B10F, R32F, RG8) with
  chains of halves to 45x25 and some other 16:9 targets (1280x720, 1120x628, 960x540 ...): only 13 images were scaled.
  Now any target of the window's shape (within 1.5 %), 40 x 22 or larger, that is not an uploaded texture: 86-117 a run.
  Shadow maps (1024 / 2048 square, a 2048 depth array) and other odd sizes stay as they are.
- **Storage images**: fragment programs #1693 and #5548 write 800x448 / 720x408 / 512x288 targets with SUST at
  gl_FragCoord in the game's pixels; such an image is made again at the game's size the first time it is bound as a storage
  image and kept there (`g_rs_keep`; the shaders that sample it use normalised coordinates). So are the depth levels below it
  (`rs_pyramid.h`, "Depth pyramid at a scale"): 8-20 a run in all.
- **Compute**: B3's compute dispatches touch no scaled image (one 64x64 storage image; `WT_VK_RTLOG=1` lists the render
  targets, every stage's storage images and any scaled image a compute stage samples), so their sizes need no change.
  Viewports, scissors, clears, copies, uploads, read-backs, `gl_FragCoord` and integer texel fetches in fragment stages
  were converted already (track O).
- **Measured** (Shibuya benchmark, 330 s, frames at 60 from 1500, window at the output size; screenshots of the scaled
  image itself with `WT_VK_SHOT_NATIVE=1`, `/tmp/shots/res<scale>/`, `/tmp/shots/hud_1x_vs_4k.png`):

  | scale | output | avg fps | 1 %-low | p99 | frames at 60 | scaled images |
  |---|---|---|---|---|---|---|
  | 1.0 | 1600x900 | 59.9 | 51.7 | 19.3 ms | 98.0 % | - |
  | 1.2 | 1920x1080 | 59.9 | 49.3 | 20.3 ms | 97.6 % | 259 MB |
  | 1.6 | 2560x1440 | 59.8 | 47.9 | 20.9 ms | 97.2 % | 385 MB |
  | 2.4 | 3840x2160 | 59.6 | 47.5 | 21.1 ms | 96.0 % | 976 MB |

  The pictures: the scene, fog, effects, the HUD, subtitles, the title, the main menu, loading screens, a cutscene and the
  game-over screen drawn at the scale, nothing misplaced or cut. In 4K the 60 fps cutscene runs at 56.4 fps (p90 21 ms).
- **graphics.ini on Linux**: until this round the file was never read on Linux (the program's folder was cut at '\'
  only); now `config/graphics.ini` is (resolution_scale and cutscene_fps from the file checked). The tracked file sets
  `anisotropic_filtering = 8` (from B2recomp), which now applies on Linux too; its `resolution_scale` was 2.0 (B2's
  2560x1440; 3200x1800 for B3) and is back to 1.0. `controllers.ini` (nnpad_sdl.c) had the same cut: fixed in the keyboard round below.
- **The clouds of erasure** (the Homunculi's smoke, Shibuya; blurry at 4K in the user's play): tessellated patches drawn
  into fixed 520x360 targets (two RGBA16F + D16 and a 3-layer RGBA16F array; then a full-screen composite). 520x360 is
  not of the window's shape: named in `rs_extra_class`, scaled with the others (1248x864 at 2.4). Crops at 4K
  (`/tmp/shots/smoke_before_after_2700.png`): the blocky, dotted cloud edges against geometry are gone, the composite
  stays aligned. `WT_VK_CLOUD_SCALE=<cap>` limits their scale for a weak GPU (default: the full scale; 4K A/B at equal
  load, 1.0 against 2.4: 78-81 % against 75-77 % of frames at 60). Their fragment stage was the cost: 30x cheaper since
  issue 107 ("The clouds of erasure and the dispatcher").
- **Measurements later on 2026-10-25 were taken with other programs loading the machine** (a game in Java at about two
  cores and the GPU, two long-running emulator processes): 4K runs then gave 75-81 % of frames at 60 whatever the setting
  (the 96 % above was an idle machine). Only A/B runs interleaved under the same load are compared.
- **First run at a scale other than 1.0**: the fragment stages are another translation (one for every scale, keyed
  `rs1`); B3 has no start-up shader preparation (no shader containers), so the first such run translated 3,755 of them
  behind held loading screens: 110.7 s of holds, once per GPU and driver. Later runs at 1.2, 1.6 and 2.4: none.

## Resolution scale, live (2026-11-01)

`resolution_scale` no longer needs a restart: a change in the PC settings panel applies when the panel closes (stepping past
3.0 on the way to another value must not make its images), one in the game's Options rows at once.
- **Images** (`rs_apply`, a job of the render thread): every target of the screen class gets an image of the new size with
  the old texels blitted over, level by level. The contents have to survive: the game does not draw every target again, a
  paused scene is a picture it drew once. `R.col` and `R.depth` name textures, so the next pass takes the new images, in the
  middle of a frame too. Multisampled images and formats that cannot be blitted stay cleared until the game draws them.
- **Stages**: one translation serves every scale other than 1.0, so only a change that leaves or reaches 1.0 changes the
  fragment stages. `SSrc.rs` keeps the class a stage was asked for (a worker may build it after the change) and `g_rs_flips`
  sends each program's fragment stage through `vstage_get`'s lookup once. The two translations stay side by side: going back costs a
  table lookup. 1.0 itself is the translation it always was, so no cache is made again by this change.
- **Measured** (Linux, RX 9070 XT, Shibuya fight, hidden window, another session's two game runs on the same PC): 82-133 images
  in 2-31 ms; 1.0 -> 1.6 -> 3.0 -> 2.0 -> 1.6 -> 3.0 in the fight and through the pause menu, each size confirmed in native
  screenshots (`WT_VK_SHOT_NATIVE=1`, 1200x675 to 4800x2700), 0 Vulkan call failures. The first change away from 1.0 with
  the `rs1` stages already on disk: frames of 114-213 ms for about 8 s, no longer stop.
  With `WT_VK_VALIDATE=1`, 1.0 -> 1.2 -> 2.4 -> 0.8 -> 1.0 from the panel gave no message but the 10 unused-output warnings
  an equal run has without a change.
- **Not measured**: that first change on a PC whose cache has no `rs1` stages (the game translates what each draw needs, held,
  on the render thread; 3,755 stages took 110.7 s over a whole first run). Windows, and a change from the Options rows (their
  job post was seen working for vsync, not for the scale).
- **Regression, fixed after v0.5.2**: `SSrc.rs` was set only in `ssrc_of`, so the preparation's two readers (`cont_stage`, the
  store's walk) made the 1.0 translation of every fragment stage at any other scale, and the game translated all of them
  again behind held loading screens: a row of short bars after the first-run screen. They set it now, and the marker of a
  scaled run is `_s2_`, so a PC prepared by v0.5.1 or v0.5.2 at a scale prepares once more; 1.0 keeps its marker. Cold first
  start to the title, scale 2.4, RX 9070 XT, hidden window: 602 holds, 46.9 s and 41 bars before; 5 holds, 32 ms and no bar
  after, second start no translation. Title pictures aligned by scene differ only in their random particles.

## Depth pyramid at a scale (issue 93, 2026-11-01)

Black triangles and cut-out steps along the outlines of characters at any `resolution_scale` above 1.0, worst when the game's
dynamic resolution steps down, with colours that stop on a grid of 16 game pixels. The game builds a min/max depth pyramid:
fs5548 stores the 1/2 level, fs5550 halves it to 1/4 and 1/8, and fs2263, in a pass with no attachments, tests lights against
the depth range of each 16-pixel tile and stores the tile's light mask, read by the materials. fs5548's image was already kept
at the game's size (a storage image), but 1/4 and 1/8 were scaled and fs2263 still takes 2x2 texels per tile: 17 % of the tile
at 2.4. An edge outside those texels is left out of the tile's range, so the tile loses its lights.
- **Fix**: the colour target of a draw from a kept image stays at the game's size when it is the next level (`rs_keep_level`,
  the rule in `rs_pyramid.h`, `tests/rs_pyramid_test.c`). Dropped before the draw fills it, so no frame reads an empty level.
  `rs_apply` leaves kept images alone, so a live change needs nothing more.
- **Measured** (Linux, RX 9070 XT, the reporter's save, Chapter 1's intro, dynamic resolution forced down by test timestamps):
  with the fix off "Why do you know our names?" and "And if they can pull it off" show the steps and
  triangles of the reporter's Windows screenshots on the same outlines; with it on, not on the frames compared. Only fs5550 is
  caught, at every dynamic size, in the intro and a chapter of play (an earlier form of the rule also caught fs1727, a radial
  mask, which reads the 1/2 level through a UV: such a stage has inputs, fs5550 has none). 2.4 forced and nothing forced, and a
  live 1.0 -> 2.4 -> 1.0 -> 2.4 under validation: 0 Vulkan call failures, no VUID. 0.8 ran on the earlier form only.
- **Left as they are**: dark marks of a game pixel or two on the same outlines. They are there at 1.0 too, and with fs5548
  reading the whole of its footprint at 2.4 (fs5548 still takes 2x2 texels of the scaled depth).
- **Not measured**: NVIDIA and Windows, where it was reported. The square steps in the title's colours (issues 66, 89) fit this
  grid, but that frame did not come up here (the title's scenes are random), so the fix is untested on them.

## Stores below a scale of 1.0 (issue 147, 2026-11-05)

Below 1.0 the scene is drawn at `resolution_scale` itself (the never-below-1.0 rule covers only FSR's ratio). fs5548 (the 1/2
min/max depth level) and fs1693 (a half-size colour) store into images kept at the game's size, at the game pixel their
fragment rounds to, when that pixel's x and y are even: on a raster smaller than the game's, whole columns and rows are never
stored. At 0.8, every fifth one: a fixed grid of zeros (the image's clear), so 64 % of the 1/4 level had a minimum of 0.
- **Fix** (`rs_store_low`, `nvn_vk_draw.c`): such a draw (a fragment stage with images and no buffer writes, in a scaled pass
  below 1.0) runs once more in a pass without attachments as large as its viewport, in the game's pixels (mx_fcs 1), so every
  texel gets its store and the stage's quad is the game's 2x2. Its colour outputs go nowhere (the first draw wrote them); a
  sample counter is paused for it. `WT_VK_STOREFULL=0` turns it off.
- **Measured** (Linux, RX 9070 XT, 0.8): the 1/4 level of the title's frame 1500 has no zero minimum and a median min/max
  spread of 0, as at 1.0 (off: 64 % zeros, median spread 1.0). The Chapter Select glove, matched by pose to a 1.0 run frame by
  frame: mean error 2.19 off, 1.36 on, better in all 11 frames compared. 0 Vulkan call failures, the same three validation
  warnings as without it (unused fragment outputs), no holds. Cost: 0.15-0.33 ms of GPU a frame at the title (two passes of
  the game's size and their barriers), only below 1.0.
- **Not shown**: the reporter's squares on shadows and water (RX 560, RADV) did not come up here; a fight gives no same-pose
  pair. Not run on Windows.

## Level of detail (2026-10-25)

- **`texture_lod_bias`** (graphics.ini; `WT_TEXTURE_LOD_BIAS`; -3 .. 3, default 0 = the game's own): added to the mip
  level of every sampler that uses mip maps (clamped to the device's limit). At -1 the same view is sharper (high-frequency
  energy: near street 113 -> 153, far street 273 -> 292, distant buildings 292 -> 340); no measurable cost (4K A/B:
  77-80 % against 77-79 % of frames at 60).
- **`model_lod`** (graphics.ini, PC settings and the Options screen, "Model detail"; `WT_MODEL_LOD`; game | highest, default game; issue 86):
  `7100bd563c` is the model level selector (its one caller `7100bd5b3c` runs the cross-fade): it writes the level to `*x1`,
  -2 none, -1 hidden, 0 finest, and past the last distance it gives -1. The Switch "Increased LOD" mod (`mov w8, #0` at
  `7100bd5b2c`) was the lead. `h_model_lod` (game_shim_eh.c) has the same effect after the call, keeping the distance cut
  of meshes with one level. Same scripted route, gameplay frames 3800-8000, RX 9070 XT: 0.92 million indices a frame become
  3.42 (3.7x), 1165 draws become 1608. 60 fps frames, scale 2.4, the clean pair of two: 16.68 -> 16.71 ms mean, p99 21.4 ->
  19.8, GPU 3.57 -> 3.77 ms a frame; scale 1.0: 16.97 -> 16.91 ms, GPU 1.37 -> 1.50. Some runs of either setting have slow
  stretches (a first run read 25.7 ms, a repeat did not): compare per-frame data over repeats, never one run. One or two
  levels finer than the game gave 1.55 / 1.90 million indices and no frame time worth trading for; no weak GPU was tried.
- **`film_grain`** (graphics.ini, PC settings and the Options screen; on | off; issue 86). The grain is `m_GrainIntensity`,
  struct offset 0x1fc, `c4[13]` in the four `filtershadertonemap*` stages (the "No Film Grain" mod clears that field's
  registration, in the two tables of `0x7102632810` and `0x71026329d0`); stages are told by a hash of their code because
  12.9898 is also in glass, fog and sky. The game sets it per scene: 0 in Chapter 2 gameplay, 0.078-0.2 on frames 575-1248,
  0.5 on the opening book (2218-2423 of the test route). There the noise floor is 0.633 with grain, 0.180 without.
- **Texture filtering fixed, every setting on the Options tab** (2026-11-10): anisotropic filtering is 16 and the mip bias -1
  for everyone (`gfx_options.c`; `profile = low` keeps the game's own, `WT_TEXTURE_LOD_BIAS` still wins); the two keys are
  read and ignored, and their rows are gone from the panel, the Options screen and the launcher. The Options screen's
  "PC Settings" tab (`docs/RE_NOTES.md` section 8, "The fifth tab") has every row of the panel but Controls: window size,
  FSR 3.1, picture fit, button prompts, Swap Confirm and face buttons are new there, and Internal Resolution has all 11
  sizes. Texts: `text_strings.rs`, with shorter second spellings where a language's file lacks letters (Xbox is "MS" in
  the Chinese and Korean files, which have no X). Checked on Linux in English only; the launcher was not rebuilt.
- **Options screen rows** (`runtime/options_row.c`, rows 34 and 35): Model Detail (Default / Highest) and Film Grain (ON /
  OFF, the game's own words), after Texture Detail; their text in `tools/wtbuild/src/text_strings.rs`.
  "Highest" is spelled with each file's glyphs: Highest (en, ja, ko), Maximal, Maximum, Massimo, Максимум, Ultra (es, zh-Hans,
  zh-Hant: no 'x', 最 or 高 there). Seen in 10 languages from the title and from the chapter menu: both rows change
  graphics.ini and apply at once; nothing overflows; the ko label and the ja Film Grain are English, as the other rows there.
- **Motion blur: no option.** `reconstructionblur`, its tile passes, `filtershadercameravelocity` and
  `modelshadervelocitymap` drew 0 times on that route (title, opening, Chapter 2 to the fight), so skipping the blur draw
  was never seen on a real frame. It needs a scene where the chain runs first.
- **Model LOD, earlier candidates (not the selector).** Candidates read and tested with a host override that scaled their values, measured by
  the indices a frame draws (`WT_VK_PRIMLOG`) on the same idle view:
  - `LodDistance0..3` / `UseLodDistance0..3` (reader `71007d3a5c`, pairs of float + flag): the values seen are 0, -1, 0.5,
    0.25, 5 / 15, 10.8 / 26, and every Use flag is 0: an effect system's settings, not models;
  - `Lod { Lod0..Lod6 }` beside `Deletable { max, extend, vanishTime }` (reader `7100646060`, seven floats at
    +0xc0c..+0xc24 of one object): read once, 0 10 20 30 50 80 100. Scaled by 0.2 and 3: 1.01 / 1.04 million indices a
    frame against 1.02-1.07 million at 1 (two runs): no effect on this view (likely the breakable props' table);
  - `LOD0..LOD7` names (`71007f44ac`): an enum parser of level names (models' LOD groups or settings), reached through a
    function pointer.
  The temporary overrides were removed.

## FSR 3.1 as anti-aliasing (issues 118, 119; 2026-11-03; experimental)

`upscaler = fsr3` (graphics.ini, PC settings "Anti-aliasing", the launcher; `WT_UPSCALER` wins; off by default): AMD FSR
3.1.4 at the internal resolution (render size = output size, AMD's "native AA"), with no AMD binary: the upscaler's GLSL
from the FidelityFX SDK v1.1.4 (MIT, `runtime/third_party/fsr3`, 27 files) compiled by our glslang at first use (8 compute
pipelines, 0.5 s once, then the SPIR-V cache), and a host of our own (`runtime/nvn_vk_fsr.c`) that follows the SDK's
`ffx_fsr3upscaler.cpp` for constants, resources, clears and the order of the passes.
- **The engine draws no motion vectors.** Its motion-blur and velocity programs (`reconstructionblur` and its tile passes,
  `FilterShaderCameraVelocity`, `ZLinearCamVelocity`, `modelshadervelocitymap(scr)`, `VelocityMap_*`, `CnsVelocity*`) are built at
  load (`f_71006a2408`, `f_71006a55b0`) but no code binds them for a draw: the one material lookup `f_7100d47e8c` is called with
  slot 3 (the velocity technique) NULL (`0x71007f5058`), the per-type binder `f_7100d5740c` is reachable only from technique
  objects no list holds, and pass 0x49 is begun and cleared to zero velocity every frame with no draw. No pipeline with any of
  these stages was ever made in five caches. So there is no flag to turn on; the inputs are made here.
- **Jitter**: `h_cam_upload` (game_shim_eh.c; the direct call at `0x7100cbb4d4` to the camera upload `f_7100cb9440`). The
  scene binds the record `*(*(0x710249d0d8) + 0x16d0)` of the pool `*(0x71024ac350)` (`0x71006888dc`); while the runtime runs FSR
  (`game_fsr_w` / `_h`, the scene image's size) that record is uploaded every frame with its projection (+0x40) and view-
  projection (+0x80) shifted by FSR's Halton(2,3) step (8 phases at 1:1), then both and the copies made from them (+0x1b0, when
  +0x400 is set) are put back. Shadows (c8), the full-screen filters (c4) and the HUD (its own cameras) do not read that buffer.
- **Motion vectors**: one compute pass at the scene's size from its depth (D24S8, 0 near, 1 far) and this and the last
  frame's view-projection without jitter, both read from c3 of the scene's draws (rows 32..47; the jitter back from [0][2] and
  [1][2], which are 0 in the game's projection). Everything that moves on its own gets the camera's motion only.
- **Where**: on the scene's HDR colour (R11G11B10F) after the transparents, at the first bloom downsample of the frame that
  reads it (`stage_fx` FX_DOWN: `filterdownsample.pso`, `filterbloomwithdownsample.pso` of shader.dat; the tonemaps as a
  fallback); the result is copied back, so bloom, exposure, the tonemap, the grain, FXAA and the HUD run as before. FSR's
  auto-exposure; no reactive or transparency mask; no sharpening.
- **Measured** (Linux, RX 9070 XT, RADV, the issue-72 save: Chapter 2's start, its cutscene, the track fight; hidden window):
  at 1.0 the first run happens before a bloom downsample, the jitter follows the sequence, 1800 frames had 11 history
  resets (dynamic-resolution level changes and guessed camera cuts). Rails, the platform edge and the canopy lose their steps
  and crawl (`~/.cache/wt-118/impl/cmp`); the gravel is a little softer. At 2.4 (the fight at 3456x1958, the game's 1440x816
  level) FSR and its copy take 1.59 ms of GPU a frame (`WT_VK_PASSTIME`): 6.12 against 4.69 ms in marked segments over 9,914
  fight frames; frame times equal (21.8 ms mean over the route, which waits on the CPU). Validation layer: no message of ours.
- **The dither (step 2)**: fading objects (LOD cross-fades, objects near the camera) discard in their depth pre-pass (ZMap_*,
  492 stages) against a 64 x 64 RGBA8 pattern at the pixel, `texelFetch(t14, uint(gl_FragCoord) & 63)`, threshold
  R * c7[4] + G * c7[5]. The game's pattern (read back): R 1 .. 254 in 2 x 2 blocks, none in 96 .. 127, G = 255 - R exactly,
  so the two meshes of a cross-fade (weights (1, 0) and (0, 1), alphas a and 1 - a) cover each pixel once; nothing in it changes
  from frame to frame (the weights alternate per object, not per frame). While FSR runs, every draw into a scene depth that
  samples that texture at unit 14 gets one of 13 phases of it (`runtime/fsr_dither.h`, `tests/fsr_dither_test.c`): phase p gives
  each texel the thresholds of the texel p * 4096 / 13 places further in the order of R (ranks, not values, because of the
  uneven histogram), so every frame is a permutation of the game's own values and covers exactly what the game's pattern covers;
  over frames (order 8 * frame mod 13, about the golden ratio, coprime with the 8 steps of the jitter) a pixel is drawn in that
  same share, within a frame over any 3 frames in a row on the game's real pattern, and FSR's history averages it. The texture
  is learnt from a depth-only draw into the scene's depth, read back once (one GPU wait) and used only if it is that kind of
  pattern (unit 14 is an ordinary material texture in about 1,500 other stages; the effects read another 64 x 64 texture there
  by screen position, left alone). Shadow casters keep the game's pattern. Measured on a billboard during a cross-fade of the
  route's cutscene (frame 3702, every pattern draw forced to discard where R > 0.5 by a test switch, since removed): relative
  hole energy 0.76-0.93 before FSR; after FSR 0.13 with the phases against 0.37 with the game's fixed pattern (which the jitter
  only blurs: 0.30, 0.23 the next two frames). The route's fades last about 10 frames; a long fade near the camera (the
  reporter's case) was not on it. `WT_FSR_DITHER=0` (TEST): the game's pattern with FSR; `=alt`: fixed in every other 30 frames.
- **Not done**: Bayonetta's whipping hair and fast limbs smear a little (camera vectors only; per-object vectors would need the
  game's velocity vertex stage, `modelshadervelocitymap.vso`, drawn again, or a previous-position output in the translator).
  Each change of the game's dynamic-resolution level (8 sets, 1600x896 .. 640x360;
  10 in 3.5 minutes of the route, mostly around the cutscene) makes the resources again and resets the history; making every
  report of a frame carry one timestamp did not stop them (the game seems to measure from frame to frame), so it was taken
  out. Upscaling: the next section. Not run on the Deck, Android or Intel.
- **NVIDIA (issue 140, 2026-11-05)**: on an RTX 4090 (Windows, driver 621.14, Quality at 2.4, a fresh save's title screen)
  v0.8.0 lost the device (`vkQueueSubmit`: VK_ERROR_DEVICE_LOST, exit code 5) after about 1100 FSR frames, with no hang
  first. Not reproduced on the RX 9070 XT (RADV; core, sync and GPU-assisted validation clean over 18,000 FSR frames). The
  suspect: AMD's samplers `s_PointClamp` / `s_LinearClamp` (bindings 1000 / 1001) were immutable samplers in push layouts
  that were never pushed; the spec makes them undefined until pushed, RADV inlines them and the validation layer counts them
  as written. They are now plain sampler bindings pushed with every pass. Also: AMD's `ComputeMinimumDifference` read
  `fSamples[5]` of a 5-element array (clamped, no result changes); a lost device prints the driver's `VK_EXT_device_fault`
  report (`nvn_vk_fault:`), and `WT_VK_CHECKPOINTS=1` names FSR's pass. Not confirmed on NVIDIA yet.
- **FSR 4 and frame generation**: FSR 4.1.2 on Vulkan exists only as the SDK 2.4.0 signed Windows DLLs (2026-10-28; RX 6000
  and newer); Adrenalin does not upgrade a Vulkan FSR 3.1, and the GPL code in runtime/ (ssbu-recomp, suyu) rules out shipping
  them: at most a DLL the player adds. AMD's frame-generation swapchain wants four queues and its own present thread, against
  display.c's pacing (issue 119 waits on this).

## FSR 3.1 quality modes (issue 118, step 3; 2026-11-03; experimental)

`upscaler = fsr3_quality | fsr3_balanced | fsr3_performance | fsr3_ultra_performance` (graphics.ini; PC settings row "FSR
3.1": Off, Native AA, Quality, Balanced, Performance, Ultra Performance; the launcher; `WT_UPSCALER` wins): the scene is
drawn at the internal resolution divided by AMD's ratio (1.5, 1.7, 2.0, 3.0) and FSR 3.1 upscales it to the internal
resolution. `fsr3` stays native AA.
- **Two scales** (`runtime/nvn_vk_draw.c`): `rs_scale()` is the picture's (resolution_scale), `rs_render_scale()` the scene's,
  latched together with the mode, so a change applies through `rs_apply` (the row is A_SCALE: when the panel closes). The
  scene's scale never goes below 1.0: fs5548 and fs1693 store kept images at the game's pixels, and a raster smaller than the
  game's would leave texels out. So the modes do something only above 900p, and at 2.4 Ultra Performance (0.8) draws at 1.0.
  Below 1.0 the scene keeps the picture's scale. Images of the picture's scale (`VTex.post`): the window's size (window
  textures, UI captures, the movie's Y plane) and the tonemap's targets. All the others, the scene and its bloom chain, are at
  the scene's scale. `rs_size` gives both sizes. A pass with attachments of both classes is drawn in the area all of them hold
  and counted (`g_rs_twoscale`, printed at exit): none on the route.
- **FSR** (`runtime/nvn_vk_fsr.c`): render size = the scene colour's image, output size = that colour's game size at the
  picture's scale. The locks and the two histories are at the output's size (AMD's maxUpscaleSize). The accumulate pass is
  dispatched over the output, `downscaleFactor` = render / output, and the jitter has AMD's phase count, 8 * (output / render)^2
  rounded: 18 at Quality, 32 at Performance, 46 at Ultra Performance on its floor at 2.4 (`game_fsr_phases`, read by
  `h_cam_upload`). Jitter and motion vectors stay in render pixels.
- **The picture after FSR**: the first tonemap (FX_GRAIN) draw that samples the scene colour after FSR has its target made
  again at the picture's scale (`rs_promote`, before its pass; one per dynamic-resolution set, a list of 64). That draw reads
  FSR's output in place of the scene colour (`fsr_up_view`, next to the dither's view in `resolve_sampler`). In a frame
  without FSR it reads a linear enlargement of the scene colour (counted: "tonemaps without FSR"; 0 on the route). Bloom reads
  the scene colour at its own size with a 2 x 2 gather, so FSR's output is also blitted back into it, reduced linearly (one
  step, also above 2:1). The grain, FXAA, the HUD and the present run as before at the picture's scale.
- **Measured** (Linux, RX 9070 XT, RADV, the issue-72 route, resolution_scale 2.4, picture 3840x2150; GPU time per fight frame
  in marked segments, `WT_VK_PASSTIME`; about 8,100 fight frames each):

  | upscaler | scene (1600x896 level) | GPU ms per fight frame | FSR and its copy |
  | --- | --- | --- | --- |
  | off | 3840x2150 | 4.79 | |
  | fsr3 (native AA) | 3840x2150 | 6.70 | 1.64 |
  | fsr3_quality | 2560x1434 | 4.43 | 1.20 |
  | fsr3_performance | 1920x1075 | 3.66 | 1.07 |
  | fsr3_ultra_performance | 1600x896 (the floor) | 3.31 | 1.02 |

  At 1.0 with FSR off: 1.75. Frame times were the same in every mode (21.8 ms mean over the route: it waits on the CPU), so
  the gain is GPU headroom. Quality looks close to native AA on the route (`~/.cache/wt-118/impl/runs/s3f_*`); Performance and
  Ultra Performance soften the signs' small text. Validation layer: only the warning every run already had (a fragment output
  with no colour attachment). Changed in the panel while playing: Quality to Balanced (the scene's 1.600 to 1.412, 124 images
  made again in 10.2 ms; FSR made its resources again, the output kept its size), Quality to
  Native AA (1.600 to 2.400, 124 images in 7.7 ms) and Native AA to off (the scales unchanged: FSR stopped, no image made
  again).
- **Not done**: no negative mip bias for the smaller scene (AMD suggests log2(render / output) - 1; texture_lod_bias exists),
  no RCAS sharpening, no reactive mask. A device without what FSR needs (`fsr_unable`) keeps the picture's scale; a pipeline
  that fails at the first run applies the scales again (`rs_apply`; tried once with a forced failure: 30 images made again
  in 2.2 ms, the picture at its full size). Not run on Windows, the Deck, Android, NVIDIA
  or Intel.


## Object motion vectors (2026-11-11; with FSR, experimental)

FSR gets the camera's motion only (above). For things that move on their own, the plan: the vertex stage also gives last
frame's position (step 1), the host keeps last frame's object constants (step 2), and a pass of our own draws the moving
objects again into FSR's motion-vector image with a depth test for equality (step 3). `WT_FSR_MV=0` turns the pass off.
- **What the game has** (the prologue's cutscene, 176-217 pre-pass draws, 19 vertex programs; four read as GLSL): position =
  projection (c3[16..31]) x view (c3[0..11]) x a matrix of constant buffer 14, which is the object's palette of 3 x 4 bone
  matrices (0x30 each, up to four weights a vertex: skinning is in the vertex stage) or, for the instanced stages, one 0x40-byte
  record an instance. It holds this frame only: there is no previous transform anywhere, so `modelshadervelocitymap.vso` drawn
  again would need ours too. Each object owns a ring of 3 slots (some 6) for it, its address changes every frame, and the draw
  order changes between frames.
- **Step 1** (`runtime/mx_translate.c`, `MxIn.prev_cbufs` / `prev_base`; `tests/mx_prev_test.c`): the chosen constant buffers
  get a second block `mxpN[]`; the stage's code runs first on those, its `gl_Position` kept in the output `mx_prev`
  (`MxOut.prev_loc`), then as always. A stage that reads none of them or stores gets the text as before. 400 of the game's
  vertex stages translated that way compiled with glslang; with the option off the text is byte-identical.
- **Step 2** (`runtime/mv_match.h`, `fsr_mv_draw` / `fsr_mv_last` in `runtime/nvn_vk_fsr.c`, `h_palette_upload` in
  `runtime/game_shim_eh.c`; `tests/mv_match_test.c`): while FSR runs, buffer 14 of every pre-pass draw is kept for one frame
  (100-200 KB) by its address, and the camera's c3 of the last frame (`F.c3_prev`). **The game says which slot was last
  frame's**: `f_7100d17d44` (object x0, buffer table x1, mesh part x2; one caller, `f_7100d03668`, on three job threads)
  writes a part's palette into slot `buffer[+0x30] + buffer[+0x38] * index` of the buffer `*(x1 + [object + 0x88] * 0x18)`.
  That entry is one of two buffers in turn, each with its own index (+0x68, going 0, 1, 2 with each write), so a palette goes
  through six slots and the phases differ from part to part. The override notes each write by (object, part) and exports
  the pair (this slot, the slot of the write before) in `game_mv_pairs`; the runtime looks the pair up at the draw and
  takes last frame's bytes of that slot. A block with no pair is looked for at its own address (a slot not written again).
  TEST log `WT_FSR_MVLOG=1`.
- **Measured** (Linux, RX 9070 XT, 300 s from New Game with `WT_SKIP_MOVIES=1`: title, the prologue's cutscenes and its
  fight; 49 logged frames, 4,318 blocks): 3,827 have last frame's bytes (88.6 %; 3,497 of them changed), 454 have no pair
  and 37 a pair whose slot was not drawn last frame. The ones without: the instanced stages' records (0x2000 blocks, copied
  by `f_710080c698`, not hooked: they keep the camera's motion) and objects in their first frame. Validation layer: only the
  known warning; `wtbuild smoke` and the lifter's golden (blessed: one more override) pass.
- **Tried first, dropped**: pairing a block with last frame's nearest block of its size. Over 4,061 blocks 21 % were paired
  at a squared distance over 100, which is no small motion, and it cost 0.26 ms a frame on the render thread.
- **Fights** (the prologue's, frames 6000-8100 of that run): no pre-pass draw takes a vertex stream from a compute buffer
  and no pre-pass vertex stage reads a storage buffer (0 of 10,020 logged draws). The dispatches of a fight (about 6 a frame)
  feed one or two draws a frame outside the pre-pass (particles). So step 1's vertex variant is enough for what has depth.
- **Step 3, the velocity pass** (`fsr_mv_prepare` / `_record` / `_commit` / `_pass` in `runtime/nvn_vk_fsr.c`, called from
  `draw_prepare` and the two direct draw commands): a pre-pass draw whose palette has last frame's bytes and changed is noted
  with what the command buffer holds for it (vertex input, buffers in the stream ring or kept vertex buffers, viewport,
  culling, depth bias) and drawn again in `fsr_run`, after the camera's compute pass, into FI_MV: the stage's step-1 variant
  with last frame's c14 and c3 at bindings 54 and 43, a fragment stage of ours (previous and current NDC without their
  jitters, as the compute pass), depth test EQUAL on the scene's depth, no depth write. The game's draws and pipelines are
  untouched. A stage's pipeline is made by a thread of its own at the stage's first draw (40 ms each on a cold cache, 0.3-6
  warm); until then its draws keep the camera's vector. Not drawn: instanced and tessellated draws, vertex stages with a
  texture or storage buffer, draws fed by compute, indirect draws, an object in its first frame, the draws noted before a
  wrap of the stream ring.
- **Checked** (Linux, RX 9070 XT, RADV; raw FI_MV of a frame before and after the pass, `WT_FSR_MVDUMP=<frame>`):
  with `WT_FSR_MV_ALL=1` (TEST: unmoved objects drawn too) in a cutscene with a moving camera (vectors 0.53 px median, 4.6
  max) the pass covered 93.3 % of the picture and 86.2 % of those pixels came out within 0.02 px of the camera's vector
  (median difference 0.001 px): the depth test passes on whole objects, and the conventions, both jitters and last frame's
  camera agree with the compute pass. In the prologue's fight the pass wrote 8.6 % of the picture (the character, her hair
  and ribbons, the gun: 12 px median, 130 max, against 0.09 px of camera motion), in a cutscene 22 %. Validation layer:
  only the known warning, also in a quality mode; shutdown clean.
- **Cost** (resolution_scale 2.4, fsr3_quality, frames 4500-8400 of the route, two runs each way, alternating): batch replay
  2.03 / 1.88 ms a frame with the pass against 1.82 / 1.79 without, GPU 5.44 / 5.27 against 5.22 / 5.28 ms, 59.7-59.8 fps
  either way: at most 0.2 ms of each, inside the spread of the runs.
- **Not shown**: that the picture is better. Two runs do not reach the same frames, so there is no pair of pictures with
  and without; the frames with the pass look sound. Not run on Windows, NVIDIA, Intel, the Deck; a fight with many enemies
  and the 30 fps cutscene mode not measured.

## Frame rates above 60: feasibility (2026-10-25; a study, not implemented)

**First step done (2026-10-27): `frame_rate = 61 .. 120` in graphics.ini.** `h_frame_clock` (`runtime/game_shim_eh.c`)
replaces the measured step by this frame's ms / +0xec with the floor 1/16 instead of 1.0 (the 2.0 ceiling and the global
override stay, the carry is zeroed); the 30 fps-mode scenes run at frame_rate as with `cutscene_fps = 60`; display.c paces
at frame_rate (Recomps#1). Measured (1.0 scale, vsync = clock, 120): the Shibuya street from the end of loading to the
cutscene 105.12 s against 105.07 s at 60 (real time kept); the fight 105-120 fps, p50 8.4 / p90 10.6 / p99 31.7 ms.
**Not done**: the census of frame counters and smoothings below (input windows, countdowns, effects may run fast), and
the frames -> seconds conversion `7100cb4f54`. Not played by hand.

**Frame counters at any frame rate: census and first sites (2026-10-27).** Rust lifter only (`lifter/recomp_eh.py` is
not changed; it is to be retired).
- **Sites** (`tools/wtbuild/src/lifter/fps.rs`): every load of a field, add / sub (immediate, register, float register,
  +/-1.0) of the loaded value and store to the same field, conditional branches and a CSEL allowed between: 17,370 in
  main (12,147 int32 immediates, 4,233 float adds of a register, ...).
- **Census**: `WTBUILD_FPS_CENSUS=1 wtbuild setup --from lift` makes every site's store call `fps_census` first
  (`runtime/game_shim_eh.c`; `build/gen_eh/fps_sites.txt` lists them); a run with `WT_FPS_CENSUS=<file>` records per
  site the visits per field against the clock's updates and what each visit adds. `wtbuild fps-census <at 60>
  <above 60> [--all]` compares two runs of the same scene: a site that adds as much per visit at 120 as at 60 is frame-locked;
  half as much, it takes the frame's time already. One run at each rate replaces B2's alternating 60 / 120 run. A normal
  lift writes the files back (about 3.5 minutes of compile each way).
- **Shibuya street fight, 300 s at 60 and at 120**: of the sites visited once per update, 147 already take the frame's
  time (most of the gameplay: B3 times with the frame's delta far more than B2), 61 are frame-locked but on a thread
  with its own clock (visited as often per second at both rates: engine, audio, 60 Hz vsync), **62 are frame-locked on
  the game's frames**: the list to read (`build/perf/census_report.txt` of the run; regenerate with `wtbuild fps-census`).
- **Fix, variable rate** (`tools/wtbuild/fps_counters.txt`: the store addresses; the lifter rewrites each site's op):
  an integer counter moves by the whole frames of game time due since its last visit (`fps_ci`: B2's rules: a countdown
  never passes 0 in one visit, an up-counter moves at most 1, the rest is owed), a float +/-1.0 the same (`fps_cf`), a
  float x +/- a by a x this update's step (`fps_fs`: fractional, motion stays smooth). Each clock update adds its step
  to the frames due, so any rate works, also one that changes from frame to frame. Off (stock) at frame_rate = 60.
- **First 10 sites**: float timers of a constant a frame (-1/60 twice, -0.1, +0.1, -0.33), two float countdowns of 1.0
  (one on 196 objects), three integer countdowns. A census of the patched build at 120 against the 60 run: each adds
  half as much per visit (ratio 1.85-2.11). Two more (`7100694cbc`, `7100694d60`) did not (1.19) and were taken out.
  300 s at 120 with the census build and 120 s with the normal build: no error.
- **The demon form's double-tap** (a player report: two quick dodges while moving forward no longer transformed above
  60): the window is a frame countdown, `710055a41c` (`[x19 + 956]`, function `7100559ebc`), so at 120 it lasted half
  as long. Scripted double-taps (`WT_SCRIPT` forward + ZR twice) at 60: 0.12 and 0.20 s transform, 0.30 not; at 120
  only 0.12. The six frame-locked countdowns of a double-tap census patched together, then bisected; with this one
  listed, 120 matches 60 (0.20 transforms, 0.30 not). The first 10 sites were not the cause (`WT_FPS_COUNTERS=0`,
  new: the sites' stock +/-1 at run time, the same failure). It was not in the 62 of the fight census: it runs only
  after a dodge, too few visits for its filter. Input windows like it need a census of the move itself.
- **Stick-motion moves and Witch Time (issue 134, 2026-11-05)**: back, forward + attack and forward, forward + attack
  rarely came out at 120. The stick tracker `710055a7ec` (B2's `71000579d0`; input block = player + 0x1780, called once
  an update by `7100559ebc`) counts every window in logic steps: the next push (+0x1e0, 10), same direction twice
  (+0x1ec), opposite directions (+0x1e8), a second set of 15 (+0x204 / +0x20c / +0x210), two arrays of 8 octants (22 and
  27) for the rotations. All 28 of its countdowns are listed now, with the command decoder's press buffers (`710055b058`:
  20 / 28 / 5 / 40 steps per command, so a press during a recovery waits as long as at 60), the 5-step window after a
  hit (Viola's parry, Bat Within, Mahaa-Kalaa start Witch Time inside it) and the Witch Time manager `71007a231c`'s
  slow-down slots (its intro slow, 16 frames at 0.125; B2's "Witch Time's end"). The dodge's own window and Witch
  Time's length already take the step. Scripted flicks 0.05 .. 0.25 s apart, then punch (`~/.cache/wt-134`,
  `trials.py`; a census build with a probe of the sites' stores): at 60 both windows open up to 0.14 s, not at 0.18;
  at 120 stock only up to 0.08; patched, 120 matches 60 in all 24 trials. The intro slow: 30 store visits at 60 and at
  stock 120, 60 patched. **The finder** (`lifter/fps.rs`) missed three forms: a store through a copy of the base
  (`mov x19, x0`), a second decrement that reaches the store by a conditional branch (+0x1ec, +0x418 and four
  manager slots count down on two paths; the old finder patched only the rarer one), and any site after a `b.lt`
  whose register number equals the condition (11), which `writes_gpr` took for a write; it also took 64-bit ORR /
  ADDS (shifted register) for a store pair. **`fps_ci` at 0**: these sites run every update, also at 0, and below
  60 fps (frame_rate above 60) frames of 2 steps piled up there as owed time (up to 8) that the next window paid at
  once: at 44 fps a 15-step window went 15 -> 7 on its first visit. A countdown at 0 now owes nothing; the same
  window then lasts its 0.25 s. Not listed: the input manager's four 10-step button latches (`7100790018` ..; no
  reader found), the manager's +0x70 / +0xac (never set) and +0x7c (an STP store the finder does not take). Not
  played by hand.
- **Hit-stops and vibration (issue 134 follow-up, 2026-11-05)**: a census of the Shibuya fight at 60 and 120 (240 s
  of combo attacks): Bayonetta's own hits keep real time already (their per-object timers are floats that take the
  step); the engine's time nodes (tick `7100c1f798`, request `7100c1f100`) freeze an object only when an enemy's
  attack lands (`71000d3440`: the attacker and its victim, 2 .. 12 frames at 0.01, 30 for one enemy) and in
  Bayonetta's damage state 204 (5 frames). Listed: the node's frames (`7100c1f904`), and the pad's vibration blend
  `7100cc3710` (`7100cc376c`, `7100cc37ec`: each rumble's steps; B2recomp's issue 53 "vibration feels weaker at
  120"). The node's delay (`7100c1f87c`) is never above 0 (all 293 requests pass 0) and the objects' delay slots
  (`71000830a0`, `7100501240`) mean "after this update's hits": not listed. Measured with the bot idle between
  attacks so that enemies hit it (probe of the stores, `~/.cache/wt-134/runlen.py`): a 3-frame freeze 0.033 s at 60,
  0.017 s at 120 stock, 0.033 s patched; 9 frames 0.079 s stock, 0.133 s patched; rumbles of 11 / 15 steps 0.189 /
  0.233 s at 60, 0.084 / 0.145 s stock, 0.165 / 0.232 s patched. A 2-frame freeze ends about 4 ms early (the first
  visit moves 1). The other frame-locked event counters of that census are load waits, handle counts, a dead BGM
  countdown and cosmetic fades (a lighting crossfade `71007f2f30`, model fades `71007f0984`); not listed.
- **Next**: read the other 52 frame-locked sites (global frame numbers and the clock's own count stay as they are;
  the large per-object ones on the job threads, 600-1,000 fields with a varying add, are probably effects), then
  smoothings (x += k (T - x), adds whose ratio is between 1 and 2), other scenes (menus, cutscenes, Witch Time, other
  characters), and play by hand. The sites' meanings are not read yet: only their form and their numbers.

**How B3 times a frame** (read off the lifted code; the same engine generation as Bayonetta 2):
- The per-frame clock `7100c1f600` (called by the main loop `7100670b80` before it picks the frame-rate mode) measures
  the last frame in ms (+0xf0) and keeps the frame's **step** at +0xe0, in 60 Hz frames:
  - gameplay (w1 = 1): the mean measured frame time since the last step / the ms of a 60 Hz frame (+0xec), minus a
    carry (+0xe4), **clamped to 1.0 .. 2.0** (the rest kept as carry); a global override (`7100ba3300`, a float that
    wins when above 0);
  - title, menus, cutscenes (w1 = 0, state flags 28 and 30): a fixed 1.0 = one 30 Hz frame (`cutscene_fps = 60`
    replaces it by the measured length, `h_frame_clock`).
- `7100c1f72c` multiplies the step into five chained **time channels** (scales at +0x88 .. +0xcc, scaled steps stored at
  +0x94 ...): the slow motions / Witch Time / pause of the game read these.
- So gameplay already moves by a measured delta between 30 and 60 fps (a 25 ms frame = a step of 1.5); above 60 the floor
  of 1.0 makes the game run faster than real time (at 120 fps, twice as fast). This is the clock of Bayonetta 2
  (B2recomp: `0x7100dd1d60`, the same 1.0 / 2.0 bounds, five channels).

**What B2recomp's 120 fps beta needed** (its `scripts/fps_patch.py`, `lifter/fps_ticks.txt`, `lifter/fps_smooth.txt`;
v0.2.0 beta on 2026-10-17, v0.3.4 on 2026-10-23, about 26 commits): the clock always measures and its floor goes to
1/16; the present asks for interval 0 and a limiter paces; about 60 **per-frame counters** (player / enemy countdowns,
combo windows and the combo display, Witch Time's end, input windows after a press, stick-motion moves, enemy sound
timers) advance by the 60 Hz frames of game time due ("smart tick", per site and field address); 48 **smoothings**
x += k (T - x) take their closed form T + (x - T)(1 - k)^step; float timers keep their overshoot; cinematic shot / scene
holds keep their game time; the slow motion's end lands on the frame grid. The counters were found by a **census**:
per-site hit counts while the frame rate alternates 60 / 120, then read one by one.

**For B3** (what carries over, what does not):
- The clock floor, the present interval and the runtime's pacing at 120 (`frame_rate`, display.c `gfx_pace_rate`, from
  Recomps#1) carry over almost as they are: about a day.
- Every counter and smoothing address is B3's own (another binary): the census tooling (site counters in the lifted
  code, the alternating-rate run) has to be rebuilt here, and each site read. B2's patterns (the smoothing form, the
  overshoot form) can be searched for the same way in B3's lifted C. Expect a list of similar size (B3 has more
  systems: Demon Slaves, Viola, Jeanne's levels).
- Known frame-locked parts so far: the 30 fps-mode scenes (cutscenes and menus: their own step, handled); the
  frames -> seconds conversion by mode (`7100cb4f54`); the pad is read once a frame (input windows count frames:
  the B2 kind of fix). Physics and animation are not read yet: if they take the scaled step they follow; anything that
  counts frames does not.
- Performance: 120 fps needs 8.3 ms on the critical path. Today's late-frame analysis (Shibuya, Tokyo) has frames of
  16-20 ms of game + render work in heavy scenes (render thread 4-11 ms, the main thread's own work and its jobs, plus
  long frames in lifted code: the tree walk `71007e1fe4`); 120 would hold in light scenes only, 80-100 in fights.
- Risks: subtle gameplay timing (combos, dodge offset, Witch Time windows, enemy attack timers) that only shows in
  play; cutscene sync (handled for the 60 case); effects that count frames.
- Estimate: the uncapped clock and pacing about a day; a playable beta with the census and the first ~50 sites about a
  week of sessions (B2's took a week after its research); then fixes as play finds them.

## RomFS archives: the game's own Kraken decoder (2026-10-25)

`build/witch-time-game targets/exefs/main.elf --pkz-dump <folder | -> [filter...]` (cwd `build/<run>`, `WT_ROMFS` as for a
run; `runtime/pkz_dump.c`) writes every `.pkz` entry to `<folder>/<entry name>`: Kraken entries decompressed by main's
own Oodle decoder (lifted), stored ones copied. No window, no audio, no game code but the decoder (image, relocations and
import handlers only); 8 threads (`WT_PKZ_JOBS`); a filter keeps entries whose archive or entry name contains it; `-`
decodes without writing. Entry names are unique across the archives, so the folder mirrors the game's paths. Write it
under a git-ignored folder (`cache/pkz/`): the files are game assets.
- **All 72 archives**: 18,732 entries (11,760 Kraken, 6,972 stored), **0 failures**: every Kraken stream used exactly its
  stored size and gave exactly its recorded size (the former `pkz.py check targets/romfs --dump cache/pkz` agreed for every entry).
  25.8 GB: 10.7 s decode only, 45.4 s with the writes (8 threads on a 16-thread CPU, NVMe, the archives partly in the page cache).
- **Content**: every Kraken entry has its type's magic (6,408 `.dtt`, 2,986 `.dat`, 1,305 `.eff`, 262 `.evn` = `DAT`;
  161 `.bin` = `PTD`; `EPB`, `WME3`, `VAT`, `VFD`, `WTB`, `BXM`; the 327 `.wtp` are headerless texture data); all 15,526
  DAT containers of the dump parse with 130,976 members inside their files. `ui/ui_option.dat` (the Options screen) holds
  13 `option00xx.uid` layouts plus `option.wta` / `.uvd` (PlatinumGames UID, as guessed before); `Text/UI/
  core_pln_USen.bin` gives 4,672 readable strings.
- **Stored replacements**: a `Text_000.pkz` rebuilt with all 394 entries stored (`None`), in `mods/<name>/romfs/`, is read
  by the game; its changed copyright line showed on the title screen. So rebuilt archives need no Kraken encoder.
- **Mods folder on Linux**: it had the `''`-only cut fixed for graphics.ini in 028ea6d (`nnfs.c`: the folder and
  `mods.ini`), so mods worked only with `WT_MODS`; fixed, the default `mods/` beside `build/` is found.
  `controllers.ini` (nnpad_sdl.c) still has that cut.

## Keyboard and mouse (2026-10-25)

The keyboard + mouse path came from B2recomp (nnhid.c, nnpad_sdl.c, display.c) and was never used on Linux:
`config/controllers.ini` was not read (the program's folder was cut at '\' only, as graphics.ini), and its defaults were
Bayonetta 2's. Now:

- **controllers.ini is read on Linux** and reloaded within a second of being saved (rebinding while the game runs).
- **B3 defaults**, from the game's own Options -> Controller page (Type A; `/tmp/shots/kbm_game_controls_typeA.png`):
  left mouse = X punch, right mouse / Enter = A kick / confirm, Space / Backspace = B jump / back, E = Y shoot,
  left Shift = ZR dodge, Q / mouse 4 = ZL Demon Slave, R = L weapon switch, F = R stance change, middle mouse / V =
  right-stick press (camera / lock-on), C = left-stick press (waypoint), arrows or 1 2 3 / T = D-pad (Demon Select /
  taunt), Tab = Minus (subscreen), Esc = Plus (pause), WASD = left stick, I J K L = right stick, left Ctrl = WALK.
  One line per action in the file, two keys each at most; a key bound to two actions is named in the log.
- **Movement**: key diagonals have the length of a straight push; WALK holds the stick at 0.4 (below the stick-as-button
  bits at 0.5). **Camera**: mouse movement becomes right-stick deflection proportional to its speed (1200 counts/s =
  full, times `mouse_sensitivity`; `mouse_invert_y`). The pointer is captured (SDL relative mode) after a click into
  the window (at once when fullscreen); that click is not given to the game; Esc or losing the focus releases it.
- **Pad and keyboard together**: both drive player 1 at once (buttons add up; a pad's stick wins while it is off
  centre), so switching is just using the other device. The button prompts are chosen at start (`button_prompts`, below).
- **Button prompts** (`runtime/prompts.c`, no mod): all prompts come from one atlas (`core_sys_btn`, texture 0
  of `core.wtp` in `ui_core*.dtt`; ASTC 4x4, BC3 in `ui_core_gb`). `nvn_gl.c` `tex_initialize` paints over it in memory when
  the texture is made, in the texture's own format, and `prompts_tick` (player 1's pad poll) again when the choice changes,
  then `nvn_gl_memory_written` has the renderers read it again; only the two atlases known by hash are touched. `auto`
  follows the device player 1 used last (pad: a button or a stick past half; keyboard: a key, or the mouse past a quarter),
  a pad plugged in or out, and `controllers.ini` as it is saved. Each set is painted once (keyboard 18 ms) and kept. PlayStation / Xbox: face buttons, L / R / ZL / ZR, Plus / Minus (by position or by printed letter). Keyboard: the
  same cells and the stick cells show the keys bound in `[keyboard]` (first key of a binding; left, right, wheel mouse buttons as
  a mouse), from `auto` when there is no pad. The two grey four-dot pictures, the d-pad and the coloured action icons stay.
  A cell holds two pictures: alpha is the button, RGB its backing (the outline with its holes filled, 2 texels wider, white on black). The action guides (`hud011x`: "Climax", "Break Free") draw the cell a second time in black with `uibaseshader`'s vertex flag that takes alpha from (r + g + b) / 3, so an RGB of any other shape shows as a black shape behind the button: first the whole cell, then, after blackening the empty blocks, the blocks around the picture. Painted cells now hold the same backing. ASTC needs two weight planes for that (block mode 0x453, 3-bit weights); all 8,192 blocks of each set match ARM's decoder (`astccmp --file`). PlayStation prompts confirmed by the user in a fight on Linux (RADV).
  Checked in the running game (Options help screen, Vulkan): PlayStation, Xbox, by letter, keyboard with a rebound key, Switch
  untouched, all changed while it ran; keyboard <-> pad and hot-plug with a uinput Xbox pad under headless gamescope (the
  game takes input only with a focused window); `tests/prompts_test.c`. `button_prompts` is also a row of the PC settings panel and of the launcher's Settings; it is written to `controllers.ini`. `swap_confirm` is the same kind of key: menus confirm with B and go back with A, and their guides show it (`options_row.c` `menu_swap_tick`, `docs/RE_NOTES.md` section 8), for a pad only: on the keyboard Enter stays confirm. Checked in the running game on Linux with scripted input: Main Menu, Difficulty, the Naive Angel notice, on, off and on with the keyboard in use; X / Y and gameplay are not touched, by the reading of main; Windows not run. The launcher's footer uses the same drawings
  (`runtime/prompt_art.h`, `gen:glyph`) and the same rule: its fixed Enter / Esc, or A / B or Cross / Circle by the label on
  the button, which is how it takes confirm and back. Native ASTC is checked only through our decoder (desktop GPUs have none).
- **Linux key state** comes from the window's SDL events (window thread), not from SDL_GetKeyboardState on the game's
  thread; cleared when the focus goes.
- Fixed on the way: with a pad on the keyboard's player, a vertical-only mouse move was replaced by the pad's centred
  stick; an unmapped joystick's axis now drives a stick only after it was seen near the centre (this machine's
  Swiftpoint Z3 mouse receiver is a joystick with five axes resting at -32768: it was player 1 with both sticks held,
  which would override WASD and the mouse once the window had the focus).

Tests:

- `WT_PAD_SELFTEST=1 build/witch-time-game`: 61/61 (was 49), new: every default key gives exactly its Pro bit, WASD,
  diagonal, walk, mouse to right stick (both axes), pad + keyboard together, rebinding by editing the file while
  running, invert / sensitivity, the pinned-axis receiver.
- **Game runs with synthetic events** (`WT_KBM_SCRIPT`, below; muted, hidden window, a copy of the save, no other
  input): Enter through the title into Shibuya; capture click swallowed; run, diagonal, walk; camera turned and tilted by
  the mouse; punch, kick, shoot, jump, dodge, lock-on, taunt; Tab subscreen and Backspace back; Esc pause, arrows / S /
  Enter / Backspace through the pause menu and Options; X rebound to U in the run's file at 150 s: U punches, the left
  mouse no longer does. The log line of each step and the Npad state the game read match. 60 fps in the free roam of
  that run. Pictures: `/tmp/shots/kbm_*.png`.
- `WT_KBM_SCRIPT="<s>[+<dur>]:<input> ..."` (Linux, test only): pushes SDL key / mouse-button / motion events
  (`MOUSE1`, `W`, `M<dx>,<dy>` = motion every 10 ms, `P<x>,<y>` = the pointer to x, y of the window, once) into the
  window's queue, seconds from window creation; the window counts as focused, and the keyboard player's Npad state is
  logged at each change (`kbm: ...`). Key names as in controllers.ini (`ENTER`, `ESC`; `RETURN` / `ESCAPE` also accepted
  since the merge); a hold is `<s>+<dur>:<key>` (`62+2:W`), an unknown name is dropped with "kbm script: unknown input".

Not tested: a person at a real keyboard and mouse (camera feel, sensitivity default), Wayland / X11 pointer capture of a
visible window (the test window is hidden, so SDL's relative mode was not switched there), Windows (not built).

## Button rebinding (2026-11-01; issues 82 and 84)

The game's control types are replaced by a page of our own: every button of gameplay, for Bayonetta and for Viola, and the
game's prompts, its HUD and its picture of the pad follow. The game's findings are in docs/RE_NOTES.md section 9.

- **Where**: Options, Controller, the rows Controls (Bayonetta) and (Viola) are action rows now and open the panel's Controls
  page, for that character; PC Settings has a Controls row to the same page. Character, Preset (Game's setting, Type A .. D), and
  16 rows of actions (the game's wording; Viola's Block and Summon Cheshire). A on a row and the button to use, or
  left / right to go through them, which is the way without a pad. A button in use changes places with the row's, so a layout is
  always a permutation: no action is lost, and press and release stay true. `Face buttons` (layout: Position, Printed letter) is a
  new row of the first page: with `button_prompts`, that is issue 84.
- **controllers.ini**: `bind_bayonetta`, `bind_viola`: `game` or 16 Pro buttons in order. Anything else is reported and means
  `game`. With `game` nothing is translated and the save's control type stands, so nothing changes for a player who rebinds nothing.
- **How**: the bits are moved in the pads' copies that gameplay reads, just after the game makes them (`h_rebind_pads`,
  runtime/rebind.h), not in nn::hid: that would move the menus' buttons too, which is what issue 82 asks to be rid of. Masks were not
  rewritten either: gameplay has sites that test the copies directly. Pause and Subscreen are read from the menus' copy, so
  `h_rebind_menu_pads` moves those two bits there while gameplay has the pad. A layout counts from Type A: `h_control_type` answers 0.
- **Prompts**: `h_prompt_number` and `h_text_symbol` (the HUD's lists are text with button tags, rebuilt every frame). The atlas
  is not touched. The Controller page's picture is one drawing with labels from a table by type, so Type A's table is written from the
  layout (options_row.c `labels_tick`). Face buttons and the three d-pad buttons share a label there, so an action on one of those
  is told by the panel and not by the picture.
- **Keyboard**: while the keyboard is what player 1 used last the layouts do not count (`nnpad_bind_live`): `[keyboard]` is its binding.
- **Checked in the running game on Linux**, with a virtual Xbox 360 pad through uinput (hidden window, copies of the save and
  config, `WT_REBIND_LOG=1`: every press as the pad has it and as gameplay gets it):
  - all 16 buttons give the bits of rebind.h;
  - Options, Controls opens the page; a press rebinds Dodge to LB, the file is written, Custom shows, the picture says Dodge LB and
    Weapon Switch RT;
  - A and B swapped: gameplay gets them swapped and every menu keeps Confirm, from the title through a chapter's notices;
  - R and ZR swapped, on the spider ride: the HUD shows Dodge RB and Massive Web RT; Dodge on a stick press and Massive Web on Start
    show those pictures;
  - Plus and Y swapped: Start does not pause, Y's button does, and Start still closes the pause menu. That run found the Pause path:
    before `h_rebind_menu_pads` Start still paused;
  - a key press turns the layouts off, and keycaps show; a line with a button twice is reported and ignored; a file of an older
    version gets the key added.
  `tests/rebind_test.c` holds the four types against 0x7100b22ee0; `tests/pcmenu_selftest.c` the page.
- **Confirmed by the user on a real pad** (a Steam Controller on Linux, 2026-11-01).
- **Not checked**: Windows (not built; the new code has no system calls of its own); Viola in gameplay (her
  layout was set from the panel and written, but no save here reaches her, so her moves, her prompts and `Not used` for her
  L and d-pad, taken from the picture's labels, were not seen); actual dodges and jumps on a rebound button were seen only as the
  bits gameplay gets, except Pause, whose effect was seen.
- **Left as they are**: the sticks (move and camera stay); what a cutscene's skip reads (the menus' copy: the pad's own Plus); the
  game's Restore Defaults on that page resets the save's types and not a layout (the page's Preset does).

## PC settings menu (2026-10-25)

An in-game panel for the keys of graphics.ini, drawn by the runtime over the game (the feasibility study's path: plumbing,
then a runtime overlay, then a tie to the game's menus). `runtime/pcmenu.c`, `runtime/nvn_vk_pcmenu.c`.

- **Open / close**: F1, or Start + Select on player 1's controller (L3 + R3 until 2026-10-31, which the game uses; seen at the game's own pad reads, `pcmenu_gate` in nnhid.c);
  B / Escape / F1 close it. Up / down choose a row, left / right change it (they stop at the ends), A / Enter go round;
  the mouse points at a row and clicks the value ('‹' for the one before). While it is open the **game is held** with the
  console's suspension (`g_guest_frozen`; by the code its sound queue runs dry: not heard, the runs are muted), the render thread presents the
  game's last picture again with the panel about 60 times a second, and the game reads an idle pad, also after closing
  until the buttons that closed it are let go.
- **A new version's graphics.ini** (2026-10-31): an update leaves `config/` alone and the old launcher installs it, so the game does it at start (`gfx_ini_refresh`, gfx_options.c): a comment or key of the defaults missing from the file makes the file the defaults with the player's values, then the player's other keys, the old file in `graphics.ini.bak`. A file with every line is not touched; a file named by `WT_GRAPHICS_CONFIG` / `WT_CONFIG` (tests, benches) never. Filling in a key changes nothing: a file with no keys and the defaults load to the same options.
- **Rows and how they apply**: display mode and window size (live on Linux through the window thread; Windows: at the
  next start), vsync (live: the present mode is chosen again and the swapchain made again, display.c's pacing chosen
  again; a display lock is let go live, a new one is only taken if none was tried in this run), cutscene frame rate
  (live: `game_cutscene_fps`, exported by the game library), anisotropic filtering and texture detail (`texture_lod_bias`; live: the sampler table is
  flushed, new samplers from then on; the old ones are kept to shutdown because cached descriptor sets may name them), aspect
  (live), internal resolution (**saved, applies at the next start**, marked on the row and in the status line). A key set
  by an environment variable (`WT_WINDOW`, `WT_VK_PRESENT`, `WT_CUTSCENE_FPS`, `WT_VK_SCALE`,
  `WT_TEXTURE_LOD_BIAS`) or by `profile = low` is shown greyed with the reason. Hook for a frame-rate cap (a study so far,
  above): one more row (with `optional`, it appears only when graphics.ini has its key) plus its live field in
  `gfx_option_set`.
- **graphics.ini writer** (`gfx_option_set`, `gfx_ini_get` in gfx_options.c): every line with the key gets the new value,
  the rest of the file stays byte for byte (comments, unknown keys, CRLF or LF), a missing key is appended, written
  through `<file>.tmp` and a rename; then the file is read again into a copy and the live keys are taken over.
- **Look** (redrawn 2026-11-01, section "Launcher and in-game look" below; the pearl segment alone over the game since
  2026-11-02): the whole window, its ground drawn on the CPU into BGRA when something changes, composed on the GPU every frame with the turning wheel over it (`pcm_gpu_draw`,
  below) and copied over the presented picture (never into the game's textures); Marcellus and Source Sans 3 subsets
  (SIL OFL 1.1) through stb_truetype.
- **The foot hints follow the prompts** (2026-11-02, Kevin's report): "Change" and "Close" show the button that acts as
  Pro A and Pro B on player 1's pad as the Button prompts and Face buttons rows say (`hint_btn`: a letter in a disc for
  Switch and Xbox, by letter or by position; Circle / Cross shapes on PlayStation; ENT / ESC key caps on the keyboard),
  and change at once when either row changes; swap_confirm swaps them as before.
- **The game's own Options screen** (2026-10-26; since 2026-10-27 seven native rows above it, next section): **a "PC Settings" row** on its Screen Display page, before Restore
  Defaults, opens the panel (A / Enter on it; the game's confirm sound; the panel's own first press is ignored, so the A
  that opened it changes nothing). Found without a run: the Options labels are messages of `messmenu.mcd` (MCD, in
  `ui/ui_menu_<lang>.dat`, not the PTD tables), their hashes are immediates in main, and they led to the screen's 25 row
  objects and its page tables (docs/RE_NOTES.md section 8). How it is built:
  - **Lift** (`lifter/recomp_eh.py`, `_options_row_patches`): the 13 adrp + add pairs that read Screen Display's 7 row
    numbers read them from a page the runtime maps after main's image (0x7105a00000), and its two row counts say 8;
    28 words, each original checked against the binary. Re-lifting changes one C file.
  - **Runtime** (`runtime/options_row.c`, at boot): that page with the numbers 18 .. 23, 25, 24; row 25 is an object
    whose method table is the Brightness row's (an action row) with two host methods: the label (message
    MENU_OPTION_LIST_PC_SETTINGS) and "decide" (the game's own confirm test and sounds; opens the panel, returns "used").
  - **Text** (`tools/wtbuild/src/text.rs`, setup step `text`): the message added to all 13 `messmenu.mcd` (spelled with
    each file's own glyphs: PC Settings / PC-Einstellungen / Paramètres PC / Ajustes de PC / Impostazioni PC /
    Настройки ПК / PC設定 / PC设置 / PC 설정; the Chinese and Korean "PC" from the files' Latin font), served as
    `mods/witchtime-pc-settings/romfs/ui_000.pkz`: the dump's archive byte for byte plus the 13 entries appended stored
    and re-pointed (185 MB; made in 0.3 s here, through `witch-time-game --pkz-dump`). Without
    the mod the row is named with the pause menu's "Options" and the log says how to make the text.
  The pause menu's hint (below) stays: it names F1 / Start + Select, which work everywhere.
  Runs (muted, hidden window, copies of the save and graphics.ini; `/tmp/shots/optionshook_*`): r3 with the virtual pad
  (WT_SCRIPT) and r4 with keyboard events (WT_KBM_SCRIPT): pause -> Options -> R x3 -> Screen Display shows the
  row (`optionshook_row_screen_display_en.png`); down x6 and A / Enter open the panel over the game's Options
  (`optionshook_panel_from_row.png`); down moves the panel, not the game; B / Backspace closes it, up / down then move
  the game's cursor (to the greyed Naive Angel Mode and back), A / Enter opens it again, B / Esc closes it (Esc = Plus
  reached the game only as an idle pad: no pause toggle), B leaves Options and the pause menu
  (`optionshook_pad_close_and_leave.png`). GameData.dat is written when Options is left: byte for byte the original in
  both runs; graphics.ini unchanged (nothing changed in the panel). Not seen: r2, the same keyboard script as r4,
  lost the key-up of the Backspace that closed the panel (the host's key state kept B down until the next Backspace,
  so the game read an idle pad for 17 s); three more Backspace closes (r4, a title-screen run with every SDL key event
  logged) released normally. Cause not found; it was in the synthetic-event test path, a real key was not tried.
  Labels, with an empty save and WT_LANGUAGE (the language of a save wins over it; title -> OPTIONS): zh-Hans,
  zh-Hant, ko, ja, de, fr, es, it, ru all show their label in the list's font, the Latin "PC" of the Chinese and Korean
  files included (`optionshook_labels_cjk.png`, `optionshook_labels_eu_and_fallback.png`); with WT_MODS=none the row
  reads "Options". From the title's Options the panel opened and closed as in a chapter; there, reaching Naive Angel
  Mode first shows its explanation (the game's code does that for row 23 when it is not greyed), which takes the input
  until B. Not tested: a real pad or keyboard (synthetic input only), en-GB / nl / pt (their files were changed and
  read back, not shown), Windows (not built).
- **Cost while closed**: one key test and one mask test per pad read, one flag read per present; nothing drawn, no thread.
  Not measured with a benchmark run.
- **Tests**: `tests/pcmenu_selftest.c` (the ini rewrite byte for byte in CRLF and LF files, duplicate keys, an appended
  key, live vs next-start keys, the rows' values, left / right at the ends, the panel at 1280x720, 1920x1080, 3840x2160).
  Game runs (muted, hidden window, copies of the save and of graphics.ini): the panel opened on the title by
  `WT_PCMENU_OPEN` and by the F1 key of `WT_SCRIPT` ('p'), moved through with the scripted pad and keys; vsync and
  cutscene_fps changed live (the next cutscene ran at 60: "runs at 60 (cutscene_fps = 60)") and written to the file (only
  those lines differ); a resolution_scale change flagged "applies at the next start"; anisotropic filtering 8 -> 16 and
  texture detail 0 -> -0.5 made the samplers again (the line in graphics.ini changed, nothing else); at resolution_scale 2.4 in a 3840x2160 window (4800x2700 pixels at 125 %) the panel is sharp and
  WT_WINDOW greys the size row. Through the pause menu: the hint appeared when it opened; F1 opened the panel over the
  game's Options screen, the scripted down / right moved the panel and not the game's Options cursor, the window size
  change made the hidden window 2000x1125 pixels (1600x900 at 125 %), and after closing B left the game's Options and the
  pause menu as usual. Pictures: `/tmp/shots/pcmenu_*.png` (title at 1.0, 4K at 2.4 with the next-start flag, the
  virtual-pad steps, the pause hint and the panel over the game's Options).
- Test aids: `WT_PCMENU_SHOT=<folder>` (every panel change: the next presented picture, panel included, as
  `pcmenu_<n>.bmp`; the swapchain gets TRANSFER_SRC when the surface offers it), `WT_PCMENU_OPEN=<s>`,
  `WT_PCMENU_LOG=1`.
- **With the keyboard and mouse branch (after the merge, 2026-10-25)**: the merged run that "did not close on the second
  F1" had script steps the kbm parser rejected (`RETURN`, `ESCAPE`, `W+2`: "kbm script: unknown input"), so Escape was never
  sent: F1 at 45 s opened, F1 at 55 s closed, F1 at 58 s opened, and nothing closed it again. Fixed on the way: (1) the
  idle pad after a close is kept per player until that player has let go (it was cleared by player 1 alone, so a keyboard
  on another player holding Esc = Plus would have opened the game's pause menu); (2) L3 + R3 did not *close* the panel
  from the keyboard (C / V): the panel's reader had no stick-press bindings; (3) the pointer position comes from the
  window's motion events (as the kbm key state), so synthetic events can click panel rows. Runs `k1`, `k2` (muted, copy of
  the save, synthetic window events only): Enter into Shibuya; a click captures the mouse; F1 opens and the log says
  "display: mouse released"; arrows move; `P840,298` + `MOUSE1` clicks the cutscene row's value (30 -> 60, written); Esc
  closes, the game reads Plus (0x400) only as an idle pad until it is let go (no pause menu: the frames after are
  gameplay), W / D / the mouse reach the game at once (`kbm: player 1 ... left 0 32767`); F1 / F1, F1 / Backspace, C + V /
  C + V open and close; after a close the mouse is captured again by the next click (windowed; fullscreen at once), as
  after Escape in play. Pictures: `/tmp/shots/pcmenu_kbm_k1_sheet.png`, `pcmenu_kbm_k2_sheet.png`.
- **Not tested**: a person with a real pad, keyboard and mouse (the pointer path ran on synthetic events only; a real
  pad's Start + Select and B not tried, only their keyboard bindings), a visible window made fullscreen (a size change of the hidden
  Wayland window was confirmed by its new pixel size; fullscreen was not tried), Windows (not built), HDR output (the panel is
  copied into the 8-bit picture; shots are refused there). Movies keep their clock while the game is held, so a movie
  catches up (drops frames) after the panel closes.
- Found on the way: `WT_LPROF` crashed the run once (glibc's `backtrace()` in the sampler's signal handler, interrupting
  a RADV driver frame without unwind information); not changed here.

## PC settings as rows of the game's Options screen (2026-10-27)

Asked for: a "PC Settings" **tab** in the game's Options screen with native rows. Done instead: **the same rows on the
Screen Display page** (the fallback asked for when a tab is not possible), below the game's own six:

| row | values | applies |
|---|---|---|
| Internal Resolution | 900p / 1080p / 1440p / 2160p | at once (nvn_vk_draw.c rs_apply; "Resolution scale, live") |
| Cutscene Frame Rate | 30 / 60 | at once |
| Vertical Sync | Auto / Monitor (display) / Timer (clock) / ON / OFF | at once |
| Display Mode | Windowed / Borderless / Fullscreen | at once (Linux; Windows at the next start, as the panel) |
| Anisotropic Filtering | Default (the game's) / 2x / 4x / 8x / 16x | at once (new samplers) |
| Texture Detail | -1 / -0.5 / 0 / +0.5 / +1 | at once (new samplers) |
| Aspect Ratio | Original / Stretch (later Fixed / Unlocked, next start; "Aspect ratios" below) | at once |

then "PC Settings" (opens the panel, which keeps window size and the values these rows do not list; F1 / Start + Select as
before) and Restore Defaults. Left / right go round the values with the game's sounds and key repeat (the game's own list
row), X resets the row to its default (the game's own "Reset Option" guide); a value of graphics.ini that is none of a
row's shows "Custom" (an extra value until left / right leave it). The game's Restore Defaults does not touch these rows
(its reset is the game's own Screen Display settings).

**Why not a tab** (main, docs/RE_NOTES.md section 8): the tab strip is its own layout, `option0011.uid`, which places four
instances of the tab layout `option0012.uid` at fixed x positions (-250, -80, 80, 250; L / R icons at +-380), and its class
(screen + 0x1d0) embeds exactly four tab objects (0xb0 bytes each, screen + 0x270 .. 0x480) with every method unrolled
four times (load, attach to slots 3..6, update, draw, select animation). The screen keeps the category (0..3) at
+0x40c8 and branches on it in about 17 jump-table switches; it wraps it with `& ~3` arithmetic in 7 places, and keeps a
cursor per category in a 4-entry array (+0x40e0) followed by other fields. A fifth tab would need: an element added to
a UID layout whose element format is only partly known (variable-length elements, key frames and links by index), a
fifth tab object outside the screen with host versions of about ten tab-strip methods, a fifth case in about 17 switches whose
jump tables the lifter resolves from the binary at lift time (it cannot take a case the binary does not have), mod-5 in
7 places, and a fifth cursor slot. Each part is possible in principle; together they are a large change to code that
cannot be tested without the game running, and it was not attempted. The rows are what the tab would hold.

How it is built:
- **Lift** (`lifter/recomp_eh.py`, `_options_row_patches`, 46 words, each original checked): the 13 adrp + add pairs read
  Screen Display's row numbers from the runtime's page (0x7105a00000); the 9 adrp + add pairs that read the 25 row objects
  (0x7101c2b1a8) read a longer table on that page; the list build reads the page's row count from the page (+0xffc) and
  the cursor wrap reads the count the list widget was given (`ldr w21, [x20, #0x28bc]`) instead of the constant 7.
- **Runtime** (`runtime/options_row.c`): the page holds rows 18 .. 23, 26 .. 32, 25, 24 (15) and the table; rows 26 .. 32
  share one method table copied from the Display Language row (a list row; fill, decide, button guide, refresh and the
  reset button are the game's) with its seven value methods ours (+0x38 label, +0x40 get, +0x48 set, +0x50 bank, +0x58
  value text, +0x60 count, +0x68 reset). Values go through `pcmenu_key_get` / `pcmenu_key_set` (pcmenu.c, the panel's own
  `set_value`), so graphics.ini is written with its comments kept and the live keys applied exactly as from the panel.
  When the panel opened from the PC Settings row closes, that row calls the game's list build (as Restore Defaults does),
  so the rows show what was changed in the panel.
- **Text** (`tools/wtbuild/src/text.rs`, setup step `text`): 8 labels (MENU_OPTION_LIST_PC_*) into the 13 `messmenu.mcd`
  and 21 values (CORE_PC_*) into the 13 `messcore.mcd`: a list row names the bank of its values (+0x50: crc32("core") for
  the language names, crc32("menu") for the control types), and only messcore has all digits (messmenu lacks 0 and 5-9 in
  every language). ON / OFF are the game's own CORE_SETTING_003 / 004. Each text is spelled with the file's own glyphs,
  preferring the list font and the Latin font 7 (another font's glyph is drawn at its own size: a Japanese グ came out
  too large); where a file has not the glyphs of a word, another word or the English one (the Chinese / Korean / Japanese
  files: VSync, Anisotropic Filtering, Aspect Ratio, Windowed, Frameless, Fullscreen; zh-Hans / zh-Hant "画面大小
  (Restart)"). ui_000.pkz in the mod now replaces 26 entries (ui_menu and ui_core, 13 each), 203 MB.
- **Without the text mod**: the page is the 8 rows of before (PC Settings named "Options", opening the panel); the log says
  how to make the text.

Runs (muted, hidden window, copies of the save and of the user's graphics.ini; synthetic input; `/tmp/shots/optionstab_*`):
- **Pad, title Options** (`title_rows_top_en`, `title_rows_bottom_en`, `window_borderless_en`, `resolution_changed_en`):
  every row changed with right / left and written to graphics.ini (only its line differs): aspect keep -> stretch,
  texture -1 -> -0.5, anisotropic 16 -> Default ("game"), window windowed -> borderless (the hidden window became 2000 x
  1125 pixels, desktop fullscreen) -> windowed, vsync display -> clock (pacing changed at once), cutscene 60 -> 30 (the
  menu dropped to 30 fps at once), resolution 1.0 -> 1.2 -> 1.6 -> 1.2 -> 1.0 ("applies at the next start", then "applied"
  back at 1.0). A on a row does nothing; X on a changed row restores its default (`reset_aspect_en`: Stretch -> Original,
  -1 -> 0); 15 rows wrap top <-> bottom (`cursor_wrap_15rows`).
- **Keyboard** (WT_KBM_SCRIPT, `kbd_panel_from_row`, `kbd_after_panel_esc`): Enter / arrows / F (= R) to the page, Enter
  on PC Settings opens the panel, Up + Right there sets aspect = stretch, Esc closes and the Aspect Ratio row shows Stretch
  at once; Left / Right then change the rows. The keyboard's X is MOUSE1, whose first click captures the mouse: it did not
  reset (not looked into further).
- **Pause menu in chapter 1** (Continue from the save copy; `pause_rows_en`, `pause_rows2_en`, `pause_back_to_game`):
  aspect, texture, anisotropic, vsync, cutscene changed and written; leaving Options wrote GameData.dat (new time stamp),
  **byte for byte the original**; SlotData untouched; B closed the pause menu, gameplay went on.
- **Languages** (empty save + WT_LANGUAGE, `lang_*`): ja, zh-Hans, zh-Hant, ko, de, fr, es, it, ru show their labels and
  values in the list's font; es / ru / it / fr / ja / zh-Hans again after shortening the values that overflowed (es
  "Personalizado", "Predeterminado"; ru "По умолчанию" and its label; the German "Automatisch" is now "Auto"), de (Randlos)
  and ko (전체 화면) again on the last build. **nl**: the game's own Dutch core text is empty (its ON / OFF and button guides are blank) and its messcore has
  no digits, so our values are partly blank there too; pt has the same files (not run). WT_LANGUAGE=en-GB showed the
  Japanese text: the game's defaults for a new save send en-GB, fr-CA and es-419 to Japanese (table at 0x71018541e4), so
  since issue #160 `nn::oe::GetDesiredLanguage` answers en-US, fr and es for them (`runtime/language_pick.h`,
  `tests/language_test.c`; empty save through the launcher on Linux: en-GB English, fr-CA French, es-MX Spanish). The
  `_gb` layouts are no longer reached.
- Without the mod (WT_MODS=none, `nomod_8rows`): the 8 rows, cursor wrap over 8.
- Tests: pcmenu self-test (the new key API: set / get by key, a value none of the row's refused, the file's lines), pad
  self-test 61/61, lifter check 382 + 25 words and 200 words / 4,000 cases: all pass.

Not tested: a person with a real pad or keyboard; Windows (not built); en-GB, pt; Fullscreen chosen from the rows (only
borderless); a profile = low or environment-locked key from the rows (the set is refused and logged, the row keeps its
value; not seen in a run).

## The goo and the dispatcher (2026-11-01; issue 92)

The green liquid of a dying giant Homunculus: 8 blobs of one 1272-index mesh, each drawn in 3 passes (vertex programs #620, #89,
#3747), for 121 frames. Its skinning vertex stage has 98 dispatcher cases and up to about 120 jumps, each one a trip through
`while (pc) switch (pc)`. RX 9070 XT, RADV: 5.65 ms a frame in those draws, whatever the resolution and on screen or not; an
RX 6600 on Windows gave about 63 ms a frame more (12 fps).
- **Vertex stages with no step back** (every target is known: BRA, the tokens, the BRX tables) run their blocks once, in order:
  `do { if (pc != N) continue; ... } while (false);`, and a jump leaves its block. Vertex stages with a real loop keep the
  dispatcher, and so do fragment stages shorter than 16 blocks, whose quad reads depend on how a quad's lanes move through the blocks (issues 100 and 107 below).
- **`invariant gl_Position` is the rule now** (`WT_VK_INVARIANT=0` off). #3747 is a colour pass with a depth test for equality
  after depth pass #620. Without the dispatcher between the sums the two compiled differently and #3747 lost a third of its
  fragments (8,327,501 to 5,541,822). With it the count is exact.
- **Measuring**: GPU timestamps come out in order, so 8 overlapping draws look like one slow draw and seven near zero.
  "wait for the GPU" also rises with another game on the GPU. Speedups of other programs in first runs on a new cache did not
  repeat (the same shaders, 4.1 and 82.5 us): compare later runs only. #4943 / #4945 went from 51 to 98 us a draw in one run
  and not in another; not explained.
- Not run on AMD's Windows driver, NVIDIA or Intel.

## The golden light and the dispatcher (2026-11-01; issue 100)

The light at the end of the prologue's last cutscene is the boss's gold cracks: vs #7759 / fs #7760, about 55 frames. fs #7760 is a
16-step parallax march over texture 2's alpha with an early exit at each step: 31 dispatcher cases and 47 jumps. RX 9070 XT, RADV,
1.6: 11 ms of the frame, peak frame 28.5 ms of GPU time; an RX 6700 XT on Windows fell to 16.5 fps there.
- **Long fragment chains run in order** too: a fragment stage with no step back, no SHFL and 16 blocks or more, which on the dumped
  stages is that march's six variants (#7756 to #7766). Same 100 frames: GPU time 11.6 to 5.8 ms a frame, peak 28.5 to 8.8, #7760
  0.59 to 0.06 ns a fragment. Long `SHFL` chains run in order since issue 107 (below); shorter stages stay because a wider test (1,644 stages,
  410 with an implicit-derivative sample after a jump) showed no clear gain outside this family, and a regression could be anywhere.
- Confirmed by the reporter (RX 6700 XT, Windows) on test build 54be236: no slowdown left.
- Not run on NVIDIA or Intel; RADV's whole-quad mode can hide quad bugs those show. The picture was checked by
  eye on shots 28 frames apart, not frame for frame.

## The clouds of erasure and the dispatcher (2026-11-02; issue 107)

Stutter on an RX 580 with many clouds of erasure on screen, worse at a higher `resolution_scale`. Each frame with clouds draws
about 40 tessellated and plain draws into the 520x360 targets (scaled with the picture, `rs_extra_class`), all through one
fragment stage family (#5318 .. #5438: 3D-noise octaves with hand-made derivatives). The stage has no loop, but it is cut into
26 to 49 blocks at its quad SHFLs, so it ran in the wave-uniform dispatcher with a header snapshot on every trip.
- **Fix**: a fragment stage with a shuffle, no step back and 16 or more blocks runs its blocks in order, like the vertex stages
  of issue 92. The snapshot of a shuffle's block is taken just before that block, between two blocks, where every lane of every
  quad is present; a lane that jumped ahead or reached EXIT waits there with its registers as they were, as in the dispatcher.
  Exactly these 64 of the 7,896 dumped stages change; all compile. Shorter shuffle stages keep the dispatcher.
- **Measured** (Linux, RX 9070 XT, RADV; Chapter 2's start from the issue-72 save, cutscene then the fight pattern, 230 s, A/B
  on caches of their own):

  | scale | cloud fragments | cloud draws, GPU | ns a fragment | longest draw | GPU ms in cloud frames, mean / p99 | whole run, mean / p99 |
  |---|---|---|---|---|---|---|
  | 1.2, main | 1,940 M | 10,809 ms | 5.57 | 1,681 us | 4.20 / 7.82 | 3.00 / 5.75 |
  | 1.2, fix | 1,830 M | 254 ms | 0.14 | 74 us | 1.97 / 3.03 | 2.04 / 3.78 |
  | 2.4, main | 7,321 M | 32,456 ms | 4.43 | 5,427 us | 12.07 / 18.76 | 7.45 / 16.93 |
  | 2.4, fix | 7,329 M | 954 ms | 0.13 | 258 us | 5.19 / 8.51 | 4.63 / 7.70 |

  Cloud frames: 0.3 M cloud fragments or more at 1.2, 1.2 M at 2.4. Other programs unchanged (#33 / #1710 2,031 and 2,058 ms,
  #1671 / #1693 1,868 and 1,860 ms). The clouds also draw at the title and in cutscenes, so the whole run gains.
- **Picture**: the cutscene's frames 3180 to 3410 shot every frame on both builds and on main twice: main against the fix differs
  as much as main against main (MAE 110-155 of 65535, the same pixels), so the clouds match within the game's own variation. The
  cloud wall in the fight looks the same. 0 Vulkan call failures.
- Not run on AMD's Windows driver (the reporter's), NVIDIA or Intel. `WT_VK_CLOUD_SCALE` still caps the clouds' scale.
## Launcher and in-game look (2026-11-01)

One design language for the launcher's screens, the PC settings panel and the first-start screen, from the project's
design page (project files, `design/witch-time-design-language.html`; mockups `design/mockup-*.png`): a pearl ground
(#EEEDF2) crossed by an indigo panel (#1B1628) on a 22 degree diagonal with a stripe of runic text on its edge, lists as
a staircase of dark bars stepping down the diagonal, the chosen one a violet to lavender to white glow with a » chevron,
a clockwork wheel with ornate hands at the local time, a dark message band (one sentence) and a pale hint band (dark
glyph discs) at the bottom. Marcellus for titles, Source Sans 3 Regular and SemiBold for the rest; red (#E5334B) marks
a value saved for the next start. The launcher never names the game: the code name and the logo.

- **Launcher** (`launcher/ui/witch.rcss`, `launcher.rml`): every screen is absolute boxes on a 1280 x 800 dp stage. The
  diagonal is `transform: skewX(22deg)` on plain boxes; gradients come from a new `gen:grad` texture (up to four
  stops, `ui_render.cpp`); the pictures are 8-bit BMPs in `launcher/ui/img` whose palette carries alpha (premultiplied)
  in its reserved byte, read by `ui_render.cpp` itself (SDL3_image stays shimmed away): the wheel in seven layers
  (two rings, the base, three gears, the hub; light and dark sets) that turn as the design page's SVG does (rings 120 s
  and 160 s the other way, gears 10 s and 14 s; `@keyframes spin`), the two hands, the runic stripe, the logo. The title menu is a `data-for` staircase (`menu` in the model, `menu(i)` the action); the
  build screen's log hides behind Show log; the settings are the panel's own rows, names and values (`setting_defs()`
  in `app.cpp`, in the panel's order: Window size, Frame rate and Texture detail are new there; Face buttons is left out, it stays auto) under three
  tabs, Display, Graphics and Controls (Q / E or the shoulder buttons, `tab` in the model), each a staircase of rows. On
  the text-heavy screens (dump, mods, settings) the wheel is small, at the bottom left on the indigo side. Fonts:
  Marcellus, Source Sans 3 (`launcher/ui/fonts`, OFL); Cinzel, Cormorant and Barlow are gone.
- **PC settings panel** (`runtime/pcmenu.c`): the same stage over the whole window (the held frame is covered; the
  copy is opaque), the light wheel behind the rows, a lilac one on the indigo side, the rows stepping down the
  diagonal at both ends as the launcher's do, 460 wide, the last ending 50 from the edge (the Controls page's 18 rows
  with a smaller step and row), the chosen row's help in the message band, the state ("Saved; applies at the next
  start", a lock) under the rows. The pictures are `runtime/art/*.bmp` `#embed`ded; fonts
  `runtime/third_party/*-pcmenu.ttf` (subsets, OFL). The mouse map (`g_row_y`, `g_val_x0/1[]` a row, `g_close_r`)
  follows. The pause-menu hint is a slanted indigo bar.
- **The wheels turn** in the panel and on the first-start screen as in the launcher (2026-11-01): the wheel art is the
  launcher's seven layers a set (`runtime/art/wheel-*.bmp`, `wheel-dark-*.bmp`, 5.6 MB embedded). A frame is the
  cached ground (`g_pbg`: ground, wedge, the wheels' bases) plus the rings and gears turned by the clock
  (`wheel_turn`, `anim_t`: `GetTickCount64`), the hubs and then the words composed over it from two layers drawn once
  a version (`Layer`, `g_to`: `put` and `put_pre` draw into a layer while it is set).
- **Composed on the GPU** (same day, Kevin's ask): `nvn_vk_pcmenu.c` `pcm_gpu_draw`. The CPU draws only what changes
  with a version: the ground (`g_pbg`, and the first-start screen's numbers over `g_bg`) and the two layers; they go
  up in one staging buffer (`pcm_upload`, ground + hubs + words). Every present copies the ground into a canvas of
  the panel's size (B8G8R8A8_UNORM, so the stored values are the CPU's, whatever the swapchain's colour class), draws
  the moving parts over it as turned textured quads (`pcmenu_gpu_scene`: `art_blit` lists a `PcmSprite` instead of
  drawing while `g_emit` is set, so the geometry is the CPU path's own: the same box, turn, opacity, ring annulus
  and slanted clip; the hubs and the words are two more quads between them), then copies the canvas over the picture
  at the panel's place. One pipeline (dynamic rendering, premultiplied blend ONE / ONE_MINUS_SRC_ALPHA as `put_pre`,
  push constants, a linear sampler with a transparent border; the annulus and the clip edge are `discard`s in the
  fragment stage), 16 art textures uploaded once from `pcmenu_art_pixels` (the staging buffer freed at the next
  job), the layer textures made again at a new size (`pcm_gpu_canvas`, after `cmd_wait_all`). `pcmenu_gpu(1)` stops
  the CPU path's 30 frames a second; `WT_PCMENU_CPU=1` keeps it, and so does a refused pipeline (the log says which).
  The hint is still copied as drawn. Verified on lavapipe (`tests/pcmenu_vk/`: the GPU picture against the CPU
  composition at the same clock, under the Khronos validation layer, no messages): mean channel difference 0.05-0.07
  at 720p, 1080p and 800x450, the worst pixels at ring edges; the first-start screen the same (its only difference
  the minute hand, when the second changed between the two renders). Not run on a real GPU yet.
- The CPU path, kept as the fallback: `pcmenu_panel_size` draws a new frame up to 30 times a second, never more often
  than a frame takes (`took`), so the thread keeps half a core at most; `pcm_upload` copies it each time. Measured
  with the harness at -O2 on the cloud machine: 18 ms a frame at 1280x720, 36 ms at 1080p, 132 ms at 4K (about 8
  frames a second there; the rings move 3 degrees a second, the gears 36 and 26, so 4K is visibly stepped). The
  rotating blits sample only the ring's annulus and a picture's alpha box.
- **First-start screen** (`pcmenu.c` `draw_prep`, `nvn_vk_draw.c` `prep_first_hold`): drawn by the panel's code too
  (title, the count of stages and the time left, a slanted bar, the dark wheel, a pearl panel at the right), copied over
  the held frame by `pcm_vk_draw` (`pcmenu_kind` 3; `nvn_vk_pcmenu.c` `pcm_upload`). The 5 x 7 dot font and the
  overlay's own bar are gone from that screen (the thin loading bar of normal frames stays).
- **Motion** (2026-11-02, Kevin's ask; no footage of the game's menus could be viewed from here, so the timing follows
  Platinum's own account of their UI: a quick start and a gradual finish, `ease_out` = 1 - (1 - t)^3). The launcher:
  a screen's rows step in from the left one after another (`stair-in`, 0.35 s + 0.08 s a row; a section tab replays
  them with its own keyframe name), the halo and the settings' chevrons grow in, a changed value nudges toward the
  arrow pressed (`SettingRow::bump`), and a leaving screen steps out the other way in 0.22 s before the next shows
  (`App::go` sets `leaving` and `next_screen`; `tick` changes the screen). RmlUi inherits opacity rather than
  compositing it, so things that must vanish scale to nothing instead; `WITCHTIME_SLOWMO=<n>` slows every animation
  for a look. The in-game panel (GPU path only): `pcmenu_gpu_scene` draws the panel as sprites over the game's
  picture; while it opens (`PCM_OPEN_MS` 320) the segment slides in from the right with its wheel and the rows step in
  from the right one after another (40 ms apart at most, 0.24 s each); closing (`PCM_CLOSE_MS` 220) slides it out
  together, and the game goes on when it has. `pcm_gpu_draw` copies the picture under the panel into the canvas (the
  swapchain image when it has TRANSFER_SRC, else the HDR picture, which now has it) before the sprites, with the ground
  as one more texture; a second slanted edge and a band of rows are more `discard`s of the fragment stage. A slanted
  edge may start outside the window (`NOCLIP` is the sentinel for none, not a negative edge). The CPU fallback has no
  motion: the panel just waits the closing's time. Verified on lavapipe only (`tests/pcmenu_vk/vktest.c`'s fourth
  argument renders a moment of the opening or the closing over a gradient); not seen at speed on a real GPU.
- **The segment alone** (2026-11-02, Kevin's ask): the in-game panel lost its indigo wedge, the dark wheel and the title
  on it. It is the pearl segment right of the runic stripe, over the game's picture, which shows left of the edge
  (`pcmenu_gpu_scene` clips the ground at the edge whenever the renderer can start from the picture, so the steady panel
  is drawn over it every frame too); the title and its line sit at the top of the segment by the stripe, the rows start
  a little lower, the help wraps to two lines when the band right of the stripe is too narrow for one, and "F1 · Start +
  Select" moved right of the edge. The CPU fallback and a swapchain without TRANSFER_SRC cannot show the game there and
  fill the left with flat indigo (`stage_edge`). The first-start screen keeps its own stage.
- **Checked**: the launcher built here (SDL3 3.4.18 from conda-forge, RmlUi 6.3) and all five screens screenshotted
  under Xvfb with lavapipe; the panel, the Controls page, the hint and the first-start screen rendered through a
  stand-alone harness that includes `pcmenu.c` (1280 x 720 and 1920 x 1080). Not run: the game itself (no dump here),
  so the panel over a real frame, its input, and the first-start screen's timing are untested; a Windows build.

## Discord Rich Presence (2026-11-01)

`runtime/presence.c` shows the chapter, verse, difficulty, area and time in the chapter on a running Discord client, over its local IPC
(a socket on Linux, the Flatpak and Snap clients included; a named pipe on Windows). `presence_tick` in `game_shim_eh.c` publishes the state on
the game's thread once every 64 frames; a thread of its own sends a change at most every 5 s, so Discord can't touch a frame.
`discord_presence = off` in `graphics.ini` stops it. docs/RE_NOTES.md section 10 has where each value comes from.

The images are the game's, so the repo holds only their keys: they are uploaded to the Discord app, and a key the app lacks is dropped by
Discord without an error.

- **Measured** (Linux, scripted runs of the prologue and chapter 2, a stand-in for Discord logging every frame): the HUD's Verse 1 and 2 through a
  story scene in the fight, the results and the next phase; Expert and Standard from two saves; "Tokyo: Shinjuku" against the banner on screen; the
  area held across phases. On the real client: the card shown, one connection held for a 130 s run, and every activity accepted.
- **Time** is wall clock from entering the chapter, not the game's Play Time: it runs on through a pause.
- **Area** is blank after Continue until the next banner: a phase does not always raise one.
- **Not tested**: Windows was compiled, not run. Chapters other than those two come from the game's tables.

## Performance report (2026-10-26)

**Method**: `wtbuild perf-bench run <scene> <scale>` then `wtbuild perf-bench report <run dirs>` (`tools/wtbuild/src/perf_bench.rs`). Each run: a copy of the save, audio
muted, the committed `config/graphics.ini` (`WT_GRAPHICS_CONFIG`), output 1600x900 / 2560x1440 / 3840x2160 for scale
1.0 / 1.6 / 2.4. Frame times from `WT_FRAMETIMES`, GPU ms per frame from Vulkan timestamps (`WT_VK_GPUTIME=1` +
`WT_GPUTIMES`: the spans of the frame's command buffers), CPU ms a frame per thread from `/proc/self/task/*/stat` (the
`nvn_perf` lines, now on Linux too). The run records the load average and GPU busy % before and after and the other
processes with the GPU open (`meta.json`). It holds `/tmp/b3_bench.lock` while it runs and waits while someone
else's lock or any "Witch Time" process is there. Scenes: *shibuya* (the street fight, fight pattern from 40 s, 330 s),
*tokyo* (the earlier save, the heavy scenes, 700 s), *cutscene* (the Shibuya cutscene at `cutscene_fps = 60`, the frames
between the game's two frame-rate mode changes around it).

**Baseline** (build of c6b7bec + the tooling; load 2.5-6.6 from other programs, outside GPU busy 15-20 % at idle):

| run | frames | avg fps | 1 %-low | p50 / p90 / p99 ms | over 50 / 100 ms | at 60 | GPU ms p50 / p90 / p99 | CPU ms/frame: main, render, MainJob1, MainDraw | load before→after |
|---|---|---|---|---|---|---|---|---|---|
| shibuya 1.0 | 16801 | 59.5 | 43.8 | 16.7 / 18.2 / 22.8 | 0 / 0 | 92.6 % | 2.3 / 2.8 / 3.2 | 5.1, 5.8, 4.3, 1.9 | 4.5→4.3 |
| shibuya 1.6 | 16772 | 59.5 | 42.5 | 16.7 / 17.7 / 23.5 | 0 / 0 | 94.7 % | 3.8 / 4.4 / 4.8 | 4.2, 5.3, 3.7, 1.6 | 6.6→3.9 |
| shibuya 2.4 | 16912 | 59.7 | 48.7 | 16.7 / 17.3 / 20.5 | 0 / 0 | 96.9 % | 6.3 / 7.4 / 8.0 | 3.5, 4.8, 3.1, 1.3 | 3.4→3.5 |
| tokyo 1.0 | 17330 | 58.5 | 20.6 | 16.7 / 18.5 / 48.5 | 103 / 2 | 90.5 % | 1.8 / 2.6 / 2.8 | 6.3, 6.5, 5.1, 2.3 | 5.4→6.6 |
| tokyo 1.6 | 17840 | 59.5 | 53.0 | 16.7 / 17.6 / 18.9 | 8 / 0 | 98.6 % | 3.2 / 4.1 / 4.5 | 4.0, 5.0, 3.6, 1.4 | 2.7→3.4 |
| tokyo 2.4 | 17939 | 59.9 | 54.5 | 16.7 / 17.4 / 18.3 | 7 / 0 | 99.3 % | 5.9 / 7.1 / 8.0 | 3.4, 4.6, 3.3, 1.2 | 2.5→3.2 |
| cutscene 1.0 | 5415 | 58.9 | 38.0 | 16.7 / 18.8 / 26.3 | 1 / 1 | 87.3 % | 2.6 / 3.6 / 5.2 | 6.5, 5.3, 4.0, 1.9 | 4.3→5.4 |
| cutscene 1.6 | 5471 | 59.5 | 47.9 | 16.7 / 17.4 / 20.9 | 1 / 1 | 97.2 % | 4.9 / 7.0 / 9.0 | 4.1, 4.3, 2.8, 1.2 | 3.9→2.7 |
| cutscene 2.4 | 5248 | 57.1 | 40.3 | 16.8 / 20.5 / 24.8 | 2 / 1 | 82.6 % | 9.0 / 12.9 / 16.3 | 4.1, 4.3, 2.8, 1.2 | 3.5→2.5 |

**Read it with the load**: these ran one after another, not interleaved; CPU ms per thread changes about 2x between load
2.5 and 6 (the same scene), so the scale columns are not comparable with each other (the 1.0 runs had the most load).
Only interleaved A/B runs below are used for before/after.

**Bound**: every scene is CPU-side: no thread is near 16.7 ms a frame, and the late frames come from the chain main →
jobs → render → present (waits, not one busy thread). The GPU is under 8 ms at p99 everywhere except the 4K cutscene,
where 35 % of the late frames have over 14 ms of GPU: that one is partly GPU-bound.

**The items**:
- **4K cutscene at 57 fps**: GPU 7.45 ms a frame on average (`WT_VK_PASSTIME=1`): the depth prepass at 2458x1382 31 %,
  the 3456x1958 scene 29 %, a 2458x1382 R11G11B10 pass 12 % (cutscenes render from a 1024x576 base). New test aid
  `WT_VK_PASSWHY=1`: render passes are ended by `OP_TARGETS` in the replay (919k) and by `resolve_sampler` (391k).
  Keeping the pass open when the same targets are set again was tried and measured interleaved (2 + 2 runs): GPU p50 9.0
  ms either way, 84.5 / 84.2 % at 60 without against 83.5 / 83.7 % with (only 34k such repeats); not kept. Not fixed:
  with 15-20 % of the GPU busy from other programs, GPU p99 is about 16 ms. Scale 1.6 holds 97 % at 60.
- **The 80-105 ms frame** is the first frame after the game-over screen ("The Witch Hunts Are Over": the scripted fight
  dies about every 37 s, the checkpoint retry reloads the area, and the tessellated scenery comes back with it). GPU time
  of that frame is a normal 2.4 ms. The sampler (`WT_LPROF`) showed threads waiting on the `nobj` table lock (the main
  and job threads querying buffers and pools from `7100d0b7d0`), the jobs join and sync waits. **Lock-free `nobj`
  lookups** (`nvn_stub.c`): those frames 80-86 ms against 88-92 before (Tokyo 1.0; the lockfree run 59.8 fps, 99.5 % at
  60 at load 3). The rest is the game's own reload work in lifted code.
- **Trains of 45-50 ms** in `f_71007e1fe4` (a quadtree walk: four recursive calls, `h_morton_split32`, three other game
  functions, about 400 instructions of float code). A temporary timing wrapper: 60.6 ms per outer call with the register
  cache for that function (67 calls), 43.3 ms without (23 calls); the inputs differ, no clear gain, not kept. A host
  version was not written (large, much float state; hours and a correctness risk). The trains come at random (6, 15 and
  49 frames over 50 ms in three Tokyo 1.0 runs of the same build). Not fixed.
- No other hot spot stood out: render(host) is the top thread at 4.3-6.5 ms a frame.

## The render thread and the GPU overlap (2026-11-05; issues 132, 133)

Issue 133 (Windows, i9-10850K, RTX 5080, 3440x1440): 35 fps in Shibuya with CPU 19 % and GPU 51 %; issue 132: the Steam
Deck cannot hold 60. **Cause**: the render thread and the GPU took turns. Wherever the replay needed a result the GPU
writes, it submitted and waited for everything queued, the previous frame included, so a frame cost the replay's CPU time
plus the GPU's time and neither looked busy. Measured on the RX 9070 XT at the issue's shape (aspect 21:9, resolution
scale 2.4, the GPU at 11-15 ms a frame): frames whose GPU work took 14-16 ms were 98 % late, at 20 ms on average, though the
GPU alone fits in 16.7 ms. The sampler (`WT_LPROF`, waits sorted by caller) had the render thread 57 % in `slot_wait`: 33 %
in the batch-start `cb_sync`, 24 % in `indirect_src`. A Windows RX 6700 XT player's log shows the same (12.9 ms of GPU
wait in a 17.4 ms replay at 48 fps).

What B3 does in a Shibuya frame (`WT_VK_DRAWLOG` with the new DLI / DLG lines): the CPU clears a 0x14000-byte count
buffer (two, used in turn); the depth prepass's fragment shaders store each object's visibility into it; about 370
objects are then drawn as `nvnCommandBufferMultiDrawElementsIndirect` with `maxDrawCount` 1, the record in guest memory
and the count (0 or 1) in that buffer; about 240 of them are visible. Some frames add a particle draw whose record a
dispatch writes (4 indices, 5,000-10,000 instances, from a 120,000-byte index buffer). At the start of each frame the
exposure that the last frame's luminance pass stored (32 bytes) is copied into four views' constants with
`nvnCommandBufferCopyBufferToBuffer`.

The changes (`runtime/nvn_vk_draw.c`, one switch each, all on by default):
- **Counts and records read by the GPU** (`gpucount_on`, `WT_VK_GPUCOUNT=0` to turn off): a count that compute or a
  storing draw wrote in this batch is read by `vkCmdDraw(Indexed)IndirectCount` from the compute buffer (the one record goes
  to the ring, first index rebased); records written in this batch are read by `vkCmdDraw(Indexed)Indirect(Count)` straight
  from the buffer, with the whole NVN buffer from the index address on bound as the index range (`index_extent`, a locked
  walk of the object table in `nvn_stub.c`, kept and checked lock-free). A culled object now costs its draw preparation
  instead of a wait. Needs `drawIndirectCount` and `drawIndirectFirstInstance`; otherwise the CPU path as before.
- **No wait at the batch start** (`cb_defer_pending`, `WT_VK_CBLAZY=0`): in keep mode the last batch's compute buffers
  become kept without waiting (counted as written: a landing merges the bytes compute changed); a new `wserial` (the
  submission of a buffer's last write) is what a CPU read of a kept buffer waits for (`cb_kept_ready`); the next command
  buffer starts with a full barrier.
- **Overwritten kept buffers dropped** (`cb_kept_discard`, `WT_VK_CBDISCARD=0`): a clear, update or copy that covers a kept
  buffer whole drops it instead of landing it (the count buffer's clear waited for the GPU that way).
- **Buffer copies on the GPU** (`gcp_*`, `WT_VK_GPUCOPY=0`): a copy from a kept buffer the GPU has not finished (at most 4 KB)
  is an overlay; every ring copy of the destination is patched by `vkCmdCopyBuffer` in the slot's `up` command buffer, and
  the bytes reach guest memory when the source's writer has finished (oldest first, merged with a snapshot). Other CPU reads
  and writes of the destination, changes of the source and the game's fence waits settle first.

| Shibuya, 200 s | fps | at 60 | 1 %-low | p90 / p99 ms | GPU ms p50 | render: replay / GPU wait ms a frame |
|---|---|---|---|---|---|---|
| 21:9, scale 2.4, all off | 57.5-57.7 | 82.0-82.5 % | 44.5-44.8 | 19.8 / 22.4 | 11.9 | 7.5 / 6.9 |
| 21:9, scale 2.4, all on | 60.0 | 98.4-98.8 % | 51.2-52.8 | 16.8 / 18.9 | 10.7-11.9 | 3.5 / 0.04 |
| 16:9, scale 1.0, all off | 60.0 | 99.2-99.3 % | 55.5-56.2 | 16.9 / 17.8-18.0 | 3.2 | 4.4-4.5 / 1.6 |
| 16:9, scale 1.0, all on | 60.0 | 98.7-98.8 % | 53.2 | 16.8 / 18.8 | 3.2 | 3.3 / 0.01-0.02 |

(Two runs each way at each setting, interleaved. In the 2.4 runs with the change, frames with 14-16 ms of GPU work are
5-7 % late against 98 %. At 1.0, where this PC is far from either limit, the render thread's CPU time rises 0.3-0.4 ms a
frame (draw preparation of the culled objects) while its wall time falls 1.1-1.2 ms; slightly more frames end at 18.5-20 ms
(p99 18.8 against 17.8-18.0, the "at 60" share 0.5 points lower), fewer go past that (p99.9 20.6-20.8 against
22.3-22.7 ms).) Vulkan validation with synchronization validation (`WT_VK_VALIDATE=1
VK_LAYER_VALIDATE_SYNC=1`, 150 s of Shibuya): no hazard and no error from these paths. Screenshots every 600 frames,
switched on and off: nothing missing, exposure the same (the runs drift apart in game time, so compared by eye).

Tried and not kept: `WT_VK_SPLIT=200 / 60` (submissions at pass changes): +1 fps, frames with 14-16 ms of GPU still 93-95 %
late. The batch-start part alone: no gain, the wait moved to the count buffer's clear and then to the exposure copy (the
sampler showed it each time); only all four parts together free the render thread.

Not checked: Windows, NVIDIA and AMD's Windows driver (the paths were run on RADV only), the Steam Deck, other chapters
(indirect dispatches whose group counts compute writes still wait on the CPU; none in Shibuya), the OpenGL renderer (unchanged).

**Counts with more than one record (2026-11-09; issues 120, 156).** The demon rides, cutscenes and the title draw a few
objects with `maxDrawCount` 2: the count is compute's, the records are the game's. Those counts were still read on the CPU,
so in every such frame the render thread submitted and waited for the GPU once (`indirect_src` from the count read, found
with a test-only histogram of `slot_wait` callers), and the frame again cost replay plus GPU: a Windows RX 6600 log has the
Chapter 1 Gomorrah ride at 16 fps with 39 ms of GPU wait in a 54.5 ms replay. Now the GPU reads those counts too
(`recs_bounded`: every record's indices must lie inside the index buffer, since the ones past the count are bound though
never drawn; `WT_VK_GPUCOUNT_MULTI=0`: off). Measured in that ride on the RX 9070 XT at 1080p, scale 1.0: wait for the GPU
3.7-6.7 ms to 0.2-0.3 ms a frame, batch replay 19-25 ms to 15.6-16.9 ms, 4.1 to 3.1 submissions, GPU time the same
(4.4-7.8 ms). Not measured: Windows, AMD's and NVIDIA's own drivers, and whether the reporters' GPUs alone fit the frame.

## Cutscenes on a slow disk (2026-11-05; issue 136)

Issue 136 (Windows, RX 6600, v0.7.0): a cutscene's first viewing freezes for 1-4 s, less the second time. **Cause**: the
game was installed on a slow disk, most likely a hard disk. Its pack reads took 25 ms per texture, where other logs show
0.2-3 ms, and the pipeline cache loaded at 146 MB/s. In each of the three watchdog reports the whole process used under
100 ms of CPU in 300 ms. The game's main thread sat at the first instruction of one lifted function after another, so
Windows was paging `game.dll`'s code in from that disk. Meanwhile, the three texture-prep workers were reading the BC7 pack
and FileContentsRead (the game's only reader) its archives.

**Change**: after the guest image is read, `runtime_init` starts a thread (`hostrt.c` `code_prefetch`) that reads the whole
game library in large I/O: `PrefetchVirtualMemory` over `game.dll`'s image, or `posix_fadvise(WILLNEED)` over `game.so` in
128 KB steps (one call reads at most the readahead size). The thread then touches one byte of every page of the lifted
functions and logs `code prefetch: ... s`, which measures the disk. `WT_CODE_PREFETCH=0`: off. The cost is about 150 MB
read once at start and about 119 MB of code resident.

**Emulated** on Linux:
- Setup: game files dropped from the page cache, and the game's reads capped at 150 MB/s and 300 IOPS (a user scope's
  `io.max`) from 24 s. The route is the issue-72 save's Chapter 2 start, an 83 s real-time cutscene that starts at 38 s
  when warm.
- Windows-style faults: page by page (`MADV_RANDOM`), locked once read (`MLOCK_ONFAULT`). Without the lock, MGLRU's
  reclaim under memory pressure faults the same pages again.
- The prefetch: samples of the game's threads waiting on a fault in `game.so` went 4-9 -> 0-1. Frame times did not
  change; this model charges a request, not a seek.
- The pack: the three workers read about 4.5 GB, the game's own loader 1.2 GB. With `bc7_textures = off` the cutscene
  started at 58 s (three runs) instead of 114-149 s. Two of those runs had no frame over 160 ms, and the third had two
  over 1 s (worst 1.4 s). With the pack, up to 9 frames went over 1 s, the worst 4.7 s.
- `WT_VK_RS_PREP=0`: no measurable difference.

On a hard disk `bc7_textures = off` is therefore the faster choice. The pack stays the default.

**The reporter's hard disk** (2026-11-05, a version before v0.8.1: no `code prefetch` line): with `bc7_textures = off` the log has no
watchdog report (three before), and the worst frame after loading is 0.8 s, at the cutscene's start (before: 3.7, 1.8 and
2.5 s). The three decode threads kept up on their 4-core i3-12100F: 17 of 3500 textures were not ready at their use.

**Decision**: the game does not turn the pack off by itself. A drive's seek-penalty flag is cheap to read, but USB
enclosures and RAID report it wrongly, and timing the reads depends on what the page cache holds, so the picture would
change from run to run. What off costs depends on the CPU, and this is one machine. Instead the launcher's Converted
textures row has **Turn off / Turn on**, which only writes `bc7_textures` and keeps the folder, and its note names hard
disks. Checked headless on Linux: the button flips the key both ways and the row keeps the focus.

Not checked: Windows (the prefetch only compiled there, and Wine reads the whole image at load anyway), the prefetch on
a real hard disk.

## Video memory over a session (2026-11-10; issue 153)

A GTX 1050 Ti (4 GB) log showed "device memory" climbing from 1425 MB to 3810 MB in 37 minutes. Measured on an RX 9070 XT
(RADV), 1280x720, scale 1.0, `upscaler = fsr3`, with the `nvn_vk_mem:` line added for it (every 600 frames: MB in blocks,
MB handed out, per class, and the driver's VK_EXT_memory_budget numbers):

| point of a 40 minute session | MB in blocks | MB handed out |
|---|---|---|
| title | 1472 | 1149 |
| Chapter 1, fight | 2688 | 2273 |
| back at Chapter Select | 2688 | 1496 |
| Chapter 3, fight | 3136 | 2730 |
| back at Chapter Select | 3136 | 1500 |
| Chapter 2, fight | 3456 | 3004 |

- **Not a leak**: what is handed out returns to 1500 MB at every Chapter Select. The climb is each chapter's own need plus
  the pool keeping its 64 MB blocks (`vmem_free` gives back only allocations above 16 MB): after a chapter 1.3 GB of the
  image blocks is free, but only 7 of 39 blocks are empty; the next chapter fills the free ranges first (5 % free at a peak).
  Nothing was lost to full free lists.
- **Chapter 2 alone needs 3.3 GB** in blocks (2.9 GB handed out) straight from the title. `bc7_textures = off` changes
  nothing: BC3 and BC7 are both 8 bits a texel, where the console's ASTC 8x8 is 2.
- **The budget was not acted on.** New blocks went to system memory only when the driver refused one (issue 22); a 4 GB
  card on Windows did not refuse at 3810 MB. Now a new block that would pass the driver's budget for its heap is treated
  as refused (`vmem_over_budget`; `WT_VK_BUDGET=0` turns it off, `WT_VK_BUDGET=N` is a test budget of N MB). Chapter 2
  with `WT_VK_BUDGET=2800`: 448 MB in system memory, the driver's count 2861 MB (its count trails the allocations),
  57.7 fps against 60.0 here. Not run on Windows, on NVIDIA or on a 4 GB card.

Left: a half-size option for the textures whose ASTC blocks are 8x8 or larger (the only thing that lowers the need).

## Aspect ratios: 21:9, 32:9, 9:10 ... (2026-10-28; experimental)

`aspect_ratio = 16:9 | auto | <w>:<h>` (graphics.ini, at the next start; `RECOMPS_ASPECT=<w/h>` wins): the game's own
picture at any shape. `auto` takes the window's shape at the start (its client size in pixels; borderless and fullscreen:
the monitor's desktop mode), told before the game starts (`gfx_aspect_window`, from `display.c` before the game's thread
runs). In game: **Aspect Ratio (next start): Fixed / Unlocked** (16:9 / auto), a row of the Options screen's Screen
Display page (it was the Original / Stretch row of `aspect`) and of the PC panel; `aspect` (Keep shape / Stretch) is the
PC panel's "Picture fit" now. Checked at 21:9: a 2100x900 window (2625x1125 pixels at 125 %) with `auto` gave the game a
2100x900 screen; the row showed Unlocked, left made it Fixed and wrote `aspect_ratio = 16:9` ("applies at the next
start"). Wider than 16:9 shows more at the sides (Hor+), narrower more above and below (Vert+); the interface stays
a 16:9 block in the middle (it is drawn with the same cameras). Checked on the title screen at 9:10, 1.85, 2.0, 21:9
(2100x900) and 32:9 (3200x900), resolution_scale 1.0 and 1.2; `aspect = keep` puts the result in the window.
**In game** (the save, `WT_PRESS=Z,25,3`, 240 s: title, Shibuya free roam, a radio-call subtitle, a cutscene, a summon,
the fight with the HUD; 16:9, 21:9, 32:9, 9:10; muted, frames every 600 saved): no crash, 10,177-10,282 frames presented
in each (the same as 16:9); the 3D view is Hor+ (32:9: much more of the street at the sides) or Vert+ (9:10: more sky
and street); the cutscene fills the screen (no bars); subtitles, "Break Free" prompts and the boss's bar stay centred.
**The HUD at the screen's edges** (`hud_area = screen`, the default; `16:9` keeps it in the middle): the UI layouts
(`ui/*.dat`, .uid) are in a 1280x720 space around the centre; each layout's root (element 0) has its place (health
-492, 269; gauges -572, -316; score 260, 297 ...). `0x7100c0ef04(layout, state, element, time)` evaluates an element
every frame (elements of 0x1b4 bytes from layout + [layout + 0xc]: position +0, +4 into state + 0x10; then its animation
tracks; found with a read watchpoint on hud0020's root in memory). The menus spanned 21:9 by being stretched (later:
"The stretched menus"; they are in the middle now). Override `ui_element` moves only a root of a layout
named `hud*` (its DAT archive found backwards from the layout in memory, once per layout) placed 200 or more from the
centre in the file, out by (720 * ratio - 1280) / 2. (Moving every such root pushed the Options rows, which their
screen places itself, off the left edge.) Checked at 21:9 and 32:9: health and gauges at the left edge, score, Verse,
combo points and the halo counter at the right; the pause menu and Options (Controller, Camera, Sound, Screen Display)
whole, no row cut; `hud_area = 16:9` as before. Elements placed by code (lock-on markers, damage numbers) were not seen
to move.
**In-game option**: "HUD Layout" (Full Screen / Centered) is a row of the Options screen's Screen Display page (after
Aspect Ratio; label and values in the text mod, all 13 languages: `text_strings.rs`) and of the PC
settings panel. It writes `hud_area` and applies at once (`game_hud_centre`, exported by the game library like
`game_cutscene_fps`). Checked at 21:9 with the virtual pad: the row shows Full Screen, right makes it Centered
(`pcmenu: hud_area = 16:9 (applied)`), and after leaving the menus the HUD is in the 16:9 area in the middle.

**Shaders for another scale or ratio, made ahead**: a run at 16:9 and resolution_scale 1.0 translates its fragment stages
without the screen scale; every other scale or ratio uses one other set (the factors are push constants), which the first
such run translated behind held loading screens. Measured on the 4-minute route (title, Shibuya, a fight; empty cache):
16:9 20.9 / 22.7 s of holds; 21:9 after that 29.5 s more (3,755 fragment stages; the driver's pipelines are a few ms of
it). Now (`nvn_vk_m4.c` `rs_pre_*`, `WT_VK_RS_PREP=0` off) each fragment stage of an unscaled run is copied and its scaled
translation written to the cache by one thread at the lowest priority, which waits while the workers have jobs or a fight
is near (without that wait it added 3-5 s to the 16:9 first run: 25.9 s twice). Measured: 16:9 first run 21.4 s of holds
(2,623 of the 3,755 made ahead in its 4 minutes; the exit line says how many); 21:9 after it 7.0 s (the 1,132 not
reached), against 29.5 s. Stages read from an older cache are queued too, so a 16:9 player's cache gets them over time.

**Movies** (1600x900 VP9, e.g. the opening `ev0000.webm`): drawn by one full-screen pass into the window texture that samples the
frame's Y plane (R8, 1600x900) and UV plane (RG8, 800x450); the game's draw call (`0x7100cd88a0`: x 0, y 0, the screen's
width and height to `0x7100cbbe50/60/70`) goes to empty functions. At 21:9 the picture was stretched (the moon an oval).
Now (`nvn_vk_draw.c` `g_movie_draw`) a draw that samples an R8 texture of the game's 16:9 screen size into a window texture
of another shape keeps a centred 16:9 box and the rest is cleared to black. Checked on the opening movie (a new game,
`WT_USERDATA` empty): 21:9 pillarboxed, the moon round, subtitles inside the picture; 9:10 letterboxed, the subtitles in
the black band below (they are placed by the screen's height, not by the interface's 16:9 area).

**Witch Time** (the full-screen effect): `RECOMPS_TEST_WITCHTIME=<s>[,<s>...]` (test only, `game_shim_eh.c`) starts it
at those seconds through the game's own start, `0x71007a2894(manager *0x710249c470, 0, 0, 0, 0, 0, speed 0.02, 180
frames, 0)` (the arguments of the dodge at `0x710039d86c`; found by the sound events `core_Mov_WitchtimeIn` and
`BGM_Witchtime_On` it plays), called from the main loop's clock between frames. In Shibuya at 70 s: the blue starfield
and darkening for about 3 s at 16:9, 21:9 and 32:9, over the whole width (no unlit band at the sides), its stars the same
size and shape at all three (not stretched); a few of them show the small crosses of the fractional pixel mapping (see
the dither above). **The dither** of objects near the camera (a fine checkerboard at 16:9) became a coarser pattern of crosses at 21:9: the
fragment stage samples a small tiled texture at gl_FragCoord / its size and discards against it, and gl_FragCoord was given
in the game's pixels, 1600 of them over 2100 image pixels. Now (`nvn_vk_draw.c` `g_fs_screen` / `g_fs_small`) a draw
whose fragment stage samples no screen-class image (scaled or kept at the game's size) and samples a texture of 64 x 64 or
less gets the image's own pixels (`mx_fcs` 1): the faded demon in Shibuya shows an even checkerboard again, at the
image's resolution (also at a resolution_scale that is not a whole number). Pause menu, Options and Witch Time
unchanged; the few X-shaped Witch Time stars are the game's own (the same at 16:9). **Not checked yet**: movies,
a real ultrawide monitor. 

**The chapter select's aim** (seen by hand on a 3440x1440 monitor, borderless, Unlocked): the aim ring sat right of where
the dart lands (290 units at 21:9, on the wall instead of the chapter's disc). The aim (`0x7100920570`) is a point on the
map moved by the stick, projected with the camera's viewport (1720x720 at 21:9: the screen getters' width), and kept on
screen by the screen's corners mapped back with `0x7100d02d70` / `0x7100d02d80`, which stay 1280x720 (+304 / +308 of the
graphics object; found with gdb on a run it starts, counting calls of the projection's users while the chapter select
is up). `0x710091c298` stores the aim's position on screen through `0x7100a6f5a0` for the ring, whose drawing takes it as
1280 wide. Overrides `screen_w` (only for the call at `0x71009207f8`: the getter's other 50 callers are not checked) and
`aim_pos` (x times 1280 / the viewport's width). Checked at 21:9 (scripted pad, the save's chapter select): the ring on
the disc at the start, at the right and top edges when pushed there, the dart lands at the ring (as at 16:9). `774b4c`
(below) read from the screen's ratio did not move or round it.

**The stretched menus** (the chapter select's ring an oval 1.34 wide at 21:9, its text, icons and prompts, the pause menu): the
game has two projections for the UI. In a fight its camera, which `h_camera_perspective` makes wider; on flat screens 0 to
1280 and 0 to 720 (`0x7100d07610`, one caller, `0x7100640218`), which filled the screen. Override `ui_ortho` gives the flat one
the camera's space: the 1280 x 720 in the middle, unstretched, with more around it (x -220 to 1500 at 21:9). So the menus are in
the middle now. What that needed besides:
- **Sprites in the UI's screen mode** (+0x4c = 1: the dims, 16 x 9 in `core0030`, 100 x 100 in `menu0103`) are as large as the
  screen by 1280.0 and 720.0, immediates: 32 loads in `eh.rs` `UI_SIZE_SITES` read `wt_ui_w_bits` / `wt_ui_h_bits`. Rust lifter
  only: the stand-in main of the lift test (`tools/wtbuild/testdata/fake_main/main.nso`) has none of them.
- **What is 1280 across in its layout** (size at the element's +0x1a4, kinds 1 and 2, not screen mode, not turned, within 30 of
  the middle; 1240 for thin bands): `h_ui_element` widens it. None has children. A whole screen keeps its shape and loses a
  little at the edges, except the capture types 1000 and 1021, which are the screen's shape already. The cutscene bars are
  this case. `h_clip_cmd` widens a rectangle over all of the 1280; only a heading's wipe clip is scaled (`ui_rule`), its child does not inherit that.
- **Turned a quarter** the same strips (kind 2, the vignettes of `menu0034`, `menu0040`, `menu1010` ...) have their length in y.
- **Named cases**: `menu000b` elements 1, 2 (a picture with a diagonal edge: the chapter select's banner and the Shop's two panels)
  scaled evenly around a point of the diagonal, which stays; `menu006a` 20 to 23 (Play History's side strips, and all four when
  narrower); `hud0430` and `hud0450` 2, 3 (the lines at the ends of their bands); `event0020` 7, 8 when narrower; `event0015`
  (the credits' ring of pictures, widened as a whole; not seen). `ui_name` keeps a signature of the layout, so another
  layout loaded at the same address is not taken for the old one, and it evicts instead of rescanning when full.
- **The capture effects** (types 1000, 1011, 1020, 1021): they copy the window to a texture and draw it back with u = UI x / 1280,
  v = UI y / 720, and u = 1 is the window's right edge. In the wider space that squeezed the picture into the middle 1280 by
  0.744, and 1021's shader (`uiradialchromaticwithblack`) paints outside 0..1 black. That was the Yes / No dialog, `core0030`
  element 25, its last: not a clip (a clip change did nothing). Overrides `capture_quad`, `gauss_quad` correct the rectangle.
- **Points of the 3D view placed on the UI** (a reticle, an icon and a small gauge, hud0340, hud0360, hud0380, and the off-screen
  arrow, hud0420): `0x7100626004` gives them in the camera's viewport, and they took that for UI x, 220 right of their target at
  21:9, and culled at 1280. `ui_pos`, `ui_w`, `h_screen_w`; markers kept on screen (`0x71009a81f4`, `0x71009b0444`) by `ui_w`,
  `ui_h`, `wt_ui_mark`. `h_screen_w` also answers the tests of a projected point (on screen? picks around the middle, the ray
  through the middle: at 21:9 the right quarter of the screen counted as off screen) and the stick-aim cursor (`+0x17c40`, half
  the getter's width at rest, so its ray left the middle), whose reticle (HUD `+0x173e0`) goes through `ui_pos`. Seen: enemy
  gauges on their enemies out to 97 % of the width at 21:9, clamped to the real edges at 9:10; the lock-on marker on its enemy at
  21:9.
- **Narrower than 16:9**: `target_h` as `target_w`, and the two `720` loads in `UI_SIZE_SITES` (`0x7100c1a808`, `0x7100c1a8f4`)
  that cut every plain dim at half the screen.
- **Movies** are a UI quad of 1280 x 720, so `ui_ortho` puts them in the box: the viewport the movie pass set squeezed them twice.
- **Screen filters** (the Clouds of Erasure's tint, a fight's opening): effect quads in front of the camera, sized for a 16:9 view
  (`vs` FNV `ad7d2a1f`, `044ea2e3`, strips into the half-size target that bloom reads), so they stopped at hard edges, two sets
  of them in a fight's opening. `stage_fnv` finds them, their viewport is as much wider. Their fragment stages read the screen
  by `gl_FragCoord`. Wider than 16:9 only, tested on the game's viewport height, as the pass grows with the scale. The Practice
  loading screen's haze is one too, though in the UI's flat projection (`vs` `9f4eba97`, x -8 to 1288, a gradient: a tone step of 5
  to 7 / 255 at columns 265 and 1885 of 2150). That stage is the game's general effect quad, also the world's and in a 128 x 128
  target, so it is taken only under a flat camera (`cb3_flat`, the camera's last row 0 0 0 1).
- **Game Over** (`hud0100`, children of el 2; not `vs 7871`, which only draws the dotted lines): plain sprites at alpha 0.2 in the
  1280 x 720's corners (el 17 to 20) and bands along its top and bottom, anchored at their ends (el 21, 22), a step of 56 levels at
  the old edges (hiding them measured it). They go to the screen's corners, the bands as much longer; the speckles (el 11, 12) get
  the rule's scale, as they drift 200 and their edge came to the 1280's.
- **The rule under a heading** (10 elements in 377 layouts, `ui_rule`): one nine-grid picture, `0x7100c1b25c` keeps its caps' size,
  so scale lengthens only the middle and the ornament stays. Half are wiped in by a clip whose rectangle cut them at the old left
  edge; a child inherits a kind 6 clip's position, not its scale. Left end by `ui_x0`, right end unmoved, the wipe starts at the
  screen's left. The tip goes with the line, away from the heading's text; on the Shop the parchment is drawn over it.

Found by a survey of the screens and an audit of the code (the capture UV came from reading `0x710097308c`, the squeeze from
measuring text against 16:9: 0.744 = 1280 / 1720, 0.5 at 32:9). Checked by screenshots on Linux at 21:9: chapter select, Chapter
Menu, Difficulty, Play History, Options, the Yes / No dialog, the Shop, movies, a fight of Verse 7 with its opening and gauges, the
ride's filter; at 32:9 the dialog; at 9:10 chapter select, Options, the dialog, a fight's HUD; at 16:9 the menus and that fight.
From `graphics.ini`: 21:9 at scale 2.0 (dialog, the ride's filter), 16:10 at scale 2.0 (Play History). On an empty cache at 21:9
the second start translated no fragment stage. The ride's prompts and HUD were seen at 21:9; the band of noise as its fight starts
is the game's own wipe, 16:9 has it too. Game Over at 21:9, 32:9 and 9:10: no step at the old edges beyond the picture's own (1.3
levels at most); the Practice loading screen at 21:9, also at scale 2.0: 0.7 / 255 at most; the rule at 21:9, 32:9, unmoved at 9:10.
UK English (`WT_LANGUAGE=en-GB`, a language with 144 layouts of its own) at 21:9: Main Menu, Practice, the ride; its Game Over not seen.
Not seen since: the verse result, the cutscene
bars on the ride (not in its first 110 s), the stick-aim cursor, Windows.

**The cutscene bars** (seen by hand on the spider ride, 3440x1440): the black bars are UI elements 1280 across, at y = -360 and
360 of `huda02a` (the ride) and `event0020`, so they stopped at the 16:9 block. `h_ui_element` widens those two by the
screen's ratio to 16:9, with the HUD at the edges or centred. Confirmed by hand on the ride; `event0020` was not seen.

**The HUD's gauges** (health, Demon Slave, enemy bars; seen by hand): their fill is a scissor, made by the UI's clip
element `0x7100c1dfe8` as (x + 640) * the target's width (`0x7100d02d90`) / 1280. Wider than 16:9 the UI is 720 * ratio
wide around the middle, so a full health bar showed 74 % (HUD at the edges) or 20 % (centred) at 21:9. Found by logging
the scissors per draw (16:9: `107 43 200 10`; 21:9: `0 43 268 10`), then gdb up from the game's scissor setter
`0x7100cf0e08` through its callback `0x7100c03880` (a pointer at `0x71024abb98`, taken at `0x7100c1e3b0`). Overrides
`target_w` (that width times 1280 / (720 * ratio), only for its two calls there) and `clip_cmd` (`0x7100d04414`, only for
its call at `0x7100c1e388`: the rectangle on the stack moved right by half the extra width before it is copied). Checked
at 21:9 in a Verse 3 fight: the scissors equal 16:9's, the three bars full, with the HUD at the edges and centred.
Wider than 16:9 only. **Verse results screen**: reported not reaching the right edge; not reproduced yet (a scripted
run does not finish a verse).

What it takes (found by reading main with `llvm-objdump` and by gdb watchpoints on the lifted code):
- **The screen size**: `0x710070a740(int *w, int *h)` stores 1600x900 (docked), `0x710070a760` 1280x720 (handheld);
  callers: the NVN window set-up `0x7100d0c990` (the window textures) and the per-frame docked / handheld check
  `0x7100cfdc50`. Host overrides `screen_docked` / `screen_handheld` (`runtime/game_shim_eh.c`): the height stays,
  the width follows the ratio. Alone, this squeezes or stretches the picture: the scene is still drawn at 1600x896 and
  put into the window texture.
- **The render-target heap**: from 1944x900 up the start-up failed (`HOST SIGNAL 11` at address 8 in `f_7100b845a0`:
  the main loop's object list was never made). The renderer set-up `0x710069c698` gave up when its 84th render target
  (`0x7100cfa5c8`, 1600x896) did not fit the heap the targets and the window textures share: 0x10400000 bytes, an
  immediate at `0x710069d164`, in the config passed to the graphics set-up `0x7100d01380` (+0x90). Override
  `gfx_setup` makes it grow with the width (21:9: 0x15600000, 32:9: 0x20a00000), and `r2_imports.c` tells the game it
  has as much more memory (it sizes its heap from that; the blocks it takes are fixed).
- **The cameras**: `0x7100cba90c(camera, fov, near, far, width, height)` sets a perspective with aspect = width / height
  (camera + 0x3d8; the projection is `0x7100cb9920`; +0x3d0 near, +0x3d4 far, +0x3dc vertical angle). Its callers give
  fixed 1280x720 (`0x7100624e10`, `0x7100c04b00`), 256 high (`0x7100646b64`), or their own. Override
  `camera_perspective`: a 16:9 size gets the screen's shape; narrower than 16:9 the vertical angle grows too
  (2 atan(tan(fov / 2) * (16/9) / ratio)), as in `camera_fov` for `0x7100cba97c`, the angle alone, which the scenes'
  cameras call every frame. All cameras seen on the title had aspect 16/9 exactly before.
- **The scene's resolution**: the game keeps making its scene targets at 16:9 sizes (1600x896, 1440x816 and their halves
  are immediates in `0x710069c698` and the dynamic resolution's code). The Vulkan backend makes those (a 16:9 shape,
  not the window's) `ratio * 9/16` times wider (`nvn_vk_draw.c` `rs_xstretch`, `rs_169_class`), on top of
  resolution_scale; scaling is on at 1.0 then too. `gl_FragCoord` takes an x and a y factor now (`mx_fcs`, `mx_fcs_y`;
  push constants 20 bytes; the fragment stages' hash key `rs1` became `rs2`, so a run with a scale translates its
  fragment stages once again: about two minutes of held loading screens, once). 21:9 at 1.0: 47 images scaled, 138 MB;
  at 1.2: 196 MB.
- **16/9 as a constant**: `mov/movk 0x3fe38e39` at 8 places (`0x7100706b70`, a getter; `774b4c`, `c2abe8`, `c30668`,
  `c81288`, `c826e8`, `ca8dd4`, `ca921c`; most divide it by a width / height). Reading all eight from the screen's
  ratio changed nothing on the title; they are left as they are (candidates if gameplay shows a screen-space effect or
  a pointer out of place). theboy181's 21:9 patch for 1.1.0 (one `fmov s1, #2.375`, made for emulators that stretch the
  picture) is at one of these.
- Measured at 16:9 without the key: the same pictures as before (two runs differ by 0.1 % of pixels, stars); the overrides
  only pass through.

Next: a run on a 21:9 monitor.

## Next blockers (most urgent first)

Camera (right stick): confirmed working by hand on a real pad (2026-10-24); no work needed.

1. **Which route a run takes** depends on the save and on when the A presses land on the title (movie route, Chapter
   Select → Prologue, Chapter Select → chapter 1). An `WT_SEQ` with `@draws` anchors would make it reproducible.
2. **Long frames left in Tokyo** (section "Performance report"): trains of 45-50 ms in a tree walk of lifted code, one
   80-86 ms frame at the game-over retry reload. Fights and Tokyo otherwise hold 60 (98 % of frames); the 4K cutscene at
   60 is partly GPU-bound (57 fps).
3. **Picture correctness**: no reference images to compare with.
4. **Windows build** of the witch-time-game rename (not compiled here).
5. **Barriers in the translator's dispatch loop** (`SharedMemoryDataRace-RaceOnLoadStoreVsAtomic`, program #7839):
   structured control flow for stages with `BAR`.
6. **First-run warm-up**: minutes of held loading screens after a translator change.
7. The static scene after `WT_SKIP_MOVIES` (stage 5), `nn::account::PopOpenUsers` (called once, harmless), the
   `nn::audio` Resampler imports (not reached), about 170 other imports without a handler (none reached).

## Known shortcuts (marked `shortcut:` in the code)

- `SdkMutexType`, `pthread_once`, `std::__call_once` wait by spinning on `Sleep(0)` (no kernel wait queue).
- `nn::fs` thread fs-context is a shared dummy object; `nn::oe` notifications / recording and `hid` mouse are no-ops.
- `--pkz-dump` calls main's Oodle functions at fixed addresses (checked against two instruction words at start: other
  builds of main stop with a message); it holds a whole entry in memory (the largest is 373 MB).
- `wtbuild`'s Windows build links `intel_tex_2`'s MSVC archives through renamed copies and `msvc_shim` (`src/texpack.rs`); a
  gnullvm build of the crate, or a port of the encoder, would replace it.
- The heap size (3,285 MB) is the console's documented application memory, not measured on hardware.
- NVN 55 additions (`nvn_gl.c`, `nvn55:` in the log): deferred binds are read when recorded; separate samplers are
  dropped and separate textures bound as combined handles; transform feedback, subroutines, checkpoints, subtile size and
  `MapVirtual` are logged no-ops.
- `nvnCommandBufferDispatchComputeIndirect` (Vulkan) reads the group counts from guest memory after a sync.
- `nn::os` ReaderWriterLock has no writer preference.
- Preparation adds one integer key (attribute 2, `job_prep`): another key in a later chapter still compiles at its first draw.
- `aspect_ratio` (`game_shim_eh.c`): checked on the title screen and 4 minutes of Shibuya (section "Aspect ratios").
- The device fields the sdk's texture code reads are modelled (`nvn_sdk.c`); "compression supported" (+0x1b34 = 1) is
  inferred, its writer was not found.
- Translator (stage 3): `SAM` / `RAM` are no-ops (helper invocations assumed active for quad shuffles); a register texel
  offset of an implicit-LOD sample is scaled by level 0's size; storage images need a format with storage support and the
  same texel size as the shader's typed format (else logged as missing).
- Movies (FFmpeg): packets of a selected track read ahead of time wait in a queue of 512; past that the oldest is dropped
  (`shortcut:` in `nnmovie_mf.c`).
- Indirect draws are read on the CPU (one Vulkan draw per record) unless compute wrote their count or records in this batch
  (then `vkCmdDraw*Indirect*`, "The render thread and the GPU overlap"); a count whose records past it name indices outside
  the index buffer, and indirect dispatches, still cost a submit and a wait.
- Storage writes of a vertex / fragment stage are made visible at the end of the pass (a barrier), not between draws of the
  same pass (as on NVN without an explicit barrier).
- Tessellation: a tessellated pipeline is made on the render thread at its first draw (one hitch; the driver's pipeline
  cache keeps it); generic patch attributes and a vertex stage with clip distances in a tessellated program are not
  handled (none in B3); the picture of the tessellated object was not compared with a reference.
- Not tested: Windows builds of these changes (only `TOOLCHAIN=linux` was built and run).
