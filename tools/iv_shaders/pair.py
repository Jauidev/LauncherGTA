#!/usr/bin/env python3
"""Pairs the containers a game registers but its SPIR-V cache lacks with the cache's unused entries.

The cache (LibertyRecompLib/shader/shader_cache.cpp) holds TU8's shaders by the hash of the whole
container, so one built from another source tree hashes differently although the program is the
same. compare.py showed that recompiling here does not reproduce the cache (it was made with a
newer XenosRecomp), so this compares what a compiler cannot change, from the container and from the
cached SPIR-V: the sampler registers (the cache's usedTextureMask), the constant registers read
(offset / 16 in RawBufferLoad) and the literal floats. Every registered container that is in the
cache is the control: the share found as the best match says how far the method can be trusted.

    py -3 -I tools/iv_shaders/pair.py <dump>/gta4_containers <work dir of compare.py>
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CACHE = os.path.join(ROOT, "third_party", "rexglue", "LibertyRecompLib", "shader", "shader_cache.cpp")
ENTRY = re.compile(r'\{ 0x([0-9A-F]+), \d+, \d+, (\d+), (\d+), (\d+), (\d+), \d+, \d+, (\d+), "([^"]+)", nullptr, (\d+) \}')
OP_TYPE_INT, OP_TYPE_FLOAT, OP_CONSTANT = 21, 22, 43
SET_FLOAT4, SET_SAMPLER = 2, 3


def read_entries():
    entries = {}
    with open(CACHE, encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f):
            if i > 1400:
                break
            m = ENTRY.search(line[:600])
            if m:
                entries[f"{int(m[1], 16):016X}"] = dict(mask=int(m[8]), name=m[7], spec=int(m[6]))
    return entries


def spirv_features(path):
    data = open(path, "rb").read()
    words = struct.unpack(f"<{len(data) // 4}I", data[:len(data) // 4 * 4])
    int64, float32 = set(), set()
    offsets, floats = set(), set()
    i = 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 0xFFFF
        if not count:
            break
        if op == OP_TYPE_INT and words[i + 2] == 64:
            int64.add(words[i + 1])
        elif op == OP_TYPE_FLOAT and words[i + 2] == 32:
            float32.add(words[i + 1])
        elif op == OP_CONSTANT:
            if words[i + 1] in int64:
                offsets.add(words[i + 3] | (words[i + 4] << 32))
            elif words[i + 1] in float32:
                floats.add(words[i + 3])
        i += count
    return dict(offsets=offsets, floats=floats, words=len(words))


def container_features(path):
    data = open(path, "rb").read()
    u32 = lambda at: struct.unpack_from(">I", data, at)[0]
    u16 = lambda at: struct.unpack_from(">H", data, at)[0]
    virtual_size = u32(4)
    table = u32(0x10) + 4  # ConstantTableContainer: size field, then the D3DX table.
    regs, samplers = set(), set()
    # D3DXSHADER_CONSTANTTABLE: size, creator, version, constants, constantInfo, flags, target.
    count, info = u32(table + 12), u32(table + 16)
    for n in range(count):
        at = table + info + n * 20
        reg_set, reg, reg_count = u16(at + 4), u16(at + 6), u16(at + 8)
        if reg_set == SET_FLOAT4:
            regs.update(range(reg, reg + reg_count))
        elif reg_set == SET_SAMPLER:
            samplers.update(range(reg, reg + reg_count))
    # The definition table: float4 runs up to a null terminator, values in the physical part.
    floats = set()
    definitions = u32(0x14)
    at = definitions + 20
    while definitions and at + 8 <= len(data):
        head = u32(at)
        if head == 0:
            break
        components = (head & 0xFFFF) + 3 & ~3
        physical = u32(at + 4)
        for k in range(components):
            where = virtual_size + physical + k * 4
            if where + 4 <= len(data):
                floats.add(u32(where))
        at += 8
    shader = u32(0x18)
    return dict(regs=regs, samplers=samplers, floats=floats, code=u32(shader + 4), mask=sum(1 << s for s in samplers))


def score(c, s, mask):
    """Higher is better; a sampler mask that differs is a mismatch by itself."""
    if c["mask"] != mask:
        return -1.0
    wanted = {r * 16 for r in c["regs"]}
    regs = len(wanted & s["offsets"]) / (len(wanted) or 1)
    observed = {o for o in s["offsets"] if o % 16 == 0 and o < 0x1000}
    extra = len(observed - wanted) / (len(observed) or 1)
    literal = c["floats"] - {0}
    floats = len(literal & s["floats"]) / (len(literal) or 1)
    size = min(c["code"] * 40, s["words"] * 4) / max(c["code"] * 40, s["words"] * 4, 1)
    return regs + floats - 0.5 * extra + 0.25 * size


def main():
    dump, work = sys.argv[1], sys.argv[2]
    entries = read_entries()
    registered = {}
    for line in open(os.path.join(dump, "order.txt")):
        h, st = line.split()
        registered.setdefault(h, st)
    is_vertex = lambda e: "_vs" in e["name"] or e["name"].endswith(".vert")
    spirv = {h: spirv_features(os.path.join(work, "cached", h + ".spv")) for h in entries}
    containers = {h: container_features(os.path.join(dump, f"gta4_{h}.container.{st}")) for h, st in registered.items()}

    known = [h for h in registered if h in entries]
    right = mask_right = 0
    wrong = []
    for h in known:
        c = containers[h]
        mask_right += c["mask"] == entries[h]["mask"]
        vertex = registered[h] == "vert"
        ranked = sorted(((score(c, spirv[u], entries[u]["mask"]), u) for u in entries if is_vertex(entries[u]) == vertex), reverse=True)
        true_score = score(c, spirv[h], entries[h]["mask"])
        if ranked[0][1] == h or ranked[0][0] == true_score:
            right += 1
        else:
            wrong.append((h, true_score, ranked[0]))
    print(f"control: {len(known)} known pairs, best match right {right} ({100 * right / len(known):.1f}%), sampler mask equal {mask_right}")
    for h, t, best in wrong[:6]:
        print(f"  miss {h} {entries[h]['name'][-44:]} true {t:.2f} best {best[0]:.2f} {entries[best[1]]['name'][-40:]}")

    missing = [h for h in registered if h not in entries]
    unused = [h for h in entries if h not in registered and "runtime_captured" in entries[h]["name"]]
    print(f"\nmissing {len(missing)}, unused captured {len(unused)}")
    chosen = {}
    for h in missing:
        c = containers[h]
        vertex = registered[h] == "vert"
        ranked = sorted(((score(c, spirv[u], entries[u]["mask"]), u) for u in unused if is_vertex(entries[u]) == vertex), reverse=True)
        chosen[h] = ranked[0][1]
        data = open(os.path.join(dump, f"gta4_{h}.container.{registered[h]}"), "rb").read()
        label = re.findall(rb"[A-Za-z_][A-Za-z0-9_]{2,}", data)
        label = " ".join(s.decode() for s in label[label.index(b"src") + 1:label.index(b"src") + 4])
        print(f"{h} {registered[h]} mask={c['mask']:04X} regs={len(c['regs']):2d} lits={len(c['floats']):2d} -> "
              f"{ranked[0][1]} {ranked[0][0]:.2f} | {ranked[1][1]} {ranked[1][0]:.2f} | {label}")
    if len(set(chosen.values())) != len(chosen):
        print("WARNING: two containers took the same entry")


main()
