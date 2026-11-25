<p align="center">
  <img src="docs/images/witch-time-logo.svg" alt="Witch Time" width="720">
</p>

<p align="center">
  <a href="https://github.com/Zexyen/Witch-Time/releases">
    <img src="https://img.shields.io/github/downloads/Zexyen/Witch-Time/total" alt="GitHub Downloads (all assets, all releases)">
  </a>
</p>

<p align="center">
  <a href="https://recomplauncher.com/game/witch-time">
    <picture>
      <source media="(prefers-color-scheme: dark)" srcset="https://recomplauncher.com/assets/press/launcher/badge-light.svg">
      <img alt="Get it on RecompLauncher" src="https://recomplauncher.com/assets/press/launcher/badge-dark.svg" height="48">
    </picture>
  </a>
</p>

An unofficial static recompilation of *Bayonetta 3* (Nintendo Switch) for PC, in the spirit and architecture of B2recomp. The game's AArch64 code is translated to C **locally on your PC from your own dump** and runs on a re-implemented system layer (threads, file system, input, audio, and NVN on Vulkan). This is not an emulator, and not a way to obtain the game.

> **No game data is distributed here.** This repository contains no game code, no game assets, no Nintendo SDK code, and no keys. Everything derived from your dump (converted ELFs, translated C, compiled game libraries, caches) is generated locally into `targets/`, `build/`, `cache/`, and `runtime/gen/`, all of which are gitignored. Never share those directories.

## Status

