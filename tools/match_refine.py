"""Second pass for the addresses tools/match_functions.py could not carry from EFLC to GTA IV.
Reads its tables and fills in what it left as "?":
  functions  best body among the unmatched IV functions between the nearest matched neighbours
  inner      same instruction index through the alignment, or the nearest aligned one before it
  strings    the same NUL-terminated text, if unique in IV's .rdata
  tables     vtables and function-pointer tables: pointers mapped through the function matches
  globals    the offset of the nearest voted globals, when both sides agree
Hand translations in analysis/gta4/eflc_to_iv_manual.tsv (EFLC, IV, comment) win over everything.

usage: py -3 -I tools/match_refine.py <addresses.tsv> <functions.tsv> <data_votes.tsv>
       (rewrites addresses.tsv; run from the project root)
"""
import bisect, difflib, glob, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script directory out
import match_functions as mf
from pe_layout import BASE, sections

addresses_path, functions_path, votes_path = sys.argv[1:4]
manual_path = os.path.join(os.path.dirname(addresses_path), "eflc_to_iv_manual.tsv")

MIN_SIMILARITY, MIN_MARGIN = 0.45, 0.05

rows = [line.rstrip("\n").split("\t") for line in open(addresses_path, encoding="utf-8")]
matches = {}
for line in open(functions_path, encoding="utf-8"):
    parts = line.split("\t")
    matches[int(parts[0], 16)] = int(parts[1], 16)
votes = sorted((int(a, 16), int(b, 16)) for a, b, *_ in (l.split("\t") for l in open(votes_path, encoding="utf-8")))

eflc_pe, iv_pe = open("build/default.pe", "rb").read(), open("build/gta4.pe", "rb").read()
eflc_layout, iv_layout = sections(eflc_pe), sections(iv_pe)


def address_of(name):
    return int(name[4:], 16) if name.startswith("sub_") else None


eflc, _ = mf.parse(sorted(glob.glob("ppc/ppc_recomp.*.cpp")), mf.OURS, address_of)
iv, _ = mf.parse(sorted(glob.glob("ppc_iv/ppc_recomp.*.cpp")), mf.OURS, address_of)
order_eflc, order_iv = sorted(eflc), sorted(iv)
taken = set(matches.values())
matched_eflc = sorted(matches)


def containing(functions, order, address):
    k = bisect.bisect_right(order, address) - 1
    if k < 0:
        return None
    f = functions[order[k]]
    return f if address < f.start + 4 * len(f.raw) else None


def match_function(start):
    """Best unmatched IV body between the matched neighbours of an EFLC function."""
    k = bisect.bisect_left(matched_eflc, start)
    if k == 0 or k == len(matched_eflc):
        return None, 0.0
    lo, hi = matches[matched_eflc[k - 1]], matches[matched_eflc[k]]
    if lo >= hi:
        return None, 0.0
    target = eflc[start]
    scored = sorted(((difflib.SequenceMatcher(None, target.norm, iv[c].norm, autojunk=False).ratio(), c)
                     for c in order_iv[bisect.bisect_right(order_iv, lo):bisect.bisect_left(order_iv, hi)]
                     if c not in taken), reverse=True)
    if not scored:
        return None, 0.0
    best, candidate = scored[0]
    if best < MIN_SIMILARITY or (len(scored) > 1 and best - scored[1][0] < MIN_MARGIN):
        return None, best
    return candidate, best


def translate_inner(address):
    f = containing(eflc, order_eflc, address)
    if not f or f.start not in matches:
        return None, None
    target = iv[matches[f.start]]
    index = (address - f.start) // 4
    before = [(i, j) for i, j in mf.align(f, target) if i <= index]
    if not before:
        return None, None
    i, j = before[-1]
    method = "inner aligned" if i == index else f"inner near +{index - i} instructions"
    return matches[f.start] + 4 * (j + index - i), method


def section_of(layout, address):
    for name, (base, size) in layout.items():
        if base <= address < base + size:
            return name
    return None


def translate_string(address):
    text = eflc_pe[address - BASE:address - BASE + 256].split(b"\0", 1)[0]
    if len(text) < 4 or not all(32 <= c < 127 for c in text):
        return None
    base, size = iv_layout[".rdata"]
    region = iv_pe[base - BASE:base - BASE + size]
    needle = b"\0" + text + b"\0"
    first = region.find(needle)
    if first < 0 or region.find(needle, first + 1) >= 0:
        return None
    return base + first + 1


iv_rdata_words = None


def translate_table(address):
    """A table of code pointers in .rdata: the IV position where most mapped pointers line up."""
    global iv_rdata_words
    words = struct.unpack_from(">8I", eflc_pe, address - BASE)
    code_base, code_size = eflc_layout[".text"]
    mapped = [(k, matches[w]) for k, w in enumerate(words) if code_base <= w < code_base + code_size and w in matches]
    if len(mapped) < 3:
        return None
    base, size = iv_layout[".rdata"]
    if iv_rdata_words is None:
        iv_rdata_words = struct.unpack_from(">%dI" % (size // 4), iv_pe, base - BASE)
    first_k, first_value = mapped[0]
    found = []
    for position, value in enumerate(iv_rdata_words):
        if value == first_value and position >= first_k:
            hits = sum(iv_rdata_words[position - first_k + k] == w for k, w in mapped
                       if position - first_k + k < len(iv_rdata_words))
            found.append((hits, position - first_k))
    found.sort(reverse=True)
    if not found or found[0][0] * 4 < len(mapped) * 3 or (len(found) > 1 and found[1][0] == found[0][0]):
        return None
    return base + 4 * found[0][1]


vote_keys = [a for a, _ in votes]


def translate_global(address):
    k = bisect.bisect_right(vote_keys, address)
    if k == 0 or k == len(votes):
        return None
    below, above = votes[k - 1], votes[k]
    delta = below[1] - below[0]
    if above[1] - above[0] == delta or address - below[0] <= 0x40:
        return address + delta
    return None


manual = {}
if os.path.exists(manual_path):
    for line in open(manual_path, encoding="utf-8"):
        if line.strip() and not line.startswith("#"):
            a, b, *comment = line.rstrip("\n").split("\t")
            manual[int(a, 16)] = (b, comment[0] if comment else "manual")

# Functions first: inner addresses of newly matched functions depend on them.
for row in rows:
    a = int(row[0], 16)
    if row[1] == "?" and a in eflc and a not in manual:
        candidate, score = match_function(a)
        if candidate is not None:
            matches[a] = candidate
            taken.add(candidate)
            bisect.insort(matched_eflc, a)
            row[1], row[2] = f"0x{candidate:08X}", f"refined body {score:.2f}"
        else:
            row[2] = f"function not matched (best {score:.2f})"

for row in rows:
    a = int(row[0], 16)
    if a in manual:
        row[1], row[2] = manual[a]
        continue
    if row[1] != "?" or a in eflc:
        continue
    value, method = translate_inner(a)
    if value is None:
        section = section_of(eflc_layout, a)
        if section == ".rdata":
            value, method = translate_string(a), "string"
            if value is None:
                value, method = translate_table(a), "pointer table"
        if value is None and section in (".rdata", ".data", None):
            value, method = translate_global(a), "global offset"
    if value is not None:
        row[1], row[2] = f"0x{value:08X}", method

with open(addresses_path, "w", encoding="utf-8", newline="\n") as f:
    for row in rows:
        f.write("\t".join(row) + "\n")
left = [r for r in rows if r[1] == "?"]
print(f"{len(rows)} addresses, {len(left)} still untranslated")
for r in left:
    print("  ", r[0], r[2])
