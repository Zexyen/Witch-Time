#!/bin/sh
# The Android builds of what the runtime links against, made with the NDK (CI's job android; also by hand):
#     SDL_TAG=... FFMPEG_TAG=... GLSLANG_TAG=... VULKAN_HEADERS_TAG=... OPUS_TAG=... TURNIP_TAG=... TURNIP_ZIP=... TURNIP_SHA256=... ADRENOTOOLS_COMMIT=... android/build_deps.sh <ndk> <out>
# (the tags are release.yml's env). <out> gets sdl3/, ffmpeg/, glslang/ (static, no optimizer), vulkan-headers/, opus/
# (libopus.so, which the runtime opens itself: nnaudio.c; a PC has the system's) and sdl-java/ (SDL's Java side, which
# the app compiles: it must be SDL3's own version, android/app/build.gradle).
set -eu
NDK=$1 OUT=$2
TC="$NDK/toolchains/llvm/prebuilt/linux-x86_64"
API=29
mkdir -p "$OUT/src"
fetch() { [ -d "$OUT/src/$1" ] || git clone -q --depth 1 --branch "$2" "$3" "$OUT/src/$1"; }
cm() {  # cmake for Android arm64, a static C++ runtime
    src=$1; shift
    cmake -S "$src" -B "$src-build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
        -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_static -DCMAKE_INSTALL_LIBDIR=lib "$@" >/dev/null
}

fetch vulkan-headers "${VULKAN_HEADERS_TAG:?}" https://github.com/KhronosGroup/Vulkan-Headers.git
rm -rf "$OUT/vulkan-headers" && cp -r "$OUT/src/vulkan-headers" "$OUT/vulkan-headers"

fetch sdl "${SDL_TAG:?}" https://github.com/libsdl-org/SDL.git
cm "$OUT/src/sdl" -DCMAKE_INSTALL_PREFIX="$OUT/sdl3" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF \
    -DSDL_ANDROID_JAR=OFF
cmake --build "$OUT/src/sdl-build" && cmake --install "$OUT/src/sdl-build" >/dev/null
cp "$OUT/src/sdl/LICENSE.txt" "$OUT/sdl3/"
rm -rf "$OUT/sdl-java" && cp -r "$OUT/src/sdl/android-project/app/src/main/java" "$OUT/sdl-java"

fetch opus "${OPUS_TAG:?}" https://github.com/xiph/opus.git
cm "$OUT/src/opus" -DCMAKE_INSTALL_PREFIX="$OUT/opus" -DBUILD_SHARED_LIBS=ON -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF
cmake --build "$OUT/src/opus-build" && cmake --install "$OUT/src/opus-build" >/dev/null
cp "$OUT/src/opus/COPYING" "$OUT/opus/"

fetch glslang "${GLSLANG_TAG:?}" https://github.com/KhronosGroup/glslang.git
cm "$OUT/src/glslang" -DCMAKE_INSTALL_PREFIX="$OUT/glslang" -DBUILD_SHARED_LIBS=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DENABLE_OPT=OFF -DGLSLANG_TESTS=OFF -DENABLE_GLSLANG_BINARIES=OFF
cmake --build "$OUT/src/glslang-build" && cmake --install "$OUT/src/glslang-build" >/dev/null
cp "$OUT/src/glslang/LICENSE.txt" "$OUT/glslang/"

# FFmpeg as on Linux (shared, LGPL, decoders and demuxers only); configure's android target names the libraries
# lib<name>.so without a version, as an app's libraries must be
fetch ffmpeg "${FFMPEG_TAG:?}" https://github.com/FFmpeg/FFmpeg.git
cd "$OUT/src/ffmpeg"
./configure --prefix="$OUT/ffmpeg" --enable-cross-compile --target-os=android --arch=aarch64 \
    --cc="$TC/bin/aarch64-linux-android$API-clang" --cxx="$TC/bin/aarch64-linux-android$API-clang++" \
    --nm="$TC/bin/llvm-nm" --ar="$TC/bin/llvm-ar" --ranlib="$TC/bin/llvm-ranlib" --strip="$TC/bin/llvm-strip" \
    --enable-shared --disable-static --enable-pic --disable-autodetect --disable-programs --disable-doc --disable-avdevice \
    --disable-avfilter --disable-network --disable-encoders --disable-muxers --disable-debug >/dev/null
make -j"$(nproc)" >/dev/null && make install >/dev/null
cp COPYING.LGPLv2.1 "$OUT/ffmpeg/"
# The Vulkan driver the app runs on Adreno GPUs (runtime/nvn_vk.c, vk_own_loader): a Mesa Turnip build, and libadrenotools, which loads it in place of the
# phone's. The file is a prebuilt, so its checksum is pinned. Measured on an Adreno 740 (AYN Thor): the stock driver loses most of the world in a fight
if [ ! -f "$OUT/turnip/libvulkan_turnip.so" ]; then
    rm -rf "$OUT/turnip" "$OUT/src/adrenotools" "$OUT/src/adrenotools-build"
    mkdir -p "$OUT/turnip"
    git clone -q https://github.com/bylaws/libadrenotools.git "$OUT/src/adrenotools"
    git -C "$OUT/src/adrenotools" checkout -q "${ADRENOTOOLS_COMMIT:?}"
    git -C "$OUT/src/adrenotools" submodule update -q --init --recursive
    cm "$OUT/src/adrenotools" -DBUILD_SHARED_LIBS=ON
    cmake --build "$OUT/src/adrenotools-build" --target adrenotools main_hook hook_impl
    b="$OUT/src/adrenotools-build"
    cp "$b/libadrenotools.so" "$b/src/hook/libmain_hook.so" "$b/src/hook/libhook_impl.so" "$OUT/turnip/"
    "$TC/bin/llvm-strip" "$OUT"/turnip/*.so
    cp "$OUT/src/adrenotools/LICENSE" "$OUT/turnip/libadrenotools-LICENSE.txt"
    curl -fsSL -o "$OUT/src/turnip.zip" "https://github.com/K11MCH1/AdrenoToolsDrivers/releases/download/${TURNIP_TAG:?}/${TURNIP_ZIP:?}"
    echo "${TURNIP_SHA256:?}  $OUT/src/turnip.zip" | sha256sum -c -
    unzip -q -o -j "$OUT/src/turnip.zip" vulkan.ad07xx.so -d "$OUT/turnip"
    mv "$OUT/turnip/vulkan.ad07xx.so" "$OUT/turnip/libvulkan_turnip.so"
fi
ls "$OUT/sdl3/lib" "$OUT/ffmpeg/lib" "$OUT/glslang/lib" "$OUT/opus/lib" "$OUT/turnip"