| Area | Current State |
|---|---|
| **Gameplay** | Boots and plays at 60 fps (with occasional short hitches). |
| **Menus & cutscenes** | 60 fps by default (or your chosen `frame_rate`; timing stays real-time; pre-rendered movies remain 30 fps). |
| **Resolution & graphics** | 1600×900 by default; `resolution_scale` renders higher (`1.2` = 1080p, `1.6` = 1440p, `2.4` = 4K; first run at a new non-1.0 scale compiles shaders once). Supports HDR, Model Detail, Film Grain, BC7 texture caching, and experimental ultrawide aspect ratios. |
| **Upscaling & frame rate** | Experimental FSR 3.1 (Native AA and Quality–Ultra Performance), flexible frame rates (`frame_rate`: 30 / 40 / 60 / 90 / 120 / 144 / 165 / 240 fps, or any custom value 30–960 in [`config/graphics.ini`](config/graphics.ini), keeping real-time speed down to 15 fps), and experimental Windows-only DLSS, FSR 4, and Frame Generation using your own DLLs. |
| **Input** | Full controller support with per-character in-game button rebinding (*Options → Controller → Controls*) and dynamic prompts (Switch, PlayStation, Xbox, Keyboard). Simultaneous keyboard + mouse support with mouse camera, rebindable in [`config/controllers.ini`](config/controllers.ini). |
| **In-game settings** | Press `F1` or `Start + Select` to open the PC Settings overlay (pauses the game; includes *Quit to desktop*). The same options also appear in a dedicated **PC Settings** tab inside the game's Options menu, localized in every game language via the `text` setup step. Changes save to [`config/graphics.ini`](config/graphics.ini) with comments preserved (updates back up your previous file to `graphics.ini.bak`). |
| **Mods** | Supports emulator-style code mods (`.pchtxt`, `.ips`) and RomFS data mods (`mods/`). See [`tools/wtbuild/README.md`](tools/wtbuild/README.md#mods). |
| **Discord** | Rich Presence showing chapter, verse, difficulty, area, and chapter time (`discord_presence = off` disables it). |

For technical progress and reverse-engineering details, see [`docs/STATUS.md`](docs/STATUS.md) and [`docs/RE_NOTES.md`](docs/RE_NOTES.md).

## What You Need

You must provide your own legally dumped copy of *Bayonetta 3* (Update **1.2.0**; exact hashes are checked by `verify` in [`tools/wtbuild/src/setup.rs`](tools/wtbuild/src/setup.rs)) in either format:

- **NSP / XCI files** — Base game `.nsp`/`.xci` + Update 1.2.0 `.nsp` (or a merged `.xci`/`.nsp`, or a folder holding both) alongside your console's `prod.keys` (auto-detected next to the packages or in `~/.switch/prod.keys`, or passed with `--keys`). Witch Time extracts everything locally—including **Program 1 (The Old Picture Book prologue)**.
- **Extracted dump folder** — Contains `exefs/` (`main`, `sdk`, `subsdk0`, `rtld`, `main.npdm` as plain NSO files), `romfs/` (`.pkz` archives and `movie/`), and optionally `program1/`.

During setup, game data (`romfs/`) is placed into `targets/romfs` so the Witch Time folder is self-contained and portable. For extracted folders, files are copied (or hard-linked on the same drive, taking no extra space) by default; pass `--move-dump` (or enable *"Move the game data here"* in the launcher) to move them instead.

## Getting Started (Releases)

Prebuilt releases bundle the launcher, `wtbuild`, the compiled runtime (with FFmpeg), and LLVM (`deps/llvm` — `clang` + `lld`), so your PC translates and compiles the game code without needing system headers or a compiler installed.

### RecompLauncher (Recommended)

Witch Time can be installed, built, configured, and launched directly through [RecompLauncher](https://recomplauncher.com/game/witch-time) (via [`recomplauncher.json`](recomplauncher.json), included in every release):

- Select your NSP/XCI files (or extracted dump folder) and RecompLauncher runs the build headlessly (`witch-time --prepare`), then launches the game with `witch-time --play`.
- Updates are managed by RecompLauncher (`RECOMPLAUNCHER=1` disables the launcher's built-in updater).
- Settings, saves, logs, and shader caches live in `recompdata/` while the compiled game stays in `build/` and `targets/`.
- Big Picture mode launches borderless when `graphics.ini` is set to windowed, and *Button prompts = Auto* follows RecompLauncher's controller hint until the game sees a controller.

### Linux

**Requirements:** A Vulkan driver and OpenGL (`libGL.so.1`).

Releases are provided as both a `.tar.gz` archive and an `.AppImage` containing the same directory layout:

- **`.tar.gz`:** Extract the archive and run `./witch-time` to pick your dump and build with on-screen progress.
- **`.AppImage`:** Because Witch Time builds into its own directory, the AppImage runs unpacked: the first start copies it into a `WitchTime` folder beside the AppImage (`~/.local/share/WitchTime` when that folder is read-only) and later starts play from there. RecompLauncher unpacks it on install.
- **CLI alternative:**
  ```sh
  # From NSP/XCI files (also extracts The Old Picture Book):
  build/wtbuild setup --dump <base.nsp|.xci> --update <update-1.2.0.nsp> [--keys <prod.keys>]

  # Or from an already-extracted dump folder:
  build/wtbuild setup --dump <dump-folder>
  ```

### Windows

**Requirements:** 64-bit Windows 10 or 11 and a Vulkan driver.

1. Download `WitchTime-<version>-windows-x86_64.zip` from the [Releases](https://github.com/Zexyen/Witch-Time/releases) page.
2. Unzip it and run `witch-time.exe`, then select your NSP/XCI files (with `prod.keys`) or extracted dump folder.
   - **CLI alternative:**
     ```bat
     build\wtbuild.exe setup --dump <base.nsp|.xci> --update <update-1.2.0.nsp> [--keys <prod.keys>]
     build\wtbuild.exe setup --dump <dump-folder>
     ```

**Notes:**
- **Windows Defender:** Release binaries are not code-signed, so Defender may flag new releases. Download only from this repository's Releases page and verify the archive against the SHA-256 digest listed on the Releases page:
  ```bat
  certutil -hashfile WitchTime-<version>-windows-x86_64.zip SHA256
  ```
- **Video playback:** Cutscene movies decode in software using the bundled FFmpeg in `prebuilt\`, requiring no system codecs. Album video clips use Windows Media Foundation; Windows "N" editions need the Media Feature Pack (checked by `wtbuild deps`).
- The game runs in its own window as `build\witch-time-game.exe` while the launcher waits for it.

### Android *(In Progress)*

Targets AArch64 Android 10+ devices with Vulkan 1.1. Releases include `WitchTime-<version>-android.apk` and `WitchTime-<version>-android-prebuilt.zip`; you compile the Android game library (`game.so`) on your PC from your dump and copy it to the device. See [`android/README.md`](android/README.md) for current status, Turnip driver notes, and setup instructions.

## Direct Launch & Shortcuts

- **`witch-time --play`** (Windows: `witch-time.exe --play`) skips the launcher UI, starts the game immediately, and exits when the game closes—useful for Steam shortcuts (launch options `--play`), Playnite, LaunchBox, or desktop entries. The launcher still opens if it detects a newer release (waits up to 5 s), if the game is not built yet, or if it was built by an older release.
- **`witch-time --data <folder>`** stores settings, saves, logs, and shader caches in a separate folder (copying existing settings and saves on first use), keeping `build/` and `targets/` in the install folder.
- **Desktop shortcuts:** In the launcher, open *Settings → Display → Desktop shortcut* to create a direct-launch shortcut (`Witch Time.lnk` on Windows; an application menu and desktop entry on Linux/Steam Deck—add it to Steam via *Add a Non-Steam Game* for Game Mode). Recreate it if you move the game folder.

## The Old Picture Book (Program 1)

*The Old Picture Book* (found in Rodin's Treasures, then on the shelf in the chapter-select room) launches a second game executable—Program 1 of Update 1.2.0 (Title ID `01004A4010FEA001`), the *Bayonetta Origins* prologue:

- **NSP / XCI dumps:** Program 1 is automatically verified and extracted alongside the main game.
- **Extracted dump folders:** Place it in a `program1/` folder beside `exefs/` and `romfs/`, containing `program1/exefs/` (`main`, `sdk`, `main.npdm`) and `program1/romfs/` (36 files, 361 MB).

The `book` setup step builds it into `build/program1` (taking ~3 minutes). If omitted, the step is skipped and opening the book in-game explains why; adding `program1/` later builds it on the next rebuild. Choosing *"Open the book?"* transitions smoothly between *Bayonetta 3* and the prologue, sharing saves and settings.

## Building from Source

A git checkout does not include `prebuilt/`, so the `link` step also builds the runtime (this also happens in a release if you modify files under `runtime/`; see [`tools/wtbuild/README.md`](tools/wtbuild/README.md)).

### Linux

Install **Clang 19+**, **lld**, **Rust / Cargo**, and development headers/libraries for **SDL3**, **Vulkan**, **glslang**, **FFmpeg**, and **OpenGL** (`wtbuild deps` reports anything missing).

```sh
cargo build --release --manifest-path tools/wtbuild/Cargo.toml

# From NSP/XCI files:
tools/wtbuild/target/release/wtbuild setup --dump <base.nsp|.xci> --update <update-1.2.0.nsp> [--keys <prod.keys>]

# Or from an extracted dump folder:
tools/wtbuild/target/release/wtbuild setup --dump <dump-folder>
```

To also build the GUI launcher (`build/witch-time`), see [`launcher/README.md`](launcher/README.md).

The setup pipeline runs these resumable steps: `deps verify convert mods tables lift compile link book text check` (supports `--only`, `--from`, and `--status`). See [`tools/wtbuild/README.md`](tools/wtbuild/README.md#steps) for details.

### Windows

Building the runtime from source requires **llvm-mingw** plus MinGW builds of **SDL3**, **FFmpeg**, **glslang**, and the **Vulkan headers** pointed to by `CPATH` and `LIBRARY_PATH`. CI cross-builds everything on Linux via `wtbuild runtime --target windows --out prebuilt`—see the `windows` job in [`.github/workflows/release.yml`](.github/workflows/release.yml) for the reference setup.

### macOS *(Experimental, Apple Silicon)*

Source checkout only (no macOS releases are published yet). The game's AArch64 code compiles natively for Apple Silicon and renders through Vulkan on MoltenVK (Metal). See [`docs/STATUS.md`](docs/STATUS.md) (*macOS*) for current progress.

Install Apple's Command Line Tools (`xcode-select --install`) and the required dependencies from [Homebrew](https://brew.sh):

```sh
brew install rust sdl3 molten-vk vulkan-loader vulkan-headers glslang spirv-tools ffmpeg
cargo build --release --manifest-path tools/wtbuild/Cargo.toml
tools/wtbuild/target/release/wtbuild setup --dump <dump-folder>
```

The `link` step builds `libwitchtime.dylib`, `witch-time-game`, `libwt_glslang.dylib`, and `build/game.dylib`. To also build the launcher (`build/witch-time` and `build/Witch Time.app`), install `cmake`, `ninja`, and `freetype` via Homebrew and follow [`launcher/README.md`](launcher/README.md).

## Credits & License

The lifter, runtime, and build system originate from **B2recomp** (K0NL), building on [ssbu-recomp](https://github.com/Denkishi/ssbu-recomp) (Denkishi). Licensed under **GPL-3.0**—see [`NOTICE.md`](NOTICE.md) and [`LICENSE`](LICENSE).

The logo lettering is set in [Cinzel Decorative](https://fonts.google.com/specimen/Cinzel+Decorative) by Natanael Gama (SIL Open Font License 1.1), converted to outlines; the font file itself is not included.
