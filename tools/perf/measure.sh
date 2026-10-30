#!/usr/bin/env bash
# usage: tools/perf/measure.sh <tag> "<env>" [secs=140]: one in-game run on the phone. Saves logcat, a screenshot and
# 20 s of Mali counters (tools/maliprof) in build/perf/<tag>, and prints fps, GPU ms per frame (GTA4_XENIA_GPU_TIME=1)
# and GPU cycles per frame, the metric that does not depend on the GPU clock (it drops when the phone heats up).
export MSYS_NO_PATHCONV=1
T=$1; E=$2; SECS=${3:-140}
D=$(cd "$(dirname "$0")/../.." && pwd)/build/perf/$T; mkdir -p "$D"
adb shell am force-stop com.gta4port
adb logcat -c
adb shell "am start -n com.gta4port/.MainActivity --es env '$E'" >/dev/null
sleep $((SECS-40))
adb shell "/data/local/tmp/maliprof 20 1000" > "$D/gpu.txt" 2>&1 &
sleep 20; adb exec-out screencap -p > "$D/shot.png"; wait
adb logcat -d > "$D/logcat.txt"
adb shell am force-stop com.gta4port
grep -E "gta4gpu: (GPU busy|tight)|VblankThread: [0-9]+ swaps" "$D/logcat.txt" | sed -E 's/^[0-9-]+ ([0-9:.]+) +[0-9]+ +[0-9]+ I (gta4|xenia) *: /\1 /; s/, ring read.*//; s/ interrupts//' > "$D/ab.txt"
A=$(grep -A1 "=== average" "$D/gpu.txt" | grep "GPU active cycles" | awk '{print $4}')
F=$(grep "VblankThread: [0-9]* swaps" "$D/ab.txt" | tail -10 | sed -E 's/.*\(([0-9.]+) fps\).*/\1/' | awk '{s+=$1} END {print s/NR}')
MS=$(grep "GPU busy" "$D/ab.txt" | tail -10 | sed -E 's/.* ([0-9.]+) ms of GPU per frame/\1/' | awk '{s+=$1} END {print s/NR}')
echo "$T: $F fps, $MS ms GPU/frame, GPU active $A cycles/s -> $(awk -v a=$A -v f=$F 'BEGIN{printf "%.1f M cycles/frame", a/f/1e6}'), clock ~$(awk -v a=$A 'BEGIN{printf "%.0f MHz", a/1e6}')" | tee "$D/summary.txt"
# Leave the game running (plain launch, no test options) for the user.
adb shell am start -n com.gta4port/.MainActivity >/dev/null
echo done
