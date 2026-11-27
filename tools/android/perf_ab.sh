#!/usr/bin/env bash
# Automatic A/B performance run on a phone over adb: each variant restarts the game, walks to the game's main menu
# (launcher title -> Play -> "Press ENTER"), lets it settle, then reads the runtime's own numbers from logcat
# (usf fps line, GPU counters through USF_GPU_CLOCK, DXVK_PASSLOG) and prints one line per variant.
#
#   sh tools/android/perf_ab.sh                       # the default matrix (baseline, 360 lines, FSR off, touch off)
#   sh tools/android/perf_ab.sh "base" "lines=360" "env:USF_FSR=0" ...
#
# A variant is a label followed by changes separated by spaces inside one argument:
#   "name|lines=360 touch=0 env:USF_FSR=0"   (lines=/touch=/... go to port_settings.txt, env:K=V to env.txt)
# port_settings.txt is restored and env.txt removed at the end (also on Ctrl+C).
set -u
PKG=io.github.belmantegu.raymanpc
DATA=/sdcard/Android/data/$PKG/files/data
SETTLE=${SETTLE:-12}        # seconds in the menu before measuring
MEASURE=${MEASURE:-10}      # seconds measured
OUT=${OUT:-perf_ab_$(date +%Y%m%d_%H%M%S).txt}
TMP=$(mktemp -d)
BASE_ENV="USF_GPU_CLOCK=1
USF_GPU_BOOST=1
USF_GPU_COUNTERS=${COUNTERS:-11:29,11:12,11:13,11:14,11:15,11:20,11:21,11:5,24:9,24:10,24:5,24:7,10:0,10:1,10:6,25:17}
DXVK_PASSLOG=1"

adb get-state >/dev/null 2>&1 || { echo "no device (adb)"; exit 1; }
adb pull "$DATA/port_settings.txt" "$TMP/settings.orig" >/dev/null || { echo "no port_settings.txt (run the game once)"; exit 1; }

restore() {
    adb push "$TMP/settings.orig" "$DATA/port_settings.txt" >/dev/null 2>&1
    adb shell rm -f "$DATA/env.txt" >/dev/null 2>&1
    adb shell am force-stop $PKG >/dev/null 2>&1
    rm -rf "$TMP"
}
trap 'restore; exit 130' INT TERM

# the screen in landscape (the launcher and the game are landscape)
read -r W H < <(adb shell wm size | sed -n 's/.*: \([0-9]*\)x\([0-9]*\).*/\1 \2/p' | tail -1)
[ "$W" -lt "$H" ] && { t=$W; W=$H; H=$t; }
tap() { adb shell input tap $(awk -v w="$W" -v h="$H" -v x="$1" -v y="$2" 'BEGIN{printf "%d %d", w*x, h*y}'); }

wait_log() {   # wait_log <regex> <seconds>
    local i=0
    while [ $i -lt "$2" ]; do
        adb logcat -d -s 'rayman:*' 2>/dev/null | grep -qE "$1" && return 0
        sleep 1; i=$((i + 1))
    done
    return 1
}

