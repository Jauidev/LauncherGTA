#!/usr/bin/env bash
# usage: tools/perf/frames.sh <tag> [secs=270]: records every frame shown while the game runs on the phone, launched with
# gta4_native_frame_timeline=true in GTA4_NATIVE_CVARS (start it once gameplay has begun). Polls SurfaceFlinger's
# per-frame present times every 3 s (a layer keeps its last 128 frames), then saves logcat (the frame-timeline lines),
# in build/perf/<tag>/frames.{sf,logcat,meta}. Then: node tools/perf/frames.js build/perf/<tag>/frames <out.json> "<label>"
export MSYS_NO_PATHCONV=1
T=$1; SECS=${2:-270}
D=$(cd "$(dirname "$0")/../.." && pwd)/build/perf/$T; mkdir -p "$D"
OUT=$D/frames
# The game's buffers go to the SurfaceView's BLAST layer (the one BufferQueueProducer logs), not to the activity's window.
layer=$(adb shell dumpsys SurfaceFlinger --list | tr -d '\r' | grep -F "SurfaceView[com.gta4port/" | grep -F "(BLAST)" | grep -v Background | head -1 | sed -E 's/^RequestedLayerState\{//; s/ parentId=.*$//; s/\}$//')
[ -n "$layer" ] || { echo "no SurfaceView layer: is the game running?"; exit 1; }
{ echo "layer: $layer"; echo "start: $(adb shell 'date "+%m-%d %H:%M:%S"')"; } > "$OUT.meta"
: > "$OUT.sf"
end=$(( $(date +%s) + SECS ))
while [ "$(date +%s)" -lt "$end" ]; do
  timeout 10 adb shell "dumpsys SurfaceFlinger --latency '$layer'" >> "$OUT.sf"
  sleep 3
done
echo "end: $(adb shell 'date "+%m-%d %H:%M:%S"')" >> "$OUT.meta"
adb logcat -d > "$OUT.logcat"
echo "frame-timeline lines: $(grep -c 'frame-timeline:' "$OUT.logcat"), SurfaceFlinger rows: $(grep -cE '^[0-9]+\s+[0-9]+\s+[0-9]+' "$OUT.sf")"
