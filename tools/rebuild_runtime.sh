#!/bin/sh
# Rebuilds only the runtime (runtime/src) and relinks private/build/rayman with the lifted objects already built by
# tools/lift/build_check.sh. Prints errors and warnings only.
cd "$(dirname "$0")/.." && sh runtime/build.sh 2>&1 | grep -E "error|warning:|undefined reference|built" | grep -v "unused-but-set" | head -40
