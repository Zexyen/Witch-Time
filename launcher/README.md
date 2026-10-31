# Witch Time (launcher)

One window for setting up, building and playing: it checks your dump, runs the build with live progress, lists mods,
edits `config/graphics.ini`, and then starts the game in the same window. C++17, SDL3 and
[RmlUi](https://github.com/mikke89/RmlUi) on SDL's GPU API. Linux and Windows.

## Build

Needs CMake 3.20+, a C++17 compiler, SDL3 3.2 or newer (with its CMake config) and FreeType. RmlUi 6.3 is fetched by
CMake, or pass `-DRMLUI_SOURCE_DIR=<checkout>` to build without network access.

```
cmake -S launcher -B build/launcher -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/launcher
./build/witch-time
```

On Windows it is cross-built with llvm-mingw (`-DCMAKE_TOOLCHAIN_FILE=<mingw toolchain>`, as the release workflow
does), statically linked but for `SDL3.dll`, and also makes the top-level `witch-time.exe`, a small program that starts
`build\witch-time.exe` (a zip cannot hold a link).

The program is written to `build/witch-time` and finds the repository from its own folder (or from `WITCHTIME_REPO`).
The screens are in `launcher/ui` (RML and RCSS) and are read at start, so they can be changed without rebuilding.

## Releases

`.github/workflows/release.yml` builds the launcher on Ubuntu 22.04 with SDL3 (`release-3.4.18`). Pushing a tag such
as `v0.1.0` publishes a release with `WitchTime-<tag>-linux-x86_64.tar.gz`: the repository source plus
`build/witch-time`, the `libSDL3.so.0` it uses, `build/wtbuild` (the build tool, `tools/wtbuild`), the runtime built
in CI (`prebuilt/`, with FFmpeg) and clang + lld (`deps/llvm`). Unpack it and run `./witch-time`. Building the game then
needs only a Vulkan driver and OpenGL from the system ("Build (Linux)" in the main README). Running the
workflow by hand only builds the archive and keeps it as a workflow artifact.

The same tag also publishes `WitchTime-<tag>-windows-x86_64.zip`, cross-built on Ubuntu with llvm-mingw and then tested
on a Windows runner (`smoke_link.py` against the runtime it ships): `witch-time.exe`, `build\witch-time.exe`, `SDL3.dll`,
`build\wtbuild.exe`, the runtime (`prebuilt\`, with FFmpeg) and LLVM's Windows clang + lld (`deps\llvm`). It needs only a Vulkan
driver ("Build (Windows)" in the main README).

## How it works

- **Build.** Runs `wtbuild setup` (`build/wtbuild` in a release, `tools/wtbuild/target/release/wtbuild` in a checkout;
  without either, `python3 scripts/setup.py`) and reads its `[i/10]` step lines and `build/logs/compile.log` for
  progress. Stop sends SIGTERM (on Windows it ends the build's job object); the next build resumes.
- **Button hints.** The footer shows the game's own prompts (`gen:glyph`, drawn by `runtime/prompt_art.h`): those of the
  device used last, or of `button_prompts` in `config/controllers.ini` when that names one (read at start).
- **Mods.** Listed with `wtbuild mods-tree` (or `scripts/pchtxt.py tree`); switches are written to `config/mods.ini`. Code mods need a rebuild
  from the mods step, data mods apply at the next start.
- **Play in the same window.** The runtime link step also makes `build/libwitchtime.so`. The launcher loads it, hands its
  SDL window over (SDL global property `witchtime.window`, which `runtime/display.c` adopts) and calls `wt_host_main`.
  When the game exits through `ExitProcess`, `plat_exit_hook` starts the launcher again (`witch-time --returned
  <code>`), unless the player closed the window or it was started with `--play`. If `libwitchtime.so` is missing or fails to load, the launcher hides its
  window and runs `build/witch-time-game` instead. On Windows the game always runs that way, as
  `build\witch-time-game.exe` with its own window. Either way the game's output goes to `build/run/witch-time-game.log`.
- **Updates.** A release build (`WT_VERSION` `vX.Y.Z`) asks GitHub for the latest release at start and, if it is
  newer, offers "Update to vX.Y.Z" on the title menu. That downloads this platform's archive with the system's `curl`,
  checks it against the SHA-256 digest GitHub lists for it (`build/wtbuild sha256`), unpacks it with `tar` (Windows 10 and 11 ship
  `curl` and `tar`), moves its files over the installed ones (keeping the player's `config/`) and starts the new
  launcher. The replaced files wait in `build/update` until the next start. A release launcher will not start a game
  that `build/built_version` says another release built (also after unpacking a release by hand); it opens the build
  screen. The next build redoes the steps the new
  sources feed (`REDO_ON_UPDATE` in `tools/wtbuild/src/setup.rs`).
- **Straight to the game.** `witch-time --play` (on Windows `witch-time.exe --play`) starts the game at once and ends
  when it does, for a Steam shortcut (Steam Deck game mode) or a desktop entry. If the game is not built yet, the
  launcher stays open and says so.
- **Input.** Mouse, keyboard (arrows, Enter, Escape) and controllers (d-pad or stick, A/Cross select, B/Circle back;
  left and right change a setting).

Fonts in `ui/fonts` are under the SIL Open Font License (the `OFL-*.txt` files next to them).
