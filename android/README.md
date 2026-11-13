# Witch Time for Android

In progress: on an AYN Thor (Snapdragon 8 Gen 2, Adreno 740, Android 13) the game reaches its title and first menus, with
sound, at 1280x720 and a steady 30 fps. Nothing past them has been played yet. AArch64 (arm64-v8a), Android 10 or newer, a GPU with
Vulkan 1.1 (the Adreno 740 is the only one tried).

The app (`WitchTime-<version>-android.apk`) holds the runtime and the libraries it uses, nothing from the game. Your PC
builds the game library for it from your dump, as it does for itself:

1. Set up Witch Time on your PC as usual (`wtbuild setup --dump <dump>`), and unpack
   `WitchTime-<version>-android-prebuilt.zip` into the Witch Time folder (it adds `prebuilt-android/`).
2. Build the Android game library: `build/wtbuild compile --target android`, then `build/wtbuild link --target android`.
   It is written to `build/android/game.so`; the objects are kept in `build/obj_android`.
3. Install the app once and start it, so that Android makes its folder, `Android/data/com.zexyen.witchtime/files` on
   the phone's shared storage. That folder is a Witch Time folder, as on the PC:
   - `targets/` of your PC's Witch Time folder (`exefs/main.elf`, `exefs/sdk.elf`, `romfs/`)
   - `build/game.so`: your PC's `build/android/game.so`
   - `config/` (optional: `graphics.ini`, `controllers.ini`)
4. Start the app. It copies `build/game.so` into its own storage (code cannot run from the shared storage) and starts
   the game; the runtime's log is `build/run/witch-time-game.log`, saves go to `build/userdata`. The first start prepares
   the game's shaders behind a progress screen, about two minutes on the Thor; they are kept for the next starts.

`config/env.txt` (optional, `KEY=value` lines) sets environment variables for the runtime, as a shell does on a PC: the
`WT_*` test switches. Copying with `adb push`: it cannot make folders inside the app's folder, so make them with
`adb shell mkdir -p` first.

## Controls

A controller works as on a PC: one paired over Bluetooth or USB, or a handheld's built-in controller. Without one the
app shows touch controls over the game: a stick where the left thumb lands, a drag on the right half for the camera, and
the Switch's buttons where a Switch has them (RS is the right stick press, lock-on). `touch_controls` in
`config/controllers.ini` decides: `auto` (the default) shows them only while no controller is connected and hides them
when one connects, `on` always, `off` never. The app reads it when it comes back to the front. The touch controls are a
controller of their own to the game ("Witch Time touch controls"), so `controllers.ini` can remap them like any pad.

Uninstalling asks whether to keep the app's data: keep it, or the folder above, with the game data, is deleted.

## The GPU driver

On an Adreno GPU the app runs on the Mesa Turnip driver it carries, not the phone's. On the Adreno 740 of the AYN Thor the stock
driver hands some fragment stages wrong inputs from their vertex stages, and most of the world goes missing in a fight (the same
scene is whole on Turnip). Set `gpu_driver = system` in `config/graphics.ini` to use the phone's driver. If Turnip does not reach
its first frames, the next start uses the phone's driver by itself.

The Turnip build has only been tested on the Thor, where it is slower than the stock driver: the GPU stays at the bottom of its clock
range, 401 MHz. The title runs at about 24 fps and a fight's opening at about 20-22. The Thor's performance modes in its system
settings raise that floor, at the cost of heat and battery: 27 fps in the fight at 550 MHz, 29-32 at 615 MHz (the title 30).

## Building the app

```
SDL_TAG=... FFMPEG_TAG=... GLSLANG_TAG=... VULKAN_HEADERS_TAG=... OPUS_TAG=... \
TURNIP_TAG=... TURNIP_ZIP=... TURNIP_SHA256=... ADRENOTOOLS_COMMIT=... android/build_deps.sh <ndk r29> <deps>
LLVM_ROOT=<ndk>/toolchains/llvm/prebuilt/linux-x86_64 CPATH=... WTBUILD_GLSLANG_LIBS=... \
    build/wtbuild runtime --target android --out prebuilt-android
gradle -p android assembleRelease -Pwitchtime.deps=<deps> -Pwitchtime.prebuilt=$PWD/prebuilt-android
```

The tags, `CPATH` and `WTBUILD_GLSLANG_LIBS` are those of job `android` in `.github/workflows/release.yml`, which is the
reference. The runtime build needs SDL3, FFmpeg and libopus copied into `prebuilt-android/` first. The APK is signed with the
debug key for now, so a newer build may need the old one uninstalled first (keep the data when asked).

`WitchTimeActivity.java` is SDL's activity, which loads `libSDL3.so`, then `game.so`, then `libwitchtime.so`, whose
`SDL_main` (`runtime/wt_main.c`) sets up the paths and runs the game. `TouchControls.java` is the touch layer (dusklight's
touch controls were the reference); it registers with SDL through `org/libsdl/app/TouchPadBridge.java`, since SDL's
controller natives are package-private.
