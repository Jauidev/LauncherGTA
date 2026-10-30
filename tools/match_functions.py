"""Matches functions between two XenonRecomp-style outputs by their PowerPC listings (the "// asm"
comments), to carry addresses from GTA IV TU8 (LibertyRecomp's generated code) to this EFLC build.

Pass 1 (seeds): each function becomes a fingerprint, its instruction sequence with build-specific
numbers masked (branch/call targets, lis immediates and large immediates, which are address halves).
A pair is accepted when the fingerprint is unique on both sides.

Pass 2 (call graph): matched pairs are aligned instruction by instruction (difflib on the masked
listings); aligned calls, tail calls and function-address loads vote for callee pairs. A vote is
accepted when it is unanimous for both sides and the two bodies are similar enough. Repeats until
nothing new matches.

Pass 3 (addresses): the same alignments translate data addresses (lis + displacement) and
addresses inside matched functions (return addresses, hook points).

usage: py -3 -I tools/match_functions.py <eflc ppc dir> <tu8 generated dir> <tu8 config.toml>
       [addresses.txt] [-o matches.tsv] [-a addresses.tsv]

The generated dir may also be a XenonRecomp output (ppc_recomp.*.cpp), which is how EFLC addresses
are carried to GTA IV: py -3 -I tools/match_functions.py ppc_iv ppc config/gta4eflc.toml ...
"""
import collections, difflib, glob, hashlib, os, re, sys

ASM = re.compile(r"^\s*// (.+)$")
OURS = re.compile(r"^PPC_FUNC_IMPL\(__imp__(sub_[0-9A-F]{8})\)")
THEIRS = re.compile(r"^DEFINE_REX_FUNC\(([A-Za-z0-9_]+)\)")
LABEL = re.compile(r"^loc_([0-9A-F]{8}):")
HEX_ADDR = re.compile(r"0x8[0-9a-fA-F]{7}")
NUMBER = re.compile(r"(?<![\w.])-?\d+")
MEMORY = re.compile(r"^(-?\d+)\(r(\d+)\)$")
LOW, HIGH = 0x82000000, 0x84000000


def normalize(line):
    line = HEX_ADDR.sub("A", line)
    op = line.split(None, 1)[0]
    if op in ("lis", "addis"):
        return op + " " + NUMBER.sub("I", line.split(None, 1)[1])
    def small(match):
        value = int(match.group(0))
        return match.group(0) if -4096 < value < 4096 else "I"
    return NUMBER.sub(small, line)


class Function:
    __slots__ = ("start", "raw", "norm", "refs", "print")

    def __init__(self, start):
        self.start, self.raw, self.norm = start, [], []

    def finish(self):
        # Data words (".long") take an address but are not part of the listing.
        self.norm = [normalize(r) if r[0] != "." else None for r in self.raw]
        body = [n for n in self.norm if n is not None]
        self.print = hashlib.sha1("\n".join(body).encode()).hexdigest() if len(body) >= 4 else None
        self.refs = references(self.raw)


def references(raw):
    """Absolute addresses per instruction index: branch targets and lis-based effective addresses."""
    refs = {}
    regs = {}
    for i, line in enumerate(raw):
        if line[0] == ".":
            continue
        parts = line.split(None, 1)
        op = parts[0]
        args = parts[1].split(",") if len(parts) > 1 else []
        target = HEX_ADDR.search(line)
        if target and op.startswith("b"):
            refs[i] = ("call" if op in ("bl", "b") else "branch", int(target.group(0), 16))
            if op == "bl":
                # r14-r31 survive a call: a base loaded before it (lis r24 ...) still counts after.
                regs = {r: v for r, v in regs.items() if int(r[1:]) >= 14}
            continue
        if op in ("lis", "addis") and len(args) >= 2:
            rd = args[0].strip()
            try:
                imm = int(args[-1])
            except ValueError:
                regs.pop(rd, None)
                continue
            if op == "addis":
                regs.pop(rd, None)
                continue
            regs[rd] = (imm << 16) & 0xFFFFFFFF
            continue
        if op in ("addi", "ori") and len(args) == 3:
            rd, ra, imm = (a.strip() for a in args)
            if ra in regs:
                try:
                    value = int(imm)
                except ValueError:
                    regs.pop(rd, None)
                    continue
                addr = (regs[ra] + value) & 0xFFFFFFFF if op == "addi" else regs[ra] | (value & 0xFFFF)
                if LOW <= addr < HIGH:
                    refs[i] = ("data", addr)
                regs[rd] = addr
                continue
        if len(args) == 2:
            m = MEMORY.match(args[1].strip())
            if m and "r" + m.group(2) in regs:
                addr = (regs["r" + m.group(2)] + int(m.group(1))) & 0xFFFFFFFF
                if LOW <= addr < HIGH:
                    refs[i] = ("data", addr)
        if args and not op.startswith(("st", "cmp", "tw", "dcb")):
            regs.pop(args[0].strip(), None)
    return refs


