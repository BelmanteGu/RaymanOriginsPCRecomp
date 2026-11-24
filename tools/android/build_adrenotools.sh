#!/bin/sh
# Builds libadrenotools (BSD-2-Clause, bylaws/libadrenotools; the copy in private/deps/libadrenotools) for Android arm64:
# the static library the runtime links (a Vulkan driver of the player's, Turnip, instead of the system's; and BCeNabler,
# which lets an older Adreno driver show the BC/DXT texture formats its hardware has) and the hook libraries that must be
# shipped beside libmain.so (private/android/lib/arm64-v8a). Run once; tools/android/build_native.sh uses the result.
# usage: sh tools/android/build_adrenotools.sh
set -e
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
S=private/deps/libadrenotools
B=private/android/adrenotools
[ -d $S ] || { echo "missing $S (bylaws/libadrenotools with lib/linkernsbypass)"; exit 1; }
cmake -S $S -B $B -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=29 \
  -DANDROID_STL=c++_static -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_SHARED_LINKER_FLAGS="-Wl,-z,max-page-size=16384" > $B.configure.log
ninja -C $B -j4 > $B.build.log
mkdir -p private/android/lib/arm64-v8a
cp $B/src/hook/lib*_hook.so $B/src/hook/libhook_impl.so private/android/lib/arm64-v8a/
ls -la $B/libadrenotools.a $B/lib/linkernsbypass/liblinkernsbypass.a private/android/lib/arm64-v8a/lib*hook*.so
