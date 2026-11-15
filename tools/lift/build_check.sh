#!/bin/sh
# Compiles every lifted file (private/lifted/*.c, or $LIFTED) for the PC (x86-64, host clang) and for Android (arm64, NDK clang),
# in parallel, and counts the files that fail and the distinct errors. A check that the lifter emits valid C on both
# targets; it does not link or run anything (the runtime does). Only files newer than their object are compiled;
# SKIP_A64=1 skips the arm64 build (PC iteration).
# usage: sh tools/lift/build_check.sh [jobs]
set -u
JOBS=${1:-$(nproc)}
LIFTED=${LIFTED:-private/lifted}      # another module: LIFTED=private/lifted_cryptopp OBJ=private/obj_cryptopp
OBJ=${OBJ:-private/obj}
NDK=${NDK:-$HOME/Android/Sdk/ndk/28.2.13676358/toolchains/llvm/prebuilt/linux-x86_64/bin}
FLAGS="-O2 -ffp-contract=off -fwrapv -fno-strict-aliasing -Wno-unused-label -Iruntime/include -I$LIFTED"
mkdir -p $OBJ/x64 $OBJ/a64 $OBJ/log
for o in $OBJ/x64/lifted_[0-9]*.o; do            # objects of files that no longer exist
  [ -e "$o" ] && [ ! -e "$LIFTED/$(basename "$o" .o).c" ] && rm -f "$o" "$OBJ/a64/$(basename "$o")"
done
export FLAGS NDK LIFTED OBJ
start=$(date +%s)
ls $LIFTED/lifted_[0-9]*.c | xargs -P "$JOBS" -I{} sh -c '
  b=$(basename {} .c)
  [ $OBJ/x64/$b.o -nt {} ] || clang $FLAGS -c {} -o $OBJ/x64/$b.o 2> $OBJ/log/$b.x64.log || echo "FAIL x64 $b"
  [ -n "${SKIP_A64:-}" ] || [ $OBJ/a64/$b.o -nt {} ] || $NDK/clang --target=aarch64-linux-android24 $FLAGS -c {} -o $OBJ/a64/$b.o 2> $OBJ/log/$b.a64.log || echo "FAIL a64 $b"
' > $OBJ/log/fails.txt
end=$(date +%s)
n=$(ls $LIFTED/lifted_[0-9]*.c | wc -l)
echo "files: $n, x86-64 objects: $(ls $OBJ/x64/*.o | wc -l), arm64 objects: $(ls $OBJ/a64/*.o | wc -l), failed: $(wc -l < $OBJ/log/fails.txt), $((end - start)) s"
echo "x86-64 .text: $(size -t $OBJ/x64/*.o | tail -1 | awk '{print $1}') bytes, arm64 .text: $(size -t $OBJ/a64/*.o | tail -1 | awk '{print $1}') bytes"
cat $OBJ/log/*.log | grep -E "error:" | sed -E 's/.*error: //' | sort | uniq -c | sort -rn | head -10