def parse(paths, header, address_of):
    functions = {}
    current = None
    bad_labels = 0
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = header.match(line)
                if m:
                    if current:
                        current.finish()
                    start = address_of(m.group(1))
                    current = Function(start) if start is not None else None
                    if current:
                        functions[start] = current
                    continue
                if not current:
                    continue
                a = ASM.match(line)
                if a:
                    current.raw.append(a.group(1).strip())
                    continue
                # Jump tables sit between instructions without a listing line (ReXGlue): pad to
                # the label so instruction indices stay addresses.
                lab = LABEL.match(line)
                if lab:
                    gap = int(lab.group(1), 16) - (current.start + 4 * len(current.raw))
                    if gap > 0 and gap % 4 == 0 and gap < 0x10000:
                        current.raw.extend([".table"] * (gap // 4))
                    elif gap:
                        bad_labels += 1
        if current:
            current.finish()
            current = None
    return functions, bad_labels


def align(fa, fb):
    """Index pairs (i in fa, j in fb) whose masked instructions are equal."""
    if fa.print is not None and fa.print == fb.print and len(fa.norm) == len(fb.norm):
        return [(i, i) for i in range(len(fa.norm))]
    sm = difflib.SequenceMatcher(None, fa.norm, fb.norm, autojunk=False)
    pairs = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        if tag == "equal":
            pairs.extend(zip(range(i1, i2), range(j1, j2)))
    return pairs


def similarity(fa, fb):
    if fa.print is not None and fa.print == fb.print:
        return 1.0
    la, lb = len(fa.norm), len(fb.norm)
    if min(la, lb) / max(la, lb, 1) < 0.5:
        return 0.0
    return difflib.SequenceMatcher(None, fa.norm, fb.norm, autojunk=False).ratio()


def main():
    argv = sys.argv[1:]
    options = {}
    for flag in ("-o", "-a", "-d"):
        if flag in argv:
            k = argv.index(flag)
            options[flag] = argv[k + 1]
            del argv[k:k + 2]
    ours_dir, theirs_dir, config = argv[:3]
    interest_file = argv[3] if len(argv) > 3 else None

    named = {}
    for line in open(config, encoding="utf-8"):
        m = re.match(r'^"0x([0-9A-Fa-f]{8})"\s*=\s*\{\s*name\s*=\s*"([^"]+)"', line)
        if m:
            named[m.group(2)] = int(m.group(1), 16)
    names_of = {v: k for k, v in named.items()}

    def address(name):
        return int(name[4:], 16) if name.startswith("sub_") else named.get(name)

    ours, bad_ours = parse(sorted(glob.glob(os.path.join(ours_dir, "ppc_recomp.*.cpp"))), OURS, address)
    theirs_files = sorted(glob.glob(os.path.join(theirs_dir, "gta4_recomp.*.cpp")))
    if theirs_files:
        theirs, bad_theirs = parse(theirs_files, THEIRS, address)
    else:
        theirs, bad_theirs = parse(sorted(glob.glob(os.path.join(theirs_dir, "ppc_recomp.*.cpp"))), OURS, address)
    if bad_ours or bad_theirs:
        print(f"warning: labels off the instruction count: EFLC {bad_ours}, TU8 {bad_theirs}")

    # Pass 1: unique fingerprints.
    by_ours, by_theirs = collections.defaultdict(list), collections.defaultdict(list)
    for a, f in ours.items():
        if f.print:
            by_ours[f.print].append(a)
    for a, f in theirs.items():
        if f.print:
            by_theirs[f.print].append(a)
    matches, how = {}, {}
    for p, starts in by_theirs.items():
        if len(starts) == 1 and len(by_ours.get(p, [])) == 1:
            matches[starts[0]] = by_ours[p][0]
            how[starts[0]] = "print"
    seeds = len(matches)
    taken = set(matches.values())

    # Pass 2: call-graph propagation.
    alignments = {}

    def aligned(t):
        if t not in alignments:
            alignments[t] = align(theirs[t], ours[matches[t]])
        return alignments[t]

    # Votes come only from pairs matched in the previous round, so both tallies keep running.
    forward = collections.defaultdict(collections.Counter)
    backward = collections.defaultdict(collections.Counter)
    def dominant(counter):
        """The candidate with at least 3/4 of the votes, or None."""
        (best, n), = counter.most_common(1)
        return best if n * 4 >= sum(counter.values()) * 3 else None

    def accept(ct, co, method):
        matches[ct] = co
        how[ct] = method
        taken.add(co)

    def propagate(pending):
        rounds = 0
        while pending:
            rounds += 1
            touched = set()
            for t in pending:
                ft, fo = theirs[t], ours[matches[t]]
                for i, j in aligned(t):
                    rt, ro = ft.refs.get(i), fo.refs.get(j)
                    if not rt or not ro or rt[0] != ro[0] or rt[0] == "branch":
                        continue
                    ct, co = rt[1], ro[1]
                    if ct in theirs and co in ours:
                        forward[ct][co] += 1
                        backward[co][ct] += 1
                        touched.add(ct)
            new = []
            for ct in touched:
                if ct in matches:
                    continue
                co = dominant(forward[ct])
                if co is None or co in taken or dominant(backward[co]) != ct:
                    continue
                if similarity(theirs[ct], ours[co]) < 0.6:
                    continue
                accept(ct, co, "graph")
                new.append(ct)
            pending = new
        return rounds

    order_theirs, order_ours = sorted(theirs), sorted(ours)
    index_ours = {a: k for k, a in enumerate(order_ours)}

    def fill_gaps():
        """Unmatched runs between two matched neighbours, paired in address order when the run
        lengths agree (the linker keeps object file order) and the bodies are similar."""
        new = []
        anchor = None
        run = []
        for t in order_theirs:
            if t not in matches:
                run.append(t)
                continue
            if anchor is not None and run:
                lo, hi = index_ours[matches[anchor]], index_ours[matches[t]]
                candidates = [o for o in order_ours[lo + 1:hi] if o not in taken]
                if 0 < hi - lo - 1 == len(run) == len(candidates):
                    for ct, co in zip(run, candidates):
                        if similarity(theirs[ct], ours[co]) >= 0.5:
                            accept(ct, co, "order")
                            new.append(ct)
                elif candidates and len(run) <= 64 and len(candidates) <= 64:
                    # Different counts (functions added or removed in EFLC): best body in the gap,
                    # if clearly better than the runner-up. Catches virtual methods, which have no
                    # direct callers for the call graph to follow.
                    for ct in run:
                        scored = sorted(((similarity(theirs[ct], ours[co]), co) for co in candidates
                                         if co not in taken), reverse=True)
                        if not scored:
                            break
                        best, co = scored[0]
                        second = scored[1][0] if len(scored) > 1 else 0.0
                        if best >= 0.7 and best - second >= 0.1:
                            accept(ct, co, "gap")
                            new.append(ct)
            anchor, run = t, []
        return new

    rounds = propagate(list(matches))
    by_graph = len(matches) - seeds
    while True:
        new = fill_gaps()
        if not new:
            break
        rounds += propagate(new)

    print(f"EFLC functions {len(ours)}, TU8 functions {len(theirs)}")
    print(f"matches: {len(matches)} ({seeds} by fingerprint, {by_graph} by call graph, "
          f"{len(matches) - seeds - by_graph} by address order + graph; {rounds} graph rounds)")

    # Pass 3: data and inner addresses.
    data_votes = collections.defaultdict(collections.Counter)
    for t in matches:
        ft, fo = theirs[t], ours[matches[t]]
        # Same length: position by position. The listing diff drops lines whose displacements differ
        # in size (27052(r24) normalizes apart from -2676(r24)), and those are the votes that matter.
        same_length = len(ft.raw) == len(fo.raw)
        for i, j in (zip(range(len(ft.raw)), range(len(fo.raw))) if same_length else aligned(t)):
            rt, ro = ft.refs.get(i), fo.refs.get(j)
            if rt and ro and rt[0] == "data" == ro[0]:
                data_votes[rt[1]][ro[1]] += 1
    data = {}
    for at, counter in data_votes.items():
        (ao, n), = counter.most_common(1)
        if n * 2 > sum(counter.values()):
            data[at] = (ao, n, sum(counter.values()))

    if "-d" in options:
        with open(options["-d"], "w", encoding="utf-8") as f:
            for at in sorted(data):
                f.write(f"0x{at:08X}\t0x{data[at][0]:08X}\t{data[at][1]}\t{data[at][2]}\n")

    starts_theirs = sorted(theirs)

    def containing(addr):
        import bisect
        k = bisect.bisect_right(starts_theirs, addr) - 1
        if k < 0:
            return None
        f = theirs[starts_theirs[k]]
        return f if addr < f.start + 4 * len(f.raw) else None

    def translate(addr):
        """(eflc address, method) or (None, reason)."""
        if addr in matches:
            return matches[addr], how[addr]
        if addr in theirs:
            return None, "function not matched"
        f = containing(addr)
        if f:
            if f.start not in matches:
                return None, f"inside unmatched sub_{f.start:08X}"
            index = (addr - f.start) // 4
            for i, j in aligned(f.start):
                if i == index:
                    return matches[f.start] + 4 * j, f"inner sub_{f.start:08X}+0x{addr - f.start:X}"
            return None, f"inner sub_{f.start:08X}+0x{addr - f.start:X} not aligned"
        if addr in data:
            ao, n, total = data[addr]
            return ao, f"data {n}/{total} votes"
        # Fields of a referenced global: nearest voted base below within 256 bytes, same offset.
        best = None
        for base_t in range(addr - 4, addr - 260, -4):
            if base_t in data:
                best = base_t
                break
        if best is not None:
            ao, n, total = data[best]
            return ao + (addr - best), f"data near 0x{best:08X}+0x{addr - best:X}"
        return None, "data not referenced by matched code"

    if interest_file:
        text = open(interest_file, encoding="utf-8").read()
        wanted = {int(x, 16) for x in re.findall(r"0x8[0-9A-Fa-f]{7}|(?<=sub_)[0-9A-F]{8}", text)}
        wanted |= {named[w] for w in re.findall(r"[A-Za-z_][A-Za-z0-9_]+", text) if w in named}
        wanted = sorted(wanted)
        rows = [(a, *translate(a)) for a in wanted]
        kinds = collections.Counter()
        for a, eflc, method in rows:
            kind = "function" if a in theirs else ("inner" if containing(a) else "data")
            kinds[kind, eflc is not None] += 1
        for kind in ("function", "inner", "data"):
            ok, total = kinds[kind, True], kinds[kind, True] + kinds[kind, False]
            print(f"of interest, {kind}: {ok}/{total} translated")
        if "-a" in options:
            with open(options["-a"], "w", encoding="utf-8") as f:
                for a, eflc, method in rows:
                    name = names_of.get(a, "")
                    f.write(f"0x{a:08X}\t{'0x%08X' % eflc if eflc is not None else '?'}\t{method}\t{name}\n")

    if "-o" in options:
        with open(options["-o"], "w", encoding="utf-8") as f:
            for a in sorted(matches):
                name = names_of.get(a, f"sub_{a:08X}")
                f.write(f"0x{a:08X}\t0x{matches[a]:08X}\t{name}\t{len(theirs[a].raw)}\t{how[a]}\n")


if __name__ == "__main__":
    main()
