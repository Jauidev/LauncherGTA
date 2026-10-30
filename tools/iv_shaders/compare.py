#!/usr/bin/env python3
"""Pairs the containers a game registers but its SPIR-V cache lacks with the cache's unused entries.

The cache (LibertyRecompLib/shader/shader_cache.cpp) holds TU8's shaders by the hash of the whole
container. A container built from another tree hashes differently although the program is the same.
Each dumped container (GTA4_SHADER_DUMP) is compiled with XenosRecomp + DXC and compared to the
cached SPIR-V through a fingerprint: opcode histogram, float constants, descriptor bindings and
raw-buffer-load offsets. Registered containers that are in the cache act as the control.

    py -3 -I tools/iv_shaders/compare.py <dump>/gta4_containers <work dir>
"""
import collections
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TOOL = os.path.join(ROOT, "build", "host", "iv_shaders", "iv_shader_tool.exe")
DXC = os.path.join(ROOT, "third_party", "rexglue", "thirdparty", "dxc-bin", "bin", "x64", "dxc.exe")
CACHE = os.path.join(ROOT, "third_party", "rexglue", "LibertyRecompLib", "shader", "shader_cache.cpp")
COMMON = os.path.join(ROOT, "third_party", "rexglue", "LibertyRecompLib", "shader_overrides", "xenos_shader_common.h")
ENTRY = re.compile(r'\{ 0x([0-9A-F]+), \d+, \d+, (\d+), (\d+), (\d+), (\d+), \d+, \d+, (\d+), "([^"]+)", nullptr, (\d+) \}')

OP_NAME, OP_MEMBER_NAME, OP_STRING, OP_SOURCE, OP_LINE = 5, 6, 7, 3, 8
OP_CONSTANT, OP_DECORATE, OP_NOP = 43, 71, 0
DEC_BINDING, DEC_SET = 33, 34


def read_cache():
    entries = {}
    blob = None
    with open(CACHE, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = ENTRY.search(line[:600])
            if m:
                entries[f"{int(m[1], 16):016X}"] = dict(offset=int(m[2]), size=int(m[3]), late=int(m[5]),
                                                        name=m[7], mask=int(m[8]))
            elif line.startswith("const uint8_t g_compressedSpirvCache"):
                blob = bytes(int(x) for x in line[line.index("{") + 1:line.rindex("}")].split(",") if x.strip())
    return entries, blob


def fingerprint(path):
    data = open(path, "rb").read()
    words = struct.unpack(f"<{len(data) // 4}I", data[:len(data) // 4 * 4])
    ops = collections.Counter()
    constants = collections.Counter()
    bindings = collections.Counter()
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        if count == 0:
            break
        if op not in (OP_NAME, OP_MEMBER_NAME, OP_STRING, OP_SOURCE, OP_LINE, OP_NOP):
            ops[op] += 1
        if op == OP_CONSTANT:
            constants[tuple(words[i + 3:i + count])] += 1
        if op == OP_DECORATE and words[i + 2] in (DEC_BINDING, DEC_SET):
            bindings[(words[i + 2], words[i + 3])] += 1
        i += count
    return dict(ops=ops, constants=constants, bindings=bindings, size=len(words))


def distance(a, b):
    """0 when equal: the share of opcodes and constants that differ."""
    def diff(x, y):
        total = sum((x | y).values()) or 1
        return sum(((x - y) + (y - x)).values()) / total
    return diff(a["ops"], b["ops"]) + 2 * diff(a["constants"], b["constants"])


def compile_container(path, work, stem):
    hlsl = os.path.join(work, stem + ".hlsl")
    spv = os.path.join(work, stem + ".spv")
    if os.path.exists(spv):
        return spv
    out = subprocess.run([TOOL, "hlsl", COMMON, path, hlsl], capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError(f"{stem}: recompile failed {out.stderr[:200]}")
    pixel = out.stdout.startswith("pixel")
    cmd = [DXC, "-spirv", "-fspv-target-env=vulkan1.0", "-HV", "2021", "-T", "ps_6_0" if pixel else "vs_6_0",
           "-E", "shaderMain", "-all-resources-bound", "-fvk-use-dx-layout", "-Qstrip_debug", "-Fo", spv, hlsl]
    cmd.append("-DXENOS_RECOMP_PIXEL_SHADER" if pixel else "-fvk-invert-y")
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode:
        raise RuntimeError(f"{stem}: dxc failed {out.stderr[:400]}")
    return spv


def main():
    dump, work = sys.argv[1], sys.argv[2]
    os.makedirs(work, exist_ok=True)
    cached_dir = os.path.join(work, "cached")
    os.makedirs(cached_dir, exist_ok=True)
    entries, blob = read_cache()
    open(os.path.join(work, "blob.zst"), "wb").write(blob)
    with open(os.path.join(work, "list.txt"), "w") as f:
        for h, e in entries.items():
            f.write(f"{h} {e['offset']} {e['size']}\n")
    subprocess.run([TOOL, "cache", os.path.join(work, "blob.zst"), os.path.join(work, "list.txt"), cached_dir], check=True)

    registered = {}
    for line in open(os.path.join(dump, "order.txt")):
        h, st = line.split()
        registered.setdefault(h, st)
    missing = [h for h in registered if h not in entries]
    unused = [h for h in entries if h not in registered]
    cached = {h: fingerprint(os.path.join(cached_dir, h + ".spv")) for h in entries}

    # Control: known pairs, the distance a true match has.
    control = [h for h in list(registered)[::25] if h in entries]
    distances = []
    for h in control:
        try:
            spv = compile_container(os.path.join(dump, f"gta4_{h}.container.{registered[h]}"), work, h)
        except RuntimeError as error:
            print("control skipped:", str(error)[:150])
            continue
        d = distance(fingerprint(spv), cached[h])
        best = min((u for u in entries if u != h), key=lambda u: distance(fingerprint(spv), cached[u]))
        distances.append((d, h, distance(fingerprint(spv), cached[best]), entries[h]["name"]))
    distances.sort()
    print("control, %d cached shaders: true-pair distance median %.3f max %.3f; best other pair at max %.3f" % (
        len(distances), distances[len(distances) // 2][0], distances[-1][0], min(x[2] for x in distances)))
    for d, h, other, name in distances[-3:]:
        print(f"  worst {h} true {d:.3f} other {other:.3f} {name}")

    print("\nmissing -> closest unused cache entry (distance; next closest)")
    pairs = {}
    for h in missing:
        path = os.path.join(dump, f"gta4_{h}.container.{registered[h]}")
        fp = fingerprint(compile_container(path, work, h))
        ranked = sorted(((distance(fp, cached[u]), u) for u in unused
                         if ("_vs" in entries[u]["name"] or ".vert" in entries[u]["name"]) == (registered[h] == "vert")))
        pairs[h] = ranked[0][1]
        data = open(path, "rb").read()
        label = " ".join(s.decode() for s in re.findall(rb"[A-Za-z_][A-Za-z0-9_]{2,}", data)[5:7])
        print(f"{h} {registered[h]} -> {ranked[0][1]} {ranked[0][0]:.3f}; {ranked[1][1]} {ranked[1][0]:.3f}  "
              f"{label}  mask={entries[ranked[0][1]]['mask']:08X} {entries[ranked[0][1]]['name'][-40:]}")
    if len(set(pairs.values())) != len(pairs):
        print("WARNING: two containers took the same cache entry")


main()
