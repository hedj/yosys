#!/usr/bin/env bash
# Synthesis results must not depend on names interned by earlier, unrelated
# work in the same process. techmap used to order cells by IdString index
# (the position in the global name table), so mapping a discarded design that
# happened to create the same names changed the result.
set -e

count() {
	${YOSYS} -q -p "$1 read_verilog ../functional/picorv32.v; hierarchy -top picorv32; script +/synth_levels.ys; tee -q -o name_history.stat stat" > /dev/null
	grep -E '^ +[0-9]+ +cells$' name_history.stat | tr -dc 0-9
}

plain=$(count "")
after=$(count "read_verilog name_history_throwaway.v; proc; memory -nomap; memory_map; design -reset;")
rm -f name_history.stat
echo "cells: $plain without, $after after unrelated throwaway work"
[ "$plain" = "$after" ]
