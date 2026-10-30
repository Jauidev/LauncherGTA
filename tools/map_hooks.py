"""Lists the guest functions a TU8 source file overrides (extern "C" void sub_XXXXXXXX / named) and
their EFLC addresses from tools/match_functions.py's output.

usage: py -3 -I tools/map_hooks.py <matches.tsv> <tu8 config.toml> <source.cpp> [filter regex]
"""
import re, sys

matches_path, config_path, source_path = sys.argv[1:4]
pattern = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None

matches = {}
for line in open(matches_path, encoding="utf-8"):
    tu8, eflc, name, size = line.rstrip("\n").split("\t")
    matches[name] = (tu8, eflc, int(size))
named = {}
for line in open(config_path, encoding="utf-8"):
    m = re.match(r'^"0x([0-9A-Fa-f]{8})"\s*=\s*\{\s*name\s*=\s*"([^"]+)"', line)
    if m:
        named[m.group(2)] = "0x" + m.group(1).upper()

hooked = sorted(set(re.findall(r'extern "C" void ([A-Za-z0-9_]+)\(', open(source_path, encoding="utf-8").read())))
found = 0
for name in hooked:
    tu8 = "0x" + name[4:] if name.startswith("sub_") else named.get(name, "?")
    if pattern and not pattern.search(name):
        continue
    if name in matches:
        found += 1
        print(f"{name:40} TU8 {tu8} -> EFLC {matches[name][1]} ({matches[name][2]} instr)")
    else:
        print(f"{name:40} TU8 {tu8} -> ?")
print(f"{len(hooked)} overrides, {found} shown with an EFLC match", file=sys.stderr)
