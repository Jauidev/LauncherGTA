// node tools/perf/frames.js <capture prefix> <out.json|out.csv> "<label>" [year]: one row per frame shown
// on screen during a tools/perf/frames.sh capture (prefix build/perf/<tag>/frames), the data of the
// "Rendimiento del port GTA IV" dashboard. Joins the gta4_native_frame_timeline lines (logcat) with
// SurfaceFlinger's --latency rows in order. desiredPresent is the frame pacer's target
// (VK_GOOGLE_display_timing), up to one period after the queue inside the publish, or the queue
// time itself when the pacer sets none, so a row belongs to the next frame whose [submit, publish
// end + period] holds it; frames with no row were dropped by SurfaceFlinger.
// Per shown frame N, with P the previous shown frame:
//   intervalo = shown(N) - shown(P)                 how long P stayed on screen
//   juego     = Present(N) - Present(P)             the game's Present interval(s)
//   cpu       = Present(N) -> publish end           renderer CPU latency
//   gpu       = publish end -> frame ready          GPU completion (SurfaceFlinger's acquire fence)
//   pantalla  = frame ready -> shown
// so intervalo = juego + (cpu + gpu + pantalla)(N) - (cpu + gpu + pantalla)(P) exactly.
const fs = require('fs');
const [, , prefix, out, label = 'captura', year = String(new Date().getFullYear())] = process.argv;
const PERIOD_NS = 34e6;

// Timeline items, and the wall clock of each log line (printed right after its last frame's publish).
const frames = [];
const offsets = [];
const wall = s => {
  const m = s.match(/^(\d\d)-(\d\d) (\d\d):(\d\d):(\d\d)\.(\d{3})/);
  return m ? Date.UTC(Number(year), Number(m[1]) - 1, Number(m[2]), Number(m[3]), Number(m[4]), Number(m[5]), Number(m[6])) : NaN;
};
for (const line of fs.readFileSync(`${prefix}.logcat`, 'utf8').split(/\r?\n/)) {
  const at = line.indexOf('frame-timeline:');
  if (at < 0) continue;
  let last;
  for (const item of line.slice(at + 15).trim().split(/\s+/)) {
    const [frame, rest] = item.split(':');
    if (!rest) continue;
    const [guest, , , , submit, publish] = rest.split(',').map(Number);
    last = { frame: Number(frame), guest, submit, publish };
    frames.push(last);
  }
  const w = wall(line);
  if (last && last.publish >= 0 && Number.isFinite(w)) offsets.push(w - (last.guest + last.publish * 1e3) / 1e6);
}
frames.sort((a, b) => a.frame - b.frame);
offsets.sort((a, b) => a - b);
const wallOffsetMs = offsets[offsets.length >> 1]; // phone wall clock (as UTC fields) - CLOCK_MONOTONIC, ms

const sfRows = new Map();
for (const line of fs.readFileSync(`${prefix}.sf`, 'utf8').split(/\r?\n/)) {
  const m = line.trim().match(/^(\d+)\s+(\d+)\s+(\d+)$/);
  if (!m) continue;
  const [desired, actual, ready] = m.slice(1).map(Number);
  if (!desired || !actual || actual > 9e18 || ready > 9e18) continue;
  sfRows.set(desired, { desired, actual, ready });
}
const sf = [...sfRows.values()].sort((a, b) => a.desired - b.desired);

let j = 0, unmatchedSf = 0;
for (const s of sf) {
  while (j < frames.length && (frames[j].publish < 0 || frames[j].guest + frames[j].publish * 1e3 + PERIOD_NS < s.desired)) j++;
  if (j >= frames.length) break;
  const f = frames[j];
  if (s.desired >= f.guest + f.submit * 1e3 - 2e6) { f.sf = s; j++; } else unmatchedSf++;
}

const iso = ms => new Date(ms).toISOString().slice(0, 23); // the UTC fields hold the phone's local time
const r1 = v => Math.round(v * 10) / 10;
const rows = [];
let prev, dropped = 0, unsplit = 0;
for (const f of frames) {
  if (!f.sf) continue;
  if (prev) {
    // Shown before its frame was ready (seen only in a ~1 s stall): the gpu/screen split is unknown.
    const split = f.sf.actual >= f.sf.ready;
    if (!split) unsplit++;
    dropped += f.frame - prev.frame - 1;
    const publishEnd = f.guest + f.publish * 1e3;
    rows.push({
      captura: label,
      imagen: f.frame,
      hora: iso(f.sf.actual / 1e6 + wallOffsetMs),
      intervalo_ms: r1((f.sf.actual - prev.sf.actual) / 1e6),
      juego_ms: r1((f.guest - prev.guest) / 1e6),
      cpu_ms: r1(f.publish / 1e3),
      gpu_ms: split ? r1((f.sf.ready - publishEnd) / 1e6) : null,
      pantalla_ms: split ? r1((f.sf.actual - f.sf.ready) / 1e6) : null,
      envio_ms: r1(f.submit / 1e3),
      descartadas: f.frame - prev.frame - 1,
    });
  }
  prev = f;
}
if (out.endsWith('.json')) {
  fs.writeFileSync(out, JSON.stringify(rows));
} else {
  const columns = Object.keys(rows[0]);
  fs.writeFileSync(out, [columns.join(','), ...rows.map(r => columns.map(c => r[c] ?? '').join(','))].join('\n') + '\n');
}
console.log(`timeline ${frames.length} frames, SurfaceFlinger ${sf.length} rows (${unmatchedSf} without a frame), ${rows.length} shown frames, ${dropped} dropped between them, ${unsplit} without the gpu/screen split`);
console.log(`wall clock: monotonic + ${(wallOffsetMs / 1e3).toFixed(3)} s (log line spread ${(offsets[Math.floor(offsets.length * 0.9)] - offsets[Math.floor(offsets.length * 0.1)]).toFixed(1)} ms p10-p90)`);
console.log(`${rows[0].hora} .. ${rows[rows.length - 1].hora}`);
