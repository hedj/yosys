#!/usr/bin/env python3
"""Check yosys synthesis flows for pass-ordering mistakes.

Run yosys with YOSYS_PASS_TRACE=<file> to record, for every pass invocation,
the cell type histogram before and after the pass. This script reads such a
trace and reports passes that ran after the cells they operate on had already
been lowered away by an earlier pass (e.g. `share` after `alumacc` has turned
every $mul into $macc_v2).

Each cell type is assigned an abstraction level; each pass that matters for
ordering declares the cell types it consumes and the levels it works at.
"""

import argparse
import json
import sys

# ---------------------------------------------------------------- levels

WORD, ARITH, GATE, LUT, TECH = range(5)
LEVEL_NAMES = ["word", "arith", "gate", "lut", "tech"]

ARITH_TYPES = {"$alu", "$macc", "$macc_v2", "$lcu", "$fa"}
LUT_TYPES = {"$lut", "$sop"}
# Cells with no level: they survive every lowering unchanged, so they say
# nothing about how far a design has been lowered.
LEVELLESS = {"$scopeinfo", "$print", "$check", "$assert", "$assume", "$cover",
             "$live", "$fair", "$initstate", "$anyconst", "$anyseq", "$anyinit",
             "$allconst", "$allseq", "$equiv", "$specify2", "$specify3", "$specrule",
             "$get_tag", "$set_tag", "$overwrite_tag", "$original_tag", "$future_ff",
             "$barrier", "$connect", "$input_port", "$output_port", "$buf"}


def level_of(cell_type):
    if cell_type in LEVELLESS:
        return None
    if not cell_type.startswith("$"):
        return TECH  # library cells and user modules
    if cell_type in ARITH_TYPES:
        return ARITH
    if cell_type in LUT_TYPES:
        return LUT
    if cell_type.startswith("$_"):
        return GATE  # simcells: $_AND_, $_DFF_P_, $_MUX_, ...
    if cell_type.startswith("$__"):
        return TECH  # techmap-internal helper cells
    return WORD


def levels_present(hist):
    return {level_of(t) for t, n in hist.items() if n and level_of(t) is not None}

# ---------------------------------------------------------------- contracts

CMP = {"$lt", "$le", "$ge", "$gt", "$eq", "$ne", "$eqx", "$nex"}
SHIFT = {"$shl", "$shr", "$sshl", "$sshr", "$shift", "$shiftx"}
DIVMOD = {"$div", "$mod", "$divfloor", "$modfloor"}
BITWISE = {"$and", "$or", "$xor", "$xnor", "$not"}
REDUCE = {"$reduce_and", "$reduce_or", "$reduce_xor", "$reduce_xnor", "$reduce_bool"}
LOGIC = {"$logic_and", "$logic_or", "$logic_not"}
WORD_ARITH = {"$add", "$sub", "$mul", "$neg", "$pos", "$pow"} | DIVMOD
MEMRD = {"$memrd", "$memrd_v2"}


class Contract:
    def __init__(self, consumes, levels, note=""):
        self.consumes = set(consumes)
        self.levels = set(levels)
        self.note = note


# Only passes whose position in a flow matters are listed. Generic cleanups
# (opt_clean, opt_merge, ...) work at every level and are not checked.
CONTRACTS = {
    "share": Contract({"$mul"} | DIVMOD | {"$shl", "$shr", "$sshl", "$sshr"} | MEMRD, [WORD],
                      "default mode only shares these types"),
    "opt_share": Contract(WORD_ARITH | CMP | SHIFT | BITWISE | LOGIC | {"$alu", "$concat"}, [WORD, ARITH]),
    "alumacc": Contract({"$add", "$sub", "$mul", "$neg", "$pos", "$macc"} | CMP, [WORD]),
    "booth": Contract({"$mul", "$macc", "$macc_v2"}, [WORD, ARITH]),
    "wreduce": Contract(WORD_ARITH | CMP | SHIFT | BITWISE | REDUCE | LOGIC | {"$mux", "$pmux"}, [WORD]),
    "peepopt": Contract({"$mul", "$shiftx", "$shr", "$shl", "$sshr", "$add", "$sub"} | DIVMOD, [WORD]),
    "muxpack": Contract({"$mux", "$pmux"}, [WORD]),
    "pmux2shiftx": Contract({"$pmux"}, [WORD]),
    "opt_muxtree": Contract({"$mux", "$pmux"}, [WORD]),
    "opt_balance_tree": Contract({"$and", "$or", "$xor", "$add", "$mul"}, [WORD]),
    "opt_demorgan": Contract(REDUCE, [WORD]),
    "opt_lut": Contract({"$lut"}, [LUT]),
    "fsm": Contract({"$dff", "$adff", "$sdff", "$dffe", "$adffe", "$sdffe", "$sdffce"}, [WORD]),
    "memory_share": Contract({"$mem_v2", "$mem"}, [WORD]),
    "abc": Contract({"$_AND_", "$_OR_", "$_XOR_", "$_NOT_", "$_MUX_", "$_NAND_", "$_NOR_",
                     "$_XNOR_", "$_ANDNOT_", "$_ORNOT_"}, [GATE]),
}

# ---------------------------------------------------------------- analysis


