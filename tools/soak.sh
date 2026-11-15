#!/bin/sh
# Unattended long run ("soak"): the game headless and unpaced (faster than real time), with scripted input that gets
# through the menus (Enter) and then plays like a monkey (hold right, jump, attack, sometimes left). Stops found on
# the way (indirect calls to code no function covers) are appended to private/analysis/extra_starts.txt by the
# runtime; run tools/full_pipeline.sh after a soak that found some.
# usage: sh tools/soak.sh <frames> [shot every N frames] [extra env assignments...]
set -u
cd "$(dirname "$0")/.."
N=${1:-20000}; EVERY=${2:-2000}; shift 2 2>/dev/null || shift $#
D=private/soak; mkdir -p $D
IN=""
f=600
while [ $f -lt 3000 ]; do IN="$IN$f-$((f+8)):A;"; f=$((f+120)); done
while [ $f -lt $N ]; do                              # cycles: Enter only (maps, menus), then ~50 s of play
  e=$((f+600)); while [ $f -lt $e ]; do IN="$IN$f-$((f+6)):A;"; f=$((f+100)); done
  e=$((f+3000))
  while [ $f -lt $e ]; do
    IN="$IN$f-$((f+300)):R;$((f+20))-$((f+40)):A;$((f+90))-$((f+130)):A;$((f+170))-$((f+180)):X;$((f+220))-$((f+260)):A;"
    [ $(( (f / 300) % 7 )) -eq 3 ] && IN="$IN$((f+300))-$((f+360)):L;$((f+300))-$((f+310)):A;"
    f=$((f+360))
  done
done
SHOTS=$(seq $EVERY $EVERY $N | tr '\n' ',')
env USF_HEADLESS=1 RAYMAN_FPS=0 USF_INPUT="$IN" USF_SHOT="$SHOTS" USF_SHOT_DIR=$PWD/$D "$@" \
  timeout $((N / 60 + 120)) private/build/rayman > $D/run.log 2>&1
echo "exit $?; frames reached: $(grep -c screenshot $D/run.log) shots"
grep -E "STOP|FATAL|NOT IMPLEMENTED|not lifted" $D/run.log | head -10
