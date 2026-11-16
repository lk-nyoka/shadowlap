#!/bin/sh
# Run scenarios several times and keep every run, so timing can be reported as
# a spread (min / median / max) instead of one lucky or unlucky lap.
#
#   sh repeat.sh N "shadow flood attack" OUTDIR      (works on QNX: plain sh, no python)
#
# Then copy OUTDIR back and summarise it:  python3 tools/timing_table.py OUTDIR
# All of run_demo.sh's settings (V2_USER, PROD_CPU, LAP, PLANT_ARGS, ...) apply.
N=${1:-10}
SCEN=${2:-"baseline shadow flood attack"}
OUT=${3:-out/repeat}
HERE=$(dirname "$0")
i=1
while [ "$i" -le "$N" ]; do
    for s in $SCEN; do
        echo "### run $i of $N: $s"
        sh "$HERE/run_demo.sh" "$s" "$OUT/run$i" 2>&1 | grep -E "plant\]|BUDGET|containment"
    done
    i=$((i + 1))
done
echo "done: $OUT (copy it back and run: python3 tools/timing_table.py $OUT)"
