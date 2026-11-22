#!/bin/sh
# Builds the PC (Linux x86-64) executable: the runtime (runtime/src) linked with the lifted game code
# (private/obj/x64, from tools/lift/build_check.sh) into private/build/rayman. Run it with the game folder:
#   private/build/rayman ["<game folder>"]        (USF_LOG=1 logs every Windows call)
set -e
rm -rf private/build/obj   # no objects of removed sources
mkdir -p private/build/obj
CFLAGS="-O2 -g -Wall -Wno-unused-parameter -ffp-contract=off -fwrapv -fno-strict-aliasing -Iruntime/include -Iruntime/src -Iprivate/include $(pkg-config --cflags sdl3 libavformat)"
# Vulkan headers: private/include/vulkan -> the NDK's copy (same headers for the PC and Android builds)
[ -e private/include/vulkan ] || { mkdir -p private/include; ln -sfn "$HOME/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/vulkan" private/include/vulkan; ln -sfn "$HOME/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/sysroot/usr/include/vk_video" private/include/vk_video; }
[ private/include/touch_art.rgba -nt tools/touch_art.py ] || .venv/bin/python -I tools/touch_art.py   # the touch controls' drawings
for f in runtime/src/*.c runtime/src/d3d9/*.c runtime/src/vk/*.c; do
  clang $CFLAGS -c "$f" -o "private/build/obj/$(basename "$f" .c).o" &
done
wait
clang -O2 -ffp-contract=off -fwrapv -fno-strict-aliasing -Iruntime/include -Iprivate/lifted -c private/lifted/lifted_table.c -o private/build/obj/lifted_table.o
clang -rdynamic -o private/build/rayman private/build/obj/*.o private/obj/x64/lifted_[0-9]*.o -lm -lpthread -lvulkan -lshaderc_shared $(pkg-config --libs sdl3 libavformat libavcodec libswscale libswresample libavutil)
echo "built private/build/rayman"
