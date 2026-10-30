"""Summarizes GTA4_XENIA_PASS_TIME logcat lines: time per pass, grouped by render pass kind.
Usage: py -3 -I passtime.py logcat.txt
"""
import collections
import re
import sys

FMT = {130: "D32S8", 129: "D24S8", 37: "RGBA8", 44: "BGRA8", 64: "A2B10G10R10", 70: "R16", 77: "RG16", 78: "RG16S",
       83: "RG16F", 91: "RGBA16", 92: "RGBA16S", 97: "RGBA16F", 98: "R32UI", 100: "R32F", 103: "RG32F",
       109: "RGBA32F", 122: "B10G11R11"}
rps = {}
passes = []
summary = []
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = re.search(r"gta4gpu: render pass ([0-9A-F]+):(.*)", line)
    if m:
        atts = []
        for a in m.group(2).split():
            f, s = a.split("x")
            atts.append(f"{FMT.get(int(f), f)}x{s}")
        rps[m.group(1)] = " ".join(atts) or "(no attachments)"
        continue
    m = re.search(r"gta4gpu: frame (\d+) pass (\d+): ([\d.]+) ms, before it ([\d.]+) ms \((\d+) dispatches, "
                  r"(\d+) copies, (\d+) barriers\), rp ([0-9A-F]+) fb ([0-9A-F]+) area (-?\d+),(-?\d+) (\d+)x(\d+), "
                  r"(\d+) draws, (\d+) clears", line)
    if m:
        g = m.groups()
        passes.append(dict(frame=int(g[0]), i=int(g[1]), ms=float(g[2]), before=float(g[3]), disp=int(g[4]),
                           copies=int(g[5]), barriers=int(g[6]), rp=g[7], area=f"{g[11]}x{g[12]}",
                           draws=int(g[13]), clears=int(g[14])))
        continue
    m = re.search(r"gta4gpu: frame (\d+): .*", line)
    if m:
        summary.append(m.group(0))

for s in summary:
    print(s)
for frame in sorted(set(p["frame"] for p in passes)):
    fp = [p for p in passes if p["frame"] == frame]
    tot = sum(p["ms"] + p["before"] for p in fp)
    print(f"\n=== frame {frame}: {len(fp)} passes, {tot:.1f} ms (passes {sum(p['ms'] for p in fp):.1f}, "
          f"between {sum(p['before'] for p in fp):.1f}, dispatches {sum(p['disp'] for p in fp)})")
    groups = collections.defaultdict(lambda: [0, 0.0, 0])
    for p in fp:
        kind = "draws" if p["draws"] >= 3 else ("1-2 draws" if p["draws"] else "clear/none")
        k = (rps.get(p["rp"], p["rp"]), kind)
        groups[k][0] += 1
        groups[k][1] += p["ms"]
        groups[k][2] += p["draws"]
    print("  by render pass kind:")
    for k, (n, ms, d) in sorted(groups.items(), key=lambda kv: -kv[1][1])[:25]:
        print(f"   {ms:7.2f} ms {n:4} passes {d:5} draws  {k[1]:10} {k[0]}")
    print("  slowest passes:")
    for p in sorted(fp, key=lambda p: -p["ms"])[:30]:
        print(f"   #{p['i']:3} {p['ms']:7.2f} ms (before {p['before']:5.2f}, {p['disp']} disp) {p['area']:>10} "
              f"draws {p['draws']:3} clears {p['clears']} {rps.get(p['rp'], p['rp'])}")
    print("  biggest gaps (work between passes):")
    for p in sorted(fp, key=lambda p: -p["before"])[:10]:
        print(f"   before #{p['i']:3}: {p['before']:6.2f} ms, {p['disp']} dispatches, {p['copies']} copies, "
              f"{p['barriers']} barriers")

# Work between passes (dispatches, copies, barriers), from the per-event timer.
work = collections.defaultdict(list)
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    m = re.search(r"gta4gpu: frame (\d+) work (.*): (\d+) times, ([\d.]+) ms", line)
    if m:
        work[int(m.group(1))].append((float(m.group(4)), int(m.group(3)), m.group(2)))
for frame, items in sorted(work.items()):
    print(f"\n=== frame {frame} work outside passes: {sum(i[0] for i in items):.1f} ms")
    for ms, n, label in sorted(items, reverse=True):
        print(f"   {ms:7.2f} ms {n:4}x  {label}")
