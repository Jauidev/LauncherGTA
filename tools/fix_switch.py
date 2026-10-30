"""Extends function boundaries so every jump-table label lands inside its owning function.
Usage: fix_switch.py <default.pe> <switch_tables.toml> <xenonrecomp.log> <functions.toml> <ppc_func_mapping.cpp> > merged
Reads the current explicit `functions` entries, adds/extends the owners of the tables that
XenonRecomp reported ("Switch case at X is trying to jump outside function"), prints the
merged `functions = [...]` block.
"""
import re, struct, sys, bisect

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script directory out
from pe_layout import sections

BASE = 0x82000000
pe = open(sys.argv[1], "rb").read()
LAYOUT = sections(pe)
TB, TS = LAYOUT[".text"]
w = struct.unpack_from(">%dI" % (TS // 4), pe, TB - BASE)
word = lambda a: w[(a - TB) // 4]

tables = {}
for m in re.finditer(r"base = 0x([0-9A-F]+)\s*\nr = \d+\s*\ndefault = 0x[0-9A-F]+\s*\nlabels = \[(.*?)\]", open(sys.argv[2]).read(), re.S):
    tables[int(m.group(1), 16)] = [int(x, 16) for x in re.findall(r"0x([0-9A-F]+)", m.group(2))]

bad = {int(x, 16) for x in re.findall(r"Switch case at ([0-9A-F]+) is trying", open(sys.argv[3]).read())}

explicit = {int(a, 16): int(s, 16) for a, s in re.findall(r"address = 0x([0-9A-F]+), size = 0x([0-9A-F]+)", open(sys.argv[4]).read())}

pb, ps = LAYOUT[".pdata"]
pdata = {}
for i in range(ps // 8):
    begin, packed = struct.unpack_from(">II", pe, pb - BASE + 8 * i)
    if TB <= begin < TB + TS:
        pdata[begin] = ((packed >> 8) & 0x3FFFFF) * 4
pstarts = sorted(pdata)


def block_end(a):
    """End of the straight-line block starting at a (after its unconditional exit)."""
    while a < TB + TS:
        y = word(a)
        a += 4
        if ((y >> 26) == 18 and not (y & 1)) or y in (0x4E800020, 0x4E800420) or y == 0:
            return a
    return a


def owner_of(addr):
    # explicit entries first, then .pdata; a leaf function without .pdata falls back to the
    # closest preceding start, which is what XenonAnalyse would have picked.
    cands = [a for a, s in explicit.items() if a <= addr < a + s]
    if cands:
        return max(cands), explicit[max(cands)]
    k = bisect.bisect_right(pstarts, addr) - 1
    if k >= 0 and addr < pstarts[k] + pdata[pstarts[k]]:
        return pstarts[k], pdata[pstarts[k]]
    # leaf function without .pdata: take the start XenonRecomp chose (ppc_func_mapping.cpp)
    k = bisect.bisect_right(mstarts, addr) - 1
    if k >= 0:
        return mstarts[k], addr + 4 - mstarts[k]
    return None, None


mstarts = sorted(int(a, 16) for a in re.findall(r"\{ 0x([0-9A-F]+), sub_", open(sys.argv[5]).read()))


tbases = sorted(tables)
unresolved = []
for site in sorted(bad):
    # the log reports the bctr address, a few instructions after the table's base
    k = bisect.bisect_right(tbases, site) - 1
    tb = tbases[k] if k >= 0 and site - tbases[k] <= 0x40 else site
    labels = tables.get(tb)
    if not labels:
        unresolved.append(tb)
        continue
    start, size = owner_of(tb)
    if start is None:
        unresolved.append(tb)
        continue
    end = max([start + size] + [block_end(l) for l in labels if l >= start])
    # never swallow the next .pdata function
    k = bisect.bisect_right(pstarts, start)
    if k < len(pstarts):
        end = min(end, pstarts[k])
    explicit[start] = max(explicit.get(start, 0), end - start)

print("functions = [")
for a in sorted(explicit):
    print("    { address = 0x%08X, size = 0x%X }," % (a, explicit[a]))
print("]")
print("tables fixed: %d, unresolved (owner without .pdata): %d %s" % (len(bad) - len(unresolved), len(unresolved),
      " ".join("%X" % u for u in unresolved[:20])), file=sys.stderr)
