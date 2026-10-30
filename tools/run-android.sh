#!/usr/bin/env bash
# Phase 2 on the phone: pushes build/arm64/gta4 and runs it from adb shell (no APK yet).
# The disc must already be in /data/local/tmp/gta4/game (adb push game /data/local/tmp/gta4/).
# Usage: tools/run-android.sh [seconds] [log]   (default 60 s, analysis/android-run.log)
# Output: stderr of the process into the log; the same lines also go to logcat, tag "gta4".
set -uo pipefail
export MSYS_NO_PATHCONV=1
# pwd -W: a C:/ path, since adb is a Windows program and MSYS_NO_PATHCONV stops the conversion.
ROOT="$(cd "$(dirname "$0")/.." && pwd -W)"
SECONDS_TO_RUN=${1:-60}
LOG=${2:-$ROOT/analysis/android-run.log}
DIR=/data/local/tmp/gta4

adb push "$ROOT/build/arm64/gta4" "$DIR/gta4" >/dev/null || exit 1
adb shell chmod 755 "$DIR/gta4"
adb logcat -c
# timeout(1) from toybox ends the run; GTA4_DUMP_AFTER prints every guest thread just before.
# Other GTA4_* variables set here (GTA4_AUTOPRESS, GTA4_LOG_FILES...) are passed through.
EXTRA=$(env | grep -E '^GTA4_(AUTOPRESS|LOG_FILES|LANGUAGE)=' | tr '\n' ' ')
adb shell "cd $DIR && $EXTRA GTA4_GAME_DIR=$DIR/game GTA4_DUMP_AFTER=$((SECONDS_TO_RUN - 5)) timeout $SECONDS_TO_RUN ./gta4" > "$LOG" 2>&1
echo "exit $? -> $LOG"
