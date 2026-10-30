"""Finds jump tables XenonAnalyse missed. The compiler's dispatch sequence is
    lis r12,hi ; addi r12,r12,lo ; rlwinm r0,rN,2,0,29 ; lwzx r0,r12,r0 ; mtctr r0 ; bctr
and tables reached without a bounds check (e.g. an index that is always 0-3) are not recognised:
the bctr becomes an indirect call to a case label that is no function (null host pointer at
run time). The table usually sits right after the bctr; its length runs to the first label.
Usage: find_switches.py <default.pe> <switch_tables.toml>  -> [[switch]] entries on stdout
"""
import re, struct, sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script directory out
from pe_layout import sections

BASE = 0x82000000

pe = open(sys.argv[1], "rb").read()
LAYOUT = sections(pe)
CODE = [(LAYOUT[n][0], LAYOUT[n][0] + LAYOUT[n][1]) for n in (".text", "BINK")]
known = {int(b, 16) for b in re.findall(r"base = 0x([0-9A-Fa-f]+)", open(sys.argv[2]).read())}


def word(a):
    return struct.unpack_from(">I", pe, a - BASE)[0]


def in_code(a):
    return any(lo <= a < hi for lo, hi in CODE)


def simm(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


found = 0
for lo, hi in CODE:
    for a in range(lo, hi - 24, 4):
        w0 = word(a)
        # lis r12,hi
        if w0 >> 16 != 0x3D80:
            continue
        w1 = word(a + 4)
        # addi r12,r12,lo
        if w1 >> 16 != 0x398C:
            continue
        w2, w3, w4, w5 = word(a + 8), word(a + 12), word(a + 16), word(a + 20)
        # rlwinm r0,rN,2,0,29 (54 0x 10 3A with rS in bits 21-25)
        if (w2 & 0xFC1FFFFF) != 0x5400103A or w3 != 0x7C0C002E or w4 != 0x7C0903A6 or w5 != 0x4E800420:
            continue
        if a in known:
            continue

        index_reg = (w2 >> 21) & 31
        table = ((w0 & 0xFFFF) << 16) + simm(w1)
        labels = []
        t = table
        first = None
        while in_code(word(t)) and word(t) & 3 == 0:
            label = word(t)
            if first is None or label < first:
                first = label
            labels.append(label)
            t += 4
            # The table ends where the code it points to begins.
            if first is not None and t >= first:
                break
        if not labels:
            continue

        # Bounds check, if any: cmplwi crX,rN,imm followed by bgt -> default.
        default = labels[0]
        for b in range(a - 4, a - 64, -4):
            w = word(b)
            if (w >> 26) == 10 and ((w >> 16) & 31) == index_reg:  # cmpli
                nxt = word(b + 4)
                if (nxt >> 26) == 16:  # bc
                    default = (b + 4) + simm(nxt & 0xFFFC)
                break

        found += 1
        print("\n# gta4port: missed by XenonAnalyse (tools/find_switches.py)")
        print("[[switch]]")
        print("base = 0x%08X" % a)
        print("r = %d" % index_reg)
        print("default = 0x%08X" % default)
        print("labels = [")
        for label in labels:
            print("    0x%08X," % label)
        print("]")

print("missed jump tables: %d" % found, file=sys.stderr)
