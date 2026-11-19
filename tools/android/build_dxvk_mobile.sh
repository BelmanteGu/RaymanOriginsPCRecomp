#!/bin/sh
# Builds the Vulkan 1.1 DXVK (dxvk-native native-1.9.2b with the mobile-GPU patches of ttom795/Skyrim_Android, its
# window system moved to SDL3: private/deps/dxvk-mobile) as libdxvk_d3d9.so, for Android arm64 (default) or for the
# PC (TARGET=host: x86-64, to test the Vulkan 1.1 path on the desktop).
# usage: [TARGET=host] sh tools/android/build_dxvk_mobile.sh
set -e
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
SDL3_SRC=${SDL3_SRC:-$HOME/Documentos/Projetos/Recomps/Castlevania/private/third_party/SDL3-3.4.0}
SDL3_ANDROID=${SDL3_ANDROID:-$HOME/Documentos/Projetos/Recomps/Castlevania/private/build-android/sdl3}
SRC=$PWD/private/deps/dxvk-mobile
export PATH="$PWD/.venv/bin:$PATH"
if [ "${TARGET:-android}" = host ]; then
  B=$PWD/private/build/dxvk-mobile
  cmake -S $SRC -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DSDL3_INCLUDE_DIR="$(pkg-config --variable=includedir sdl3)" -DSDL3_LIBRARY="$(pkg-config --libs sdl3)" > /dev/null
  ninja -C $B 2>&1 | grep -E "error|FAILED|Linking" | head -40
  ls -la $B/libdxvk_d3d9.so
else
  B=$PWD/private/android/dxvk-mobile
  OUT=$PWD/private/android/lib/arm64-v8a
  cmake -S $SRC -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DANDROID_STL=c++_static \
    -DSDL3_INCLUDE_DIR=$SDL3_SRC/include -DSDL3_LIBRARY=$SDL3_ANDROID/libSDL3.so \
    -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" > /dev/null
  ninja -C $B 2>&1 | grep -E "error|FAILED|Linking" | head -40
  cp $B/libdxvk_d3d9.so $OUT/libdxvk_d3d9.so
  $NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip --strip-unneeded $OUT/libdxvk_d3d9.so
  ls -la $OUT/libdxvk_d3d9.so
fi
