#!/bin/sh
# Builds libopus (BSD licence, xiph.org) for Android arm64 as a static library: private/android/opus/out/{include,lib}.
# The compressed game archive (runtime/src/lite_*.c) stores the game's sounds as Opus: the launcher encodes them once
# (libpack.so) and the game decodes them (libmain.so). Run once; tools/android/build_native.sh uses the result.
# usage: sh tools/android/build_opus.sh
set -e
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
V=1.5.2
D=private/android/opus
mkdir -p $D
[ -f $D/opus-$V.tar.gz ] || curl -sSL -o $D/opus-$V.tar.gz https://downloads.xiph.org/releases/opus/opus-$V.tar.gz \
  || curl -sSL -o $D/opus-$V.tar.gz https://github.com/xiph/opus/releases/download/v$V/opus-$V.tar.gz   # the same release on GitHub
[ -d $D/opus-$V ] || tar -xzf $D/opus-$V.tar.gz -C $D
cmake -S $D/opus-$V -B $D/build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=29 \
  -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_BUILD_TESTING=OFF -DOPUS_BUILD_SHARED_LIBRARY=OFF -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX=$PWD/$D/out > $D/configure.log
ninja -C $D/build -j4 install > $D/build.log
ls -la $D/out/lib/libopus.a
