<p align="center">
  <img src="docs/images/witch-time-logo.svg" alt="Witch Time" width="720">
</p>

# Witch Time
![](https://dcbadge.limes.pink/api/server/https://discord.gg/hvkD3JNhWZ) ![GitHub Downloads (all assets, all releases)](https://img.shields.io/github/downloads/Zexyen/witch-time/total)


An unofficial static recompilation of *Bayonetta 3* (Nintendo Switch) for PC, in the spirit and architecture of
[B2recomp](../B2recomp): the game's AArch64 code is translated to C **on your PC from your own dump**, and runs on a
re-implemented system layer (threads, file system, input, audio, and NVN on Vulkan). Not an emulator, and not a way to
get the game.

**This repository contains no game code, no game assets, no Nintendo SDK code, no keys.** Everything derived from the
game (converted ELFs, translated C, the compiled `game.so`, caches) is generated locally into `targets/`, `build/`,
`cache/` and `runtime/gen/`, which git ignores. Never share them.

## Status

| What | Result |
|---|---|
| Boot and gameplay | boots and plays; 60 fps in play (Tokyo and Shibuya tested), occasional short hitches |
| Menus and cutscenes | 30 fps as on the console by default; 60 fps with `cutscene_fps = 60` in `config/graphics.ini` (timing stays real time; movies keep their 30 fps) |
| Resolution | the game's 1600x900 by default; `resolution_scale` in `config/graphics.ini` renders it larger (1.2 = 1080p, 1.6 = 1440p, 2.4 = 4K, measured at 60 fps on an RX 9070 XT; changed live from the PC settings panel or the game's Options; the first time away from 1.0 on a PC, the game stops for its shaders once) |
| Input | controllers, with every button of gameplay rebindable in the game (Options, Controller, Controls), by character, and the prompts follow; keyboard + mouse (rebindable in `config/controllers.ini`; mouse camera; both at once); keyboard tested with synthetic events, not yet by hand |
| Settings | in-game PC settings panel: F1 or Start + Select (the game is held while it is open); vsync, cutscene frame rate, window, anisotropic filtering, texture detail and aspect change at once, the internal resolution when the panel closes, the frame rate (60 / 90 / 120, experimental above 60) at the next start; written to `config/graphics.ini` with its comments kept; a new version updates that file's comments and adds its new keys at the next start, your values kept and the old file in `graphics.ini.bak`. The same settings (internal resolution 900p-2160p, cutscene frame rate, frame rate 60 / 90 / 120 (experimental), vsync, display mode, anisotropic filtering, texture detail, aspect) are also rows of the game's own Options screen (Screen Display page; changed with left / right, X resets a row), with a "PC Settings" row that opens the panel; labelled in each of the game's languages (setup step `text` makes that text from your dump). Not a tab of their own: see STATUS.md |

Details: [docs/STATUS.md](docs/STATUS.md); findings: [docs/RE_NOTES.md](docs/RE_NOTES.md).

## Build (Linux)

A release needs nothing installed but a Vulkan driver and OpenGL (`libGL.so.1`), which every desktop distribution
has: it brings its own compiler (clang + lld in `deps/llvm`), the runtime already built
(`prebuilt/`, with the FFmpeg it uses) and the build tool. Your PC only translates and compiles the game's code, and
that needs no system headers. The `deps` step checks all of it and names what is missing. Your dump: one folder with
`exefs/` (`main`, `sdk`, `subsdk0`, `rtld`, `main.npdm`, plain NSO files) and `romfs/` (the `.pkz` archives and
`movie/`), extracted with your own tools from your own console and game; this project decrypts nothing.

A checkout has no `prebuilt/`, so the link step builds the runtime too: that needs clang 19 or newer + lld and the
development files of SDL3, Vulkan, glslang, FFmpeg and OpenGL. The same happens in a release whose `runtime/`
you changed (`tools/wtbuild/README.md`, "Prebuilt runtime").

The build is one program, `wtbuild` (`tools/wtbuild`, Rust). A release ships it built as `build/wtbuild`; in a checkout,
build it once with cargo:

```
cargo build --release --manifest-path tools/wtbuild/Cargo.toml
tools/wtbuild/target/release/wtbuild setup --dump <your dump folder>
```

Or build the launcher (`launcher/README.md`) and run `build/witch-time`, which does the same with progress on screen
and then plays in its own window.

Setup puts the game data (the dump's `romfs`) in `targets/romfs`, so the Witch Time folder holds the whole game and can be
moved anywhere: copied by default (hard links on the same drive, so no extra space, and your dump stays whole), or moved
with `--move-dump` (the launcher's "Move the game data here" switch).

The steps (`deps verify convert mods tables lift compile link text check`, resumable; `--only`, `--from`, `--status`)
are those of `scripts/setup.py`, which stays as the reference the Rust port is checked
against (`tools/wtbuild/README.md`). The `verify` step accepts only the build named in `tools/wtbuild/src/setup.rs`.

## Build (Windows)

Windows 10 or 11, 64-bit. A release (`WitchTime-<version>-windows-x86_64.zip`) needs nothing installed but a Vulkan driver: it
brings LLVM's own clang + lld (`deps\llvm`), the runtime already built (`prebuilt\`: `witch-time-game.exe`,
`SDL3.dll`, `ssbu_glslang.dll`, FFmpeg's DLLs) and `build\wtbuild.exe`. Unzip it, run `witch-time.exe` and pick your dump folder, or:

```
build\wtbuild.exe setup --dump <your dump folder>
```

Windows Defender might flag Witch Time as a Trojan: the release's binaries are not code-signed, and each release has
new ones Defender has not seen. Download it only from this repository's Releases page, and check the zip against the
`.sha256` file beside it (`certutil -hashfile <zip> SHA256`).

Movies play through the FFmpeg that ships in `prebuilt\`, in software, so they need no codec on your PC. The album's video
clips use Media Foundation, which Windows has; the "N" editions need the Media Feature Pack, and the `deps` step says so
when it is missing. The game runs as `build\witch-time-game.exe`, with its own window; the launcher waits for it.

Building the runtime yourself (a checkout, or a changed `runtime\`) needs llvm-mingw and the MinGW builds of SDL3,
FFmpeg, glslang and the Vulkan headers, named in `CPATH` and `LIBRARY_PATH`. CI cross-builds all of it on Linux
(`.github/workflows/release.yml`, job `windows`): `wtbuild runtime --target windows --out prebuilt`.

## Credits

Lifter, runtime and build scripts come from B2recomp (K0NL), which builds on
[ssbu-recomp](https://github.com/Denkishi/ssbu-recomp) (Denkishi). GPL-3.0; see `NOTICE.md` and `LICENSE`.

The logo's lettering is set in the "Bayonetta" typeface by
[Kustren](http://kustren.deviantart.com/) (free for non-commercial use), converted to outlines; the font file itself
is not included.
