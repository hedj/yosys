#!/usr/bin/env python3
"""Trace every synth_* flow on the given designs and run flowcheck on each trace."""
import argparse, os, subprocess, sys
from concurrent.futures import ThreadPoolExecutor
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import flowcheck

FLOWS = ["synth", "synth_achronix", "synth_analogdevices", "synth_anlogic", "synth_easic", "synth_ecp5",
         "synth_efinix", "synth_fabulous", "synth_gatemate", "synth_gowin", "synth_ice40", "synth_intel",
         "synth_intel_alm", "synth_lattice", "synth_microchip", "synth_nanoxplore", "synth_nexus",
         "synth_quicklogic", "synth_sf2", "synth_xilinx"]

def run(yosys, outdir, flow, design):
    src, top, pre = design
    tag = f"{flow}.{top}"
    trace = os.path.join(outdir, tag + ".jsonl")
    if os.path.exists(trace):
        os.unlink(trace)
    r = subprocess.run([yosys, "-q", "-l", os.path.join(outdir, tag + ".log"), "-p",
                        f"read_verilog {src}; {pre} {flow} -top {top}"],
                       env={**os.environ, "YOSYS_PASS_TRACE": trace}, capture_output=True, text=True)
    if r.returncode:
        return flow, top, None, (r.stderr or r.stdout).strip().splitlines()[-1:]
    return flow, top, flowcheck.analyse(flowcheck.load(trace)), None

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--yosys", default=os.path.join(os.path.dirname(__file__), "../../build/yosys"))
    ap.add_argument("--out", default="flowcheck_out")
    ap.add_argument("--flows", nargs="*", default=FLOWS)
    ap.add_argument("design", nargs="+", help="file.v:top[:chparam args]")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    designs = []
    for d in args.design:
        src, top, *chp = d.split(":")
        pre = f"hierarchy -top {top} {chp[0]};" if chp else ""
        designs.append((os.path.abspath(src), top, pre))
    jobs = [(f, d) for f in args.flows for d in designs]
    with ThreadPoolExecutor(os.cpu_count()) as ex:
        results = list(ex.map(lambda j: run(args.yosys, args.out, *j), jobs))
    by_flow = {}
    for flow, top, findings, err in results:
        entry = by_flow.setdefault(flow, {"errors": [], "findings": []})
        if err:
            entry["errors"].append(f"{top}: {err}")
        else:
            entry["findings"] += [f"{f['severity']}: {f['msg']}" for f in findings]
    for flow in args.flows:
        e = by_flow[flow]
        msgs = sorted(set(e["findings"]))
        print(f"== {flow}: {len(msgs)} finding(s)" + (f", {len(e['errors'])} error(s)" if e["errors"] else ""))
        for m in msgs:
            print("   " + m)
        for m in e["errors"]:
            print("   ERROR " + m)

if __name__ == "__main__":
    main()
