#!/bin/sh
# Finds where the lifted game faults: runs private/build/usfiv under gdb, takes the lifted function of the faulting
# frame, rebuilds only its file with debug info, relinks, runs again and prints the C lines around the fault (each
# has the original x86 instruction as a comment) plus the registers. Extra arguments go to the game (env vars before).
# usage: sh tools/debug_fault.sh
set -u
B=private/build/usfiv
fn=$(timeout 300 gdb -q -batch -ex run -ex bt $B 2>&1 | grep -oE "in f_[0-9A-F]{8}" | head -1 | cut -d' ' -f2)
[ -z "$fn" ] && { echo "no fault in lifted code"; exit 1; }
F=$(grep -l "^void $fn(" private/lifted/lifted_*.c)
O=private/obj/x64/$(basename "$F" .c).o
clang -O2 -g -ffp-contract=off -fwrapv -fno-strict-aliasing -Wno-unused-label -Iruntime/include -Iprivate/lifted -c "$F" -o "$O"
clang -o $B private/build/obj/*.o private/obj/x64/lifted_[0-9]*.o -lm -lpthread -lvulkan -lshaderc_shared $(pkg-config --libs sdl3)
out=$(timeout 300 gdb -q -batch -ex run -ex "info line *\$rip" -ex "info registers rax rbx rcx rdx rsi rdi r8 r9 r10 r11 r12 r13 r14 r15" -ex bt $B 2>&1)
L=$(echo "$out" | grep -oE "^Line [0-9]+" | head -1 | cut -d' ' -f2)
echo "fault in $fn ($F line $L)"
echo "$out" | grep -E "^r[a-z0-9]+ " | tr '\n' ' '; echo
echo "$out" | grep -E "^#" | head -8
sed -n "$((L-12)),$((L+2))p" "$F" | sed -E 's/c->eax = eax.*c->fb = fb;/[sync]/; s/eax = c->eax.*fb = c->fb;/[load]/' | cut -c1-190
