"""Rewrites GTA IV TU8 guest addresses in LibertyRecomp source files to their EFLC equivalents.

Uses the address table written by tools/match_functions.py -a (TU8, EFLC, method, TU8 name):
every sub_XXXXXXXX / __imp__sub_XXXXXXXX and every 0x8XXXXXXX literal found in the table is
replaced; TU8 symbolic function names (D3DResource_Release...) become sub_<EFLC>. Literals not in
the table (constants, flags) are left alone and listed if they look like code/data addresses, so
they can be checked by hand.

Hand-found translations in analysis/tu8_to_eflc_manual.tsv (same columns, # comments) are read
after the table and win over it.

usage: py -3 -I tools/translate_addresses.py <addresses.tsv> <input> <output> [more input output]
"""
import os, re, sys

table, names = {}, {}
manual = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), "tu8_to_eflc_manual.tsv")
for path in (sys.argv[1], manual):
    if not os.path.exists(path):
        continue
    for line in open(path, encoding="utf-8"):
        if line.startswith("#") or not line.strip():
            continue
        tu8, eflc, method, name = (line.rstrip("\n").split("\t") + [""])[:4]
        if eflc == "?":
            continue
        table[int(tu8, 16)] = int(eflc, 16)
        if name:
            names[name] = int(eflc, 16)

SUB = re.compile(r"\b(__imp__)?sub_([0-9A-F]{8})\b")
HEX = re.compile(r"\b0x(8[0-9A-Fa-f]{7})\b")
NAME = re.compile(r"\b(__imp__)?(" + "|".join(map(re.escape, sorted(names, key=len, reverse=True))) + r")\b") if names else None

pairs = sys.argv[2:]
for k in range(0, len(pairs), 2):
    source, target = pairs[k], pairs[k + 1]
    text = open(source, encoding="utf-8").read()
    missing = set()

    def sub(m):
        a = int(m.group(2), 16)
        if a in table:
            return f"{m.group(1) or ''}sub_{table[a]:08X}"
        missing.add(f"sub_{m.group(2)}")
        return m.group(0)

    def hexa(m):
        a = int(m.group(1), 16)
        if a in table:
            digits = m.group(1)
            value = f"{table[a]:08X}" if digits.isupper() or digits.isdigit() else f"{table[a]:08x}"
            return "0x" + value
        if 0x82000000 <= a < 0x84000000:
            missing.add(m.group(0))
        return m.group(0)

    text = SUB.sub(sub, text)
    text = HEX.sub(hexa, text)
    if NAME:
        text = NAME.sub(lambda m: f"{m.group(1) or ''}sub_{names[m.group(2)]:08X}", text)
    with open(target, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(f"{source} -> {target}: {len(missing)} addresses not translated")
    for item in sorted(missing):
        print("   ", item)
