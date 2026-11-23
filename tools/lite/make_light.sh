#!/bin/sh
# The light copy of the player's game on the PC (docs/LIGHT_COPY.md): the same thing the Android launcher makes once.
# Builds tools/lite's two programs, writes <out>/RaymanOrigins/ (program, GameData/bundle_PC.lite, intro / ending /
# sample movies), checks it against the original archive, and stores it in <out>/RaymanOrigins-lite.zip (not
# recompressed: its contents already are) for the launcher's ".zip file" import.
# usage: sh tools/lite/make_light.sh "<Steam game folder>" [out folder, default private/lite_out] [threads, default 4]
set -e
SRC=${1:?usage: make_light.sh <game folder> [out] [threads]}
OUT=${2:-private/lite_out}
T=${3:-4}
B=private/lite
mkdir -p $B "$OUT/RaymanOrigins/GameData"
OPUS="$(pkg-config --cflags --libs opus)"
cc -O2 -DLITE_MAKE_MAIN -Iruntime/src runtime/src/lite_make.c -o $B/lite_make $OPUS -lz -lpthread
cc -O2 -Iruntime/src tools/lite/lite_check.c runtime/src/lite_read.c -o $B/lite_check $OPUS -lz -lpthread -lm

IPK="$SRC/GameData/bundle_PC.ipk"
[ -f "$IPK" ] || { echo "no GameData/bundle_PC.ipk in $SRC"; exit 1; }
cp "$SRC/Rayman Origins.exe" "$OUT/RaymanOrigins/"
for m in intro.bik cine_fin.bik sample.bik; do
    [ -f "$SRC/GameData/$m" ] && cp "$SRC/GameData/$m" "$OUT/RaymanOrigins/GameData/"
done
nice -n 19 $B/lite_make "$IPK" "$OUT/RaymanOrigins/GameData/bundle_PC.lite" "$T"
nice -n 19 $B/lite_check "$IPK" "$OUT/RaymanOrigins/GameData/bundle_PC.lite" | tail -1

rm -f "$OUT/RaymanOrigins-lite.zip"
(cd "$OUT" && python3 -c "
import os, zipfile
with zipfile.ZipFile('RaymanOrigins-lite.zip', 'w', zipfile.ZIP_STORED, allowZip64=True) as z:
    for d, _, fs in os.walk('RaymanOrigins'):
        for f in sorted(fs): z.write(os.path.join(d, f))
")
du -sh "$OUT/RaymanOrigins" "$OUT/RaymanOrigins-lite.zip"