# What a lowering pass turns each type into, where that is known. Used to
# tell a real loss ($mul -> $macc_v2, which share does not handle) from a
# harmless one ($lt -> $alu, which opt_share does handle).
LOWERS = {
    "alumacc": {t: {"$alu", "$macc_v2"} for t in {"$add", "$sub", "$neg", "$pos"}}
               | {t: {"$alu"} for t in CMP}
               | {"$mul": {"$macc_v2"}, "$macc": {"$macc_v2"}},
}

# Lowering passes: finding their inputs already lowered (by themselves on an
# earlier run, or by a deliberate target-specific mapping) is not a problem.
LOWERING_PASSES = {"alumacc", "booth"}


def is_target_mapping(pass_name, became):
    """Did this lowering map to the target (techmap to LUTs/DSPs/vendor cells)?"""
    if pass_name == "techmap" or pass_name.endswith("_dsp") or pass_name.startswith(("lattice_", "xilinx_", "ice40_")):
        return True
    return any(level_of(t) in (LUT, TECH) for t in became)


# Composite passes that run their children at whatever level the design is
# at. A child finding nothing to do there is expected, not an ordering issue.
LEVEL_AGNOSTIC_PARENTS = {"opt"}


def parents(events):
    """Map event index -> list of ancestor pass names (nearest first)."""
    # Events are in post-order: a parent is the next event at depth - 1.
    nearest = {}
    parent_of = [None] * len(events)
    for i in range(len(events) - 1, -1, -1):
        d = events[i]["depth"]
        parent_of[i] = nearest.get(d - 1)
        nearest[d] = i
    out = []
    for i in range(len(events)):
        chain, j = [], parent_of[i]
        while j is not None:
            chain.append(events[j]["pass"])
            j = parent_of[j]
        out.append(chain)
    return out


def analyse(events):
    """Return findings for one trace (a list of pass events in execution order)."""
    lowered_by = {}  # type -> (pass name, cmd, set of types it became)
    had_chance = {}  # pass name -> types that were live when it ran
    ancestors = parents(events)
    findings = []
    for i, ev in enumerate(events):
        # Events are logged when a pass finishes, so a composite pass (synth,
        # opt, memory, ...) follows its children. Only leaf passes count.
        if i > 0 and events[i - 1]["depth"] > ev["depth"]:
            continue
        before, after = ev["before"], ev["after"]
        name = ev["pass"]
        contract = CONTRACTS.get(name)
        if contract:
            lost = {}
            for t in contract.consumes:
                if t in lowered_by and not before.get(t):
                    by, cmd, became = lowered_by[t]
                    if became & contract.consumes:
                        continue
                    if name in LOWERING_PASSES and (by == name or is_target_mapping(by, became)):
                        continue
                    if t in had_chance.get(name, ()):
                        continue  # an earlier run of this pass already saw these cells
                    lost.setdefault((by, cmd), set()).add(t)
            for (by, cmd), types in lost.items():
                became = set().union(*(lowered_by[t][2] for t in types))
                findings.append({
                    "kind": "stranded", "severity": "info" if is_target_mapping(by, became) else "warning",
                    "pass": ev["cmd"], "index": i,
                    "types": sorted(types), "lowered_by": cmd,
                    "msg": f"`{ev['cmd']}` handles {', '.join(sorted(types))}, but `{cmd}` already "
                           f"lowered them to {', '.join(sorted(became))}, which it does not handle",
                })
            had_chance.setdefault(name, set()).update(t for t in contract.consumes if before.get(t))
            lv_before = levels_present(before)
            if (lv_before and min(lv_before) > max(contract.levels)
                    and not LEVEL_AGNOSTIC_PARENTS & set(ancestors[i])):
                findings.append({
                    "kind": "level", "severity": "warning", "pass": ev["cmd"], "index": i,
                    "msg": f"`{ev['cmd']}` works at {'/'.join(LEVEL_NAMES[l] for l in sorted(contract.levels))} "
                           f"level but the design is already at {'/'.join(LEVEL_NAMES[l] for l in sorted(lv_before))}",
                })
        removed = {t for t, n in before.items() if n and not after.get(t)}
        if removed:
            new = {t for t, n in after.items() if n and not before.get(t)}
            new_levels = {level_of(t) for t in new if level_of(t) is not None}
            for t in removed:
                lt = level_of(t)
                if lt is None:
                    continue
                known = LOWERS.get(name, {}).get(t)
                if known is not None:
                    lowered_by[t] = (name, ev["cmd"], known)
                elif any(l > lt for l in new_levels):
                    lowered_by[t] = (name, ev["cmd"], {u for u in new if level_of(u) is not None and level_of(u) > lt})
        for t, n in after.items():
            if n and t in lowered_by:
                del lowered_by[t]  # reintroduced by a later pass: not lowered any more
    return findings


def load(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def dedupe(findings):
    out, keys = [], set()
    for f in findings:
        key = (f["kind"], f["msg"])
        if key not in keys:
            keys.add(key)
            out.append(f)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("trace", nargs="+", help="YOSYS_PASS_TRACE output file(s)")
    ap.add_argument("--json", action="store_true", help="print findings as JSON")
    args = ap.parse_args()
    findings = []
    for path in args.trace:
        for f in analyse(load(path)):
            f["trace"] = path
            findings.append(f)
    findings = dedupe(findings)
    if args.json:
        json.dump(findings, sys.stdout, indent=1)
    else:
        for f in findings:
            print(f"{f['severity']}: [{f['kind']}] {f['msg']}")
    return 1 if any(f["severity"] == "warning" for f in findings) else 0


if __name__ == "__main__":
    sys.exit(main())
