"""Per-component, flow-sensitive backward precision analysis on spirv-dis listings (prototype of the C++ one).

Like precise_comp.py, but a load only depends on the stores that reach it (reaching definitions over the CFG, per
variable element and component), since the Xenos compiler reuses registers for unrelated values.
Usage: py -3 -I precise_flow.py listing.txt...
"""
import collections
import re
import sys

COMPONENTWISE = {"OpFNegate", "OpFAdd", "OpFSub", "OpFMul", "OpFDiv", "OpFRem", "OpFMod", "OpSelect", "OpPhi",
                 "OpCopyObject", "OpConvertSToF", "OpConvertUToF", "OpFConvert", "OpIAdd", "OpISub", "OpIMul",
                 "OpFOrdEqual", "OpFOrdNotEqual", "OpFOrdLessThan", "OpFOrdGreaterThan", "OpFOrdLessThanEqual",
                 "OpFOrdGreaterThanEqual", "OpFUnordEqual", "OpFUnordNotEqual", "OpFUnordLessThan",
                 "OpFUnordGreaterThan", "OpFUnordLessThanEqual", "OpFUnordGreaterThanEqual", "OpLogicalAnd",
                 "OpLogicalOr", "OpLogicalNot", "OpIsNan", "OpIsInf"}
GLSL_COMPONENTWISE = {"Round", "RoundEven", "Trunc", "FAbs", "SAbs", "FSign", "SSign", "Floor", "Ceil", "Fract",
                      "Radians", "Degrees", "Sin", "Cos", "Tan", "Asin", "Acos", "Atan", "Sinh", "Cosh", "Tanh",
                      "Atan2", "Pow", "Exp", "Log", "Exp2", "Log2", "Sqrt", "InverseSqrt", "FMin", "UMin", "SMin",
                      "FMax", "UMax", "SMax", "FClamp", "UClamp", "SClamp", "FMix", "Step", "SmoothStep", "Fma",
                      "NMin", "NMax", "NClamp", "Ldexp"}
ARITH = {"OpFNegate", "OpFAdd", "OpFSub", "OpFMul", "OpFDiv", "OpFRem", "OpFMod", "OpVectorTimesScalar", "OpDot",
         "OpExtInst", "OpSelect", "OpPhi"}
TERMINATORS = {"OpBranch", "OpBranchConditional", "OpSwitch", "OpReturn", "OpReturnValue", "OpKill", "OpUnreachable",
               "OpTerminateInvocation"}


