"""Counts the instructions of spirv-dis listings whose result only depends on constants and uniform buffers.

Those are the same for every pixel of a draw (fetch constant decoding, system constants), so they could be computed
once on the CPU. Loads from Function variables count as uniform only if every store that may reach them is (here
approximated flow-insensitively per variable element).
Usage: py -3 -I uniform_only.py listing.txt...
"""
import collections
import re
import sys

total = uniform_total = 0
per_op = collections.Counter()
for path in sys.argv[1:]:
    defs, order = {}, []
    var_class, locations, consts = {}, {}, {}
    stores = collections.defaultdict(list)
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*(%\S+) = (Op\w+)(.*)", line)
        if m:
            rid, op, rest = m.groups()
            args = rest.split()
            if op.startswith("OpType") or op == "OpLabel":
                continue
            defs[rid] = (op, args)
            order.append(rid)
            if op == "OpVariable":
                var_class[rid] = args[1]
                locations[rid] = (rid, None)
            elif op == "OpConstant":
                consts[rid] = args[1]
            elif op in ("OpAccessChain", "OpInBoundsAccessChain"):
                var, el = locations.get(args[1], (args[1], "any"))
                if el is None and len(args) > 2:
                    el = consts.get(args[2], "any")
                locations[rid] = (var, el)
            continue
        m = re.match(r"\s*OpStore (%\S+) (%\S+)", line)
        if m and m.group(1) in locations:
            stores[locations[m.group(1)]].append(m.group(2))
    uniform = {}
    CONSTANT_OPS = ("OpConstant", "OpConstantComposite", "OpConstantTrue", "OpConstantFalse", "OpConstantNull",
                    "OpUndef", "OpSpecConstant", "OpExtInstImport", "OpVariable", "OpFunction")
    changed = True
    while changed:
        changed = False
        for rid in order:
            if rid in uniform:
                continue
            op, args = defs[rid]
            if op in CONSTANT_OPS:
                value = True
            elif op in ("OpAccessChain", "OpInBoundsAccessChain"):
                value = all(uniform.get(a, a not in defs) for a in args[2:])
            elif op == "OpLoad":
                var, el = locations.get(args[1], (None, None))
                cls = var_class.get(var)
                if cls in ("Uniform", "UniformConstant", "PushConstant", "StorageBuffer"):
                    value = cls != "StorageBuffer"
                elif cls == "Function":
                    objs = [o for (v, e), os in stores.items() if v == var and (e == el or "any" in (e, el))
                            for o in os]
                    if not all(o in uniform for o in objs):
                        continue  # undecided yet
                    value = all(uniform[o] for o in objs)
                else:
                    value = False
            elif op in ("OpImageSampleImplicitLod", "OpImageSampleExplicitLod", "OpImageFetch", "OpPhi",
                        "OpFunctionParameter", "OpDPdx", "OpDPdy", "OpDPdxCoarse", "OpDPdyCoarse", "OpFwidth"):
                value = False
            else:
                ids = [a for a in args[1:] if a in defs]
                if not all(a in uniform for a in ids):
                    continue
                value = all(uniform[a] for a in ids)
            uniform[rid] = value
            changed = True
    # Undecided values (cycles through stores) are not uniform.
    for rid in order:
        op = defs[rid][0]
        if op in ("OpConstant", "OpConstantComposite", "OpVariable", "OpAccessChain", "OpInBoundsAccessChain",
                  "OpExtInstImport", "OpFunction", "OpUndef", "OpConstantTrue", "OpConstantFalse", "OpConstantNull"):
            continue
        total += 1
        if uniform.get(rid):
            uniform_total += 1
            per_op[op] += 1
print(f"instructions with results: {total}, uniform-only: {uniform_total} ({100 * uniform_total / max(total, 1):.0f}%)")
print("most common uniform-only:", ", ".join(f"{op} {n}" for op, n in per_op.most_common(12)))
