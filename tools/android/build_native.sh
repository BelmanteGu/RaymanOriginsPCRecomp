#!/bin/sh
# Builds the Android arm64 native library private/android/lib/arm64-v8a/libmain.so: the runtime (runtime/src, with
# FFmpeg for the Bink movies: tools/android/build_ffmpeg.sh) + the lifted game (private/obj/a64, from
# tools/lift/build_check.sh) + the call-site table linked in + shaderc (built from the NDK's sources) and SDL3
# (libSDL3.so, copied next to it). Everything derived from the game stays in private/.
# usage: sh tools/android/build_native.sh
# needs: NDK r28c, private/android/shaderc (see below), an SDL3 3.4 Android build (SDL3_ANDROID=dir with libSDL3.so,
#        SDL3_SRC=the SDL3 source tree, for its headers and Java files)
set -e
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
SDL3_SRC=${SDL3_SRC:-$HOME/Documentos/Projetos/Recomps/Castlevania/private/third_party/SDL3-3.4.0}
SDL3_ANDROID=${SDL3_ANDROID:-$HOME/Documentos/Projetos/Recomps/Castlevania/private/build-android/sdl3}
OUT=private/android/lib/arm64-v8a
OBJ=private/android/obj
CC="$TC/clang --target=aarch64-linux-android29"
CXX="$TC/clang++ --target=aarch64-linux-android29"

# shaderc (GLSL -> SPIR-V at run time), once: ndk-build of $NDK/sources/third_party/shaderc
SHADERC=private/android/shaderc/out/local/arm64-v8a/libshaderc_combined.a
if [ ! -f $SHADERC ]; then
  mkdir -p private/android/shaderc
  (cd private/android/shaderc && $NDK/ndk-build NDK_PROJECT_PATH=. APP_BUILD_SCRIPT=$NDK/sources/third_party/shaderc/Android.mk \
     APP_STL=c++_static APP_ABI=arm64-v8a APP_PLATFORM=android-29 NDK_APP_OUT=out libshaderc_combined -j12 > build.log)
fi

rm -rf $OBJ; mkdir -p $OBJ $OUT
FF=private/android/ffmpeg/out                 # tools/android/build_ffmpeg.sh (movies); without it, movies end at once
if [ -f $FF/lib/libavcodec.a ]; then FFC="-I$FF/include"; FFL="$FF/lib/libavformat.a $FF/lib/libavcodec.a $FF/lib/libswscale.a $FF/lib/libswresample.a $FF/lib/libavutil.a"
else FFC="-DUSF_NO_FFMPEG"; FFL=""; fi
CFLAGS="-O2 -g -fPIC -Wall -Wno-unused-parameter -ffp-contract=off -fwrapv -fno-strict-aliasing $FFC
        -Iruntime/include -Iruntime/src -I$SDL3_SRC/include -I$NDK/sources/third_party/shaderc/libshaderc/include"
for f in runtime/src/*.c runtime/src/d3d9/*.c runtime/src/vk/*.c; do
  $CC $CFLAGS -c "$f" -o "$OBJ/$(basename "$f" .c).o" &
done
wait
n_src=$(ls runtime/src/*.c runtime/src/d3d9/*.c runtime/src/vk/*.c | wc -l)
[ "$(ls $OBJ/*.o | wc -l)" -eq "$n_src" ] || { echo "error: a runtime file did not compile"; exit 1; }
LFLAGS="-O2 -fPIC -ffp-contract=off -fwrapv -fno-strict-aliasing -Iruntime/include"
$CC $LFLAGS -Iprivate/lifted -c private/lifted/lifted_table.c -o $OBJ/lifted_table.o
# the call-site argument table (tools/funcs/callsites.py), linked in: there is no private/ folder on the phone
cat > $OBJ/callsites_blob.S <<EOF
  .section .rodata
  .global usf_callsites_blob, usf_callsites_blob_end
  .balign 8
usf_callsites_blob:
  .incbin "$PWD/private/analysis/callsite_args.bin"
usf_callsites_blob_end:
EOF
$CC -c $OBJ/callsites_blob.S -o $OBJ/callsites_blob.o
$CXX -shared -o $OUT/libmain.so $OBJ/*.o private/obj/a64/lifted_[0-9]*.o $SHADERC $FFL \
  -L$SDL3_ANDROID -lSDL3 -lvulkan -landroid -llog -lm -static-libstdc++ -Wl,--gc-sections -Wl,-z,max-page-size=16384 -Wl,--exclude-libs,ALL
cp $SDL3_ANDROID/libSDL3.so $OUT/
[ -n "${USF_KEEP_SYMBOLS:-}" ] || $TC/llvm-strip --strip-debug $OUT/libmain.so
ls -la $OUT
