# flowcheck

Finds pass-ordering mistakes in yosys synthesis flows.

1. Run yosys with `YOSYS_PASS_TRACE=trace.jsonl`. Every `Pass::call` appends
   one JSON line: the pass, its nesting depth, and the cell type histogram of
   the design before and after it ran.
2. `flowcheck.py trace.jsonl` assigns every cell type an abstraction level
   (word, arith, gate, lut, tech) and checks the passes listed in `CONTRACTS`:
   - **stranded**: the pass handles a cell type that an earlier pass lowered
     into something it does not handle (e.g. `share` after `alumacc` has
     turned every `$mul` into `$macc_v2`). A warning when a generic lowering
     did it, info when a target-specific mapping did.
   - **level**: the pass runs after the design has been lowered below every
     level it works at.

`run_flows.py mix.v:mix ...` traces every `synth_*` flow on the given designs
and prints the findings per flow.
