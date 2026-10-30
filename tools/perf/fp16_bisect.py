"""Finds the 16-bucket groups of guest pixel shaders that break the image at 16 bits (GTA4_XENIA_FP16=m:<mask>).

Each test starts the game with some buckets relaxed, takes a screenshot in game and calls the image broken when the
game area differs from a correct screenshot of the same scene. Adaptive group testing: a set that renders
correctly is good as a whole; a broken set is split in halves.
Usage: py -3 -I fp16_bisect.py <output dir> <correct screenshot>
"""
import os
import subprocess
import sys
import time

from PIL import Image, ImageChops, ImageStat

OUT = sys.argv[1]
REF = sys.argv[2] if len(sys.argv) > 2 else None


def small(path):
    return Image.open(path).convert("L").crop((300, 100, 2400, 1150)).resize((96, 48), Image.BILINEAR)
ENV = "GTA4_AUTOPRESS=1 GTA4_GPU=xenia GTA4_STATUS=0 GTA4_RES=960x540"
os.makedirs(OUT, exist_ok=True)
log = open(os.path.join(OUT, "bisect.txt"), "a", encoding="utf-8")


def adb(*args, **kwargs):
    env = dict(os.environ, MSYS_NO_PATHCONV="1")
    return subprocess.run(["adb", *args], env=env, capture_output=True, **kwargs)


def brightness(mask):
    adb("shell", "am", "force-stop", "com.gta4port")
    adb("shell", f"am start -n com.gta4port/.MainActivity --es env '{ENV} GTA4_XENIA_FP16=m:{mask:X}'")
    time.sleep(95)
    png = adb("exec-out", "screencap", "-p").stdout
    adb("shell", "am", "force-stop", "com.gta4port")
    path = os.path.join(OUT, f"mask_{mask:04X}.png")
    open(path, "wb").write(png)
    # Mean difference to a correct screenshot of the same scene (correct runs: < 0.8; broken: 5 to 66).
    return ImageStat.Stat(ImageChops.difference(small(REF), small(path))).mean[0]


def good(mask):
    value = brightness(mask)
    ok = value < 2.0
    line = f"mask {mask:04X}: difference {value:.2f} -> {'ok' if ok else 'BROKEN'}"
    print(line, flush=True)
    log.write(line + "\n")
    log.flush()
    return ok


bad = []
pending = [list(range(0, 8)), list(range(8, 16))]
while pending:
    group = pending.pop(0)
    mask = sum(1 << b for b in group)
    if good(mask):
        continue
    if len(group) == 1:
        bad.append(group[0])
        continue
    half = len(group) // 2
    pending += [group[:half], group[half:]]

good_mask = sum(1 << b for b in range(16) if b not in bad)
print(f"broken buckets: {bad}; all good buckets together: mask {good_mask:04X}", flush=True)
log.write(f"broken buckets: {bad}; good mask {good_mask:04X}\n")
print(f"check of the good mask: {'ok' if good(good_mask) else 'BROKEN'}", flush=True)
