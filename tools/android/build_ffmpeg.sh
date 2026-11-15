#!/bin/sh
# A minimal FFmpeg for Android arm64, only what the game's movies need: the Bink demuxer, the Bink video and Bink
# audio decoders, swscale and swresample. Static libraries in private/android/ffmpeg/out.
# usage: sh tools/android/build_ffmpeg.sh   (the FFmpeg 7.1 source in private/android/ffmpeg/ffmpeg-7.1, from ffmpeg.org)
set -e
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}
TC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
SRC=private/android/ffmpeg/ffmpeg-7.1
OUT=$PWD/private/android/ffmpeg/out
cd $SRC
make distclean > /dev/null 2>&1 || true
./configure --prefix=$OUT --target-os=android --arch=aarch64 --enable-cross-compile \
  --cc=$TC/aarch64-linux-android29-clang --cxx=$TC/aarch64-linux-android29-clang++ --ar=$TC/llvm-ar \
  --ranlib=$TC/llvm-ranlib --strip=$TC/llvm-strip --nm=$TC/llvm-nm \
  --enable-static --disable-shared --enable-pic --disable-programs --disable-doc --disable-network \
  --disable-everything --disable-autodetect \
  --enable-demuxer=bink --enable-decoder=bink,binkaudio_dct,binkaudio_rdft --enable-protocol=file \
  --enable-swscale --enable-swresample --disable-avdevice --disable-avfilter --disable-postproc \
  --extra-cflags="-O2" > configure.log
make -j12 > make.log 2>&1
make install > install.log 2>&1
ls $OUT/lib
