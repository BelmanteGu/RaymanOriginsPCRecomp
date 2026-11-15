#!/bin/sh
# The whole chain for Rayman Origins.exe: function discovery, call-site table, lift, compile check (x86-64 + arm64),
# runtime. ~8 minutes on 12 threads. Output (game-derived) stays in private/.
set -e
cd "$(dirname "$0")/.."
mkdir -p private/analysis
.venv/bin/python -I tools/funcs/discover.py > private/analysis/discover.txt 2>&1
grep -E "^functions|switch_tables |unclaimed|by_origin" private/analysis/discover.txt | head -5
.venv/bin/python -I tools/funcs/callsites.py | head -1
.venv/bin/python -I tools/lift/lift.py > private/lift_census.txt 2>&1; head -1 private/lift_census.txt
sh tools/lift/build_check.sh 12 | head -2
sh runtime/build.sh 2>&1 | tail -1