run_variant() {
    local label="$1" changes="$2"
    cp "$TMP/settings.orig" "$TMP/settings"
    changes="lines=${LINES:-540} $changes"     # every test at the Medium mode (540 lines in the screen's shape) unless LINES= says otherwise
    printf '%s\n' "$BASE_ENV" > "$TMP/env"
    for c in $changes; do
        case "$c" in
            env:*) printf '%s\n' "${c#env:}" >> "$TMP/env" ;;
            *=*)   k=${c%%=*}; grep -v "^$k=" "$TMP/settings" > "$TMP/s2"; printf '%s\n' "$c" >> "$TMP/s2"; mv "$TMP/s2" "$TMP/settings" ;;
        esac
    done
    adb push "$TMP/settings" "$DATA/port_settings.txt" >/dev/null
    adb push "$TMP/env" "$DATA/env.txt" >/dev/null
    adb shell input keyevent KEYCODE_WAKEUP; adb shell svc power stayon usb >/dev/null 2>&1   # the screen on (a dozing phone shows nothing)
    adb shell am force-stop $PKG; sleep 1
    adb logcat -c
    adb shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1
    sleep 5
    tap 0.50 0.42; sleep 2          # launcher title -> home
    tap 0.50 0.508                  # Play
    if ! wait_log 'usf: [0-9.]+ fps' 90; then echo "$label: the game did not start" | tee -a "$OUT"; return; fi
    sleep 8
    adb shell input keyevent KEYCODE_ENTER            # "Press ENTER" -> the game's main menu
    sleep "$SETTLE"
    adb logcat -d -s 'rayman:*' > "${OUT%.txt}_${label}_start.log"   # the run up to the menu (texture / shader lists)
    adb logcat -c
    sleep "$MEASURE"
    adb logcat -d -s 'rayman:*' > "$TMP/log"
    cp "$TMP/log" "${OUT%.txt}_${label}_measure.log"
    adb exec-out screencap -p > "${OUT%.txt}_$label.png" 2>/dev/null
    awk -v label="$label" -v changes="$changes" '
        /usf: [0-9.]+ fps/ { for (i = 1; i <= NF; i++) { if ($(i+1) == "fps,") { fps += $i; nf++ }
                             if ($(i) == "cpu") { cpu += $(i+1); nc++ } } }
        /gpu clock: [0-9]+ MHz/ { for (i = 1; i <= NF; i++) if ($(i+1) == "MHz") { m = $i + 0; if (m > 50 && m < 2000) { mhz += m; nm++ } } }
        /gpu counter [0-9]+:[0-9]+:/ { id = $0; sub(/.*gpu counter /, "", id); sub(/:[^:]*$/, "", id)
                             for (i = 1; i <= NF; i++) if ($(i+1) == "M") { v = $i + 0; if (v < 5000) { cs[id] += v; cn[id]++; if (id == "11:29") { px += v; np++ } } }
                             for (i = 1; i <= NF; i++) if ($i ~ /%$/ && $(i+1) == "of") { pc = $i + 0; if (pc <= 1000) { cp[id] += pc } } }
        /passlog: +[0-9.]+\/s/ { line = $0; sub(/.*passlog: +/, "", line); if (!(line in seen)) { seen[line] = 1; passes = passes "\n    " line } }
        /depth yes/ && !scene { for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+x[0-9]+$/) { split($i, d, "x"); scene = d[1] * d[2] } }
        END {
            f = nf ? fps / nf : 0; pxs = np ? px / np : 0; pf = (f > 0) ? pxs / f : 0
            printf "%-12s %5.1f fps  cpu %4.1f ms  gpu %4.0f MHz  RB %6.1f Mpx/s = %5.2f Mpx/frame", label, f, nc ? cpu / nc : 0, nm ? mhz / nm : 0, pxs, pf
            if (scene) printf " = %4.1f x scene (%d px)", pf * 1e6 / scene, scene
            printf "   [%s]\n", changes
            split("11:29 RB_3D_PIXELS|11:12 RB_Z_READ|11:13 RB_Z_WRITE|11:14 RB_C_READ|11:15 RB_C_WRITE|11:20 RB_BLENDED_FXP|11:21 RB_BLENDED_FP16|11:5 RB_STARVE_SP|24:9 CCU_GMEM_READ|24:10 CCU_GMEM_WRITE|24:5 CCU_COLOR_BLOCKS|24:7 CCU_COLOR_BLOCK_HIT|10:0 SP_BUSY|10:1 SP_ALU_WORKING|10:6 SP_STALL_RB|25:17 LRZ_TOTAL_PIXEL|11:0 RB_BUSY|11:31 RB_ZPROC|11:32 RB_CPROC|11:30 RB_BLENDER", names, "|")
            for (k in names) { split(names[k], nn, " "); nmap[nn[1]] = nn[2] }
            for (id in cs) printf "    %-6s %-20s %8.3f M/frame  %6.1f%% of cycles\n", id, (id in nmap ? nmap[id] : "?"), (f > 0 ? cs[id] / cn[id] / f : 0), cp[id] / cn[id]
        }' "$TMP/log" | tee -a "$OUT"
    grep 'passlog: ' "$TMP/log" | tail -5 | sed 's/.*passlog:/    passlog:/' >> "$OUT"
}

if [ $# -eq 0 ]; then
    set -- "max|"
fi
echo "device: $(adb shell getprop ro.product.model) / $(adb shell cat /sys/class/kgsl/kgsl-3d0/gpu_model 2>/dev/null)  screen ${W}x${H}" | tee "$OUT"
for v in "$@"; do run_variant "${v%%|*}" "${v#*|}"; done
restore
trap - INT TERM
echo "results: $OUT"
