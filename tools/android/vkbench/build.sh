#!/bin/sh
# Builds and runs vkbench on the connected phone: blended textured full-screen layers at 1280x720 with three fragment
# shaders (colour centroid as DXVK emits for ps_2_0, colour plain, no colour). Needs glslangValidator, xxd, the NDK.
#   sh tools/android/vkbench/build.sh [layers] [frames]
set -e
cd "$(dirname "$0")"
NDK=${NDK:-$HOME/Library/Android/sdk/ndk/28.2.13676358}
CC=$(ls -d "$NDK"/toolchains/llvm/prebuilt/*/bin/aarch64-linux-android30-clang | head -1)
OUT=${TMPDIR:-/tmp}/vkbench
mkdir -p "$OUT"
for f in quad.vert centroid.frag plain.frag nocolor.frag; do
  glslangValidator -V --target-env vulkan1.1 -o "$OUT/$f.spv" "$f" > /dev/null
  (cd "$OUT" && xxd -i -n "$(echo $f | tr . _)" "$f.spv" > "$f.h")
done
"$CC" -O2 -I"$OUT" -o "$OUT/vkbench" vkbench.c -lvulkan
adb push "$OUT/vkbench" /data/local/tmp/vkbench > /dev/null
adb shell chmod 755 /data/local/tmp/vkbench
adb shell /data/local/tmp/vkbench "${1:-14}" "${2:-60}"
