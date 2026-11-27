#!/bin/sh
# Builds vkbench2 and pushes it to the phone: sh tools/android/vkbench/build2.sh, then
#   adb shell /data/local/tmp/vkbench2 tex=bc3 grid=16 draws=130 depth=1 passes=2
set -e
cd "$(dirname "$0")"
NDK=${NDK:-$HOME/Library/Android/sdk/ndk/28.2.13676358}
CC=$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android30-clang | head -1)
OUT=${TMPDIR:-/tmp}/vkbench
mkdir -p "$OUT"
for f in quad2.vert plain.frag; do
  glslangValidator -V --target-env vulkan1.1 -o "$OUT/$f.spv" "$f" > /dev/null
  (cd "$OUT" && xxd -i -n "$(echo $f | tr . _)" "$f.spv" > "$f.h")
done
"$CC" -O2 -I"$OUT" -o "$OUT/vkbench2" vkbench2.c -lvulkan
adb push "$OUT/vkbench2" /data/local/tmp/vkbench2 > /dev/null
adb shell chmod 755 /data/local/tmp/vkbench2
