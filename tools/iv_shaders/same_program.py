#!/usr/bin/env python3
"""Compares, position by position, the shaders two titles register after the cache's last hit.

GTA IV and EFLC load the same missing .fxc files in the same order. For each pair: the physical part
(literal floats and microcode) must be byte-identical but for its last 8 bytes, a build signature that
differs in all of them, and the constant table equal; then the second title can take the first one's
alias. Prints the alias lines for rex/graphics/gta4_native/shader_aliases.h.

    py -3 -I tools/iv_shaders/same_program.py <IV gta4_containers> <EFLC gta4_containers>
"""
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
HEADER = os.path.join(ROOT, "third_party", "rexglue", "include", "rex", "graphics", "gta4_native", "shader_aliases.h")
CACHE = os.path.join(ROOT, "third_party", "rexglue", "LibertyRecompLib", "shader", "shader_cache.cpp")
ENTRY = re.compile(r"\{ 0x([0-9A-F]+), \d+, \d+, \d+, \d+")


def cache_hashes():
    hashes = set()
    with open(CACHE, encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f):
            if i > 1400:
                break
            m = ENTRY.search(line[:200])
            if m:
                hashes.add(f"{int(m[1], 16):016X}")
    return hashes


def missing(dump, cached):
    seen, out = set(), []
    for line in open(os.path.join(dump, "order.txt")):
        h, stage = line.split()
        if h not in seen:
            seen.add(h)
            if h not in cached:
                out.append((h, stage))
    return out


def parts(dump, h, stage):
    data = open(os.path.join(dump, f"gta4_{h}.container.{stage}"), "rb").read()
    u32 = lambda at: struct.unpack_from(">I", data, at)[0]
    u16 = lambda at: struct.unpack_from(">H", data, at)[0]
    virtual_size = u32(4)
    table = u32(0x10) + 4
    constants = []
    for n in range(u32(table + 12)):
        at = table + u32(table + 16) + n * 20
        name = data[table + u32(at):].split(b"\0", 1)[0]
        constants.append((name, u16(at + 4), u16(at + 6), u16(at + 8)))
    # The physical part without its trailing signature: literal floats and microcode, no build path.
    return data[virtual_size:-8], sorted(constants)


def main():
    iv_dump, eflc_dump = sys.argv[1], sys.argv[2]
    cached = cache_hashes()
    aliases = dict((f.upper(), t.upper()) for f, t in re.findall(r"\{0x([0-9A-Fa-f]{16})ull, 0x([0-9A-Fa-f]{16})ull\}", open(HEADER).read()))
    comments = dict((m[0].upper(), m[1]) for m in re.findall(r"\{0x([0-9A-Fa-f]{16})ull, 0x[0-9A-Fa-f]{16}ull\},\s*// (.+)", open(HEADER).read()))
    iv, eflc = missing(iv_dump, cached), missing(eflc_dump, cached)
    print(f"missing IV {len(iv)} EFLC {len(eflc)}")
    identical = 0
    lines = []
    for (a, sa), (b, sb) in zip(iv, eflc):
        code_a, const_a = parts(iv_dump, a, sa)
        code_b, const_b = parts(eflc_dump, b, sb)
        same = sa == sb and code_a == code_b and const_a == const_b
        identical += same
        print(f"{a} {b} {sa} code {len(code_a)}/{len(code_b)} {'SAME' if same else 'DIFF code=%s const=%s' % (code_a == code_b, const_a == const_b)}  {comments.get(a, '?')}")
        if same and b not in aliases:
            lines.append(f"    {{0x{b}ull, 0x{aliases[a]}ull}},  // {comments[a]}")
    print(f"identical {identical}/{len(iv)}\n")
    print("\n".join(lines))


main()