def analyze(path):
    defs, width, float_ids, consts, locations = {}, {}, set(), {}, {}
    type_width, float_types = {}, set()
    var_width = {}
    stores = []      # (block, keys, object, object_component or None)
    load_info = {}   # load id -> (block, position, keys list per result component)
    seeds = []
    blocks = collections.OrderedDict()  # label -> list of ("store", index) / ("load", id)
    succ = collections.defaultdict(list)
    current = None
    dynamic_vars = set()
    input_vars = set()
    for line in open(path, encoding="utf-8"):
        m = re.match(r"\s*(%\S+) = (Op\w+)(.*)", line)
        s = re.match(r"\s*(Op\w+)(.*)", line)
        if m:
            rid, op, rest = m.groups()
            args = rest.split()
            if op == "OpTypeFloat":
                type_width[rid] = 1
                if args[0] == "32":
                    float_types.add(rid)
                continue
            if op in ("OpTypeInt", "OpTypeBool"):
                type_width[rid] = 1
                continue
            if op == "OpTypeVector":
                type_width[rid] = int(args[1])
                if args[0] in float_types:
                    float_types.add(rid)
                continue
            if op == "OpTypePointer":
                type_width[rid] = ("ptr", args[1])
                continue
            if op == "OpTypeArray":
                type_width[rid] = ("arr", args[0])
                continue
            if op.startswith("OpType"):
                continue
            if op == "OpLabel":
                current = rid
                blocks[rid] = []
                continue
            defs[rid] = (op, args)
            if args:
                w = type_width.get(args[0], 4)
                width[rid] = w if isinstance(w, int) else 4
                if args[0] in float_types:
                    float_ids.add(rid)
            if op == "OpConstant":
                consts[rid] = args[1]
            elif op == "OpVariable":
                if len(args) > 1 and args[1] == "Input" and rid == "%gl_FragCoord":
                    input_vars.add(rid)
                # Element width of the pointee: vectors (or arrays of vectors).
                pointee = type_width.get(args[0])
                w = 4
                if isinstance(pointee, tuple):
                    t = type_width.get(pointee[1])
                    if isinstance(t, tuple) and t[0] == "arr":
                        t = type_width.get(t[1])
                    if isinstance(t, int):
                        w = t
                var_width[rid] = w
                locations[rid] = (rid, None, None)
            elif op in ("OpAccessChain", "OpInBoundsAccessChain"):
                var, el, comp = locations.get(args[1], (args[1], "any", None))
                for a in args[2:]:
                    value = consts.get(a, "any")
                    if value == "any":
                        dynamic_vars.add(var)
                    if el is None:
                        el = value
                    elif comp is None:
                        comp = value
                locations[rid] = (var, el, comp)
            elif op == "OpLoad" and current is not None:
                blocks[current].append(("load", rid))
            image = op.startswith("OpImage") and op != "OpImage"
            if image:
                for a in args[2:]:
                    if a.startswith("%"):
                        seeds.append((a, 15))
            elif op in ("OpBitcast", "OpConvertFToU", "OpConvertFToS") or op.startswith("OpDPd") or op.startswith("OpFwidth"):
                seeds.append((args[1], 15))
            continue
        if s:
            op, rest = s.groups()
            args = rest.split()
            if op == "OpStore" and current is not None:
                loc = locations.get(args[0])
                if loc:
                    stores.append((loc, args[1]))
                    blocks[current].append(("store", len(stores) - 1))
            elif op == "OpBranch":
                succ[current].append(args[0])
            elif op == "OpBranchConditional":
                succ[current] += args[1:3]
            elif op == "OpSwitch":
                succ[current] += [a for a in args[1:] if a.startswith("%")]

    def keys_of(loc):
        var, el, comp = loc
        w = var_width.get(var, 4)
        if comp not in (None, "any"):
            return [(var, el, int(comp))]
        return [(var, el, c) for c in range(w)]

    # Reaching definitions: state = dict key -> bitset of store indices.
    def transfer(state, items, on_load=None):
        state = dict(state)
        for kind, x in items:
            if kind == "store":
                loc, _ = stores[x]
                if loc[0] in dynamic_vars or loc[1] == "any":
                    for k in keys_of(loc):
                        state[k] = state.get(k, 0) | (1 << x)
                else:
                    for k in keys_of(loc):
                        state[k] = 1 << x
            elif on_load:
                on_load(x, state)
        return state

    preds = collections.defaultdict(list)
    for b, ss in succ.items():
        for t in ss:
            preds[t].append(b)
    order = list(blocks)
    out = {b: {} for b in order}
    changed = True
    while changed:
        changed = False
        for b in order:
            state = {}
            for p in preds[b]:
                for k, v in out[p].items():
                    state[k] = state.get(k, 0) | v
            new = transfer(state, blocks[b])
            if new != out[b]:
                out[b] = new
                changed = True
    reaching = {}  # load id -> {component: bitset}

    def record(load_id, state):
        op, args = defs[load_id]
        loc = locations.get(args[1])
        if not loc:
            return
        var, el, comp = loc
        if var in dynamic_vars or el == "any":
            reaching[load_id] = None  # flow-insensitive fallback
            return
        reaching[load_id] = {i: state.get(k, 0) for i, k in enumerate(keys_of(loc))}

    for b in order:
        state = {}
        for p in preds[b]:
            for k, v in out[p].items():
                state[k] = state.get(k, 0) | v
        transfer(state, blocks[b], record)

    # Forward taint from interpolators (positions, rays, texture coordinates): everything computed from them, and
    # everything combined with them (backward walk below), keeps full precision.
    users = collections.defaultdict(list)
    for rid, (op, args) in defs.items():
        for a in args[1:]:
            users[a].append(rid)
    tainted = set()
    stack = [rid for rid, (op, args) in defs.items() if op in ("OpLoad", "OpAccessChain") and len(args) > 1 and locations.get(args[1], (None,))[0] in input_vars]
    # Loads reached by each store (through Xenia's register array).
    store_loads = collections.defaultdict(list)
    for load_id, r in reaching.items():
        if r is None:
            continue
        for bits in r.values():
            x = 0
            while bits:
                if bits & 1:
                    store_loads[x].append(load_id)
                bits >>= 1
                x += 1
    object_stores = collections.defaultdict(list)
    for index, (sloc, obj) in enumerate(stores):
        object_stores[obj].append(index)
    while stack:
        x = stack.pop()
        if x in tainted:
            continue
        tainted.add(x)
        stack += users[x]
        for index in object_stores.get(x, ()):
            stack += store_loads[index]
    seeds += [(x, 15) for x in tainted if x in float_ids]
    needed = collections.defaultdict(int)
    work = list(seeds)

    def need(i, mask):
        if i in defs and mask & ~needed[i]:
            work.append((i, mask))

    while work:
        i, mask = work.pop()
        if i not in defs:
            continue
        mask &= (1 << width.get(i, 4)) - 1 or 1
        new = mask & ~needed[i]
        if not new:
            continue
        needed[i] |= new
        op, args = defs[i]
        ops = args[1:]
        if op == "OpLoad":
            loc = locations.get(ops[0])
            r = reaching.get(i, "none")
            if not loc:
                continue
            if r is None or r == "none":
                for (sloc, obj) in stores:
                    if sloc[0] == loc[0]:
                        need(obj, 15)
                continue
            for comp_index, bits in r.items():
                if not new >> comp_index & 1:
                    continue
                x = 0
                while bits:
                    if bits & 1:
                        sloc, obj = stores[x]
                        # Component of the stored object that defines this component.
                        if sloc[2] in (None,) and loc[2] in (None,):
                            need(obj, 1 << comp_index)
                        elif sloc[2] not in (None, "any") and loc[2] in (None,):
                            need(obj, 1)
                        elif loc[2] not in (None, "any") and sloc[2] in (None,):
                            need(obj, 1 << int(loc[2]))
                        else:
                            need(obj, 15)
                    bits >>= 1
                    x += 1
            continue
        if op in ("OpVariable", "OpConstant", "OpConstantComposite", "OpUndef"):
            continue
        if op == "OpCompositeExtract":
            need(ops[0], 1 << int(ops[1]) if len(ops) == 2 else 15)
        elif op == "OpCompositeConstruct":
            pos = 0
            for a in ops:
                w = width.get(a, 1)
                sub = (new >> pos) & ((1 << w) - 1)
                if sub:
                    need(a, sub)
                pos += w
        elif op == "OpVectorShuffle":
            v1, v2, comps = ops[0], ops[1], [int(c) for c in ops[2:]]
            w1 = width.get(v1, 4)
            for j, c in enumerate(comps):
                if new >> j & 1 and c != 0xFFFFFFFF:
                    need(v1, 1 << c) if c < w1 else need(v2, 1 << (c - w1))
        elif op == "OpCompositeInsert":
            obj, comp_, k = ops[0], ops[1], int(ops[2])
            if new >> k & 1:
                need(obj, 1)
            if new & ~(1 << k):
                need(comp_, new & ~(1 << k))
        elif op == "OpVectorTimesScalar":
            need(ops[0], new)
            need(ops[1], 1)
        elif op in COMPONENTWISE:
            for a in (ops[0::2] if op == "OpPhi" else ops):
                need(a, new if width.get(a, 1) > 1 else 1)
        elif op == "OpExtInst" and len(ops) >= 2 and ops[1] in GLSL_COMPONENTWISE:
            for a in ops[2:]:
                need(a, new if width.get(a, 1) > 1 else 1)
        else:
            for a in (ops[2:] if op == "OpExtInst" else ops):
                need(a, 15)

    cands = [i for i, (op, args) in defs.items() if op in ARITH and i in float_ids]
    relaxed = [i for i in cands if not needed[i]]
    return len(relaxed), len(cands)


tr = tc = 0
for p in sys.argv[1:]:
    r, c = analyze(p)
    tr += r
    tc += c
    if len(sys.argv) <= 6:
        print(f"{p[-16:]}: {r} of {c} relaxed")
print(f"total: {tr} of {tc} ({100 * tr / max(tc, 1):.0f}%)")
