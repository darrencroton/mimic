#!/bin/bash
# Serialized sampled-profile collection for mimic.
#
#   sample_runs.sh <run.yaml> <repeats> <outdir>
#
# For each repeat: start /usr/bin/sample in -wait mode (so it attaches at process
# creation, capturing startup), launch ./mimic in the background, wait for mimic,
# then wait for sample to flush.  Exactly one mimic process runs at any time.
# Sampling interval 1 ms, duration cap 10 s (sample exits when the target dies).
set -u
REPO="${MIMIC_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
CFG="$1"; N="$2"; OUT="$3"
mkdir -p "$OUT"
cd "$REPO" || exit 1
for i in $(seq 1 "$N"); do
  f="$OUT/sample_$(printf %02d "$i").txt"
  /usr/bin/sample -wait mimic 10 1 -mayDie -fullPaths -f "$f" >/dev/null 2>&1 &
  spid=$!
  sleep 0.35            # let sample enter its wait loop
  t0=$(python3 -c 'import time;print(time.perf_counter())')
  ./mimic "$CFG" -q > "$OUT/run_$(printf %02d "$i").log" 2>&1
  rc=$?
  t1=$(python3 -c 'import time;print(time.perf_counter())')
  wait $spid
  echo "repeat $i rc=$rc wall=$(python3 -c "print(f'{$t1-$t0:.4f}')") lines=$(wc -l < "$f")"
  [ $rc -ne 0 ] && { echo "ABORT: mimic rc=$rc"; exit 1; }
done
exit 0
