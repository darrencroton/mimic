#!/bin/bash
# Serialized sampled-profile collection for mimic.
#
#   sample_runs.sh <run.yaml> <repeats> <outdir>
#
# For each repeat: start /usr/bin/sample in -wait mode so it attaches at process creation
# and captures startup, launch ./mimic, wait for mimic, then wait for sample to flush.
# Exactly one mimic process runs at any time.  Sampling interval 1 ms, duration cap 10 s
# (sample exits when the target dies).
#
# Aborts on a failed mimic run, a failed sampler, or a report with no call graph -- an
# empty or truncated report would otherwise reach attribute.py as silently missing samples.
set -u

REPO="${MIMIC_REPO:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"

if [ $# -ne 3 ]; then
  echo "usage: sample_runs.sh <run.yaml> <repeats> <outdir>" >&2
  exit 2
fi
CFG="$1"; N="$2"; OUT="$3"

case "$N" in
  ''|*[!0-9]*) echo "ABORT: repeats must be a positive integer, got '$N'" >&2; exit 2 ;;
esac
if [ "$N" -lt 1 ]; then
  echo "ABORT: repeats must be at least 1, got $N" >&2
  exit 2
fi

if ! [ -x /usr/bin/sample ]; then
  echo "ABORT: /usr/bin/sample not found; this harness is macOS only (see README.md)" >&2
  exit 2
fi

# Refuse a directory that already holds reports: attribute.py aggregates every
# sample_*.txt it finds, so leftovers from a previous collection would be silently
# mixed into the new profile.  Reports are never deleted here; move them aside.
if compgen -G "$OUT"/sample_*.txt >/dev/null 2>&1; then
  echo "ABORT: $OUT already contains sample_*.txt; move them aside first" >&2
  exit 2
fi

mkdir -p "$OUT"
cd "$REPO" || exit 1

for i in $(seq 1 "$N"); do
  f="$OUT/sample_$(printf %02d "$i").txt"
  /usr/bin/sample -wait mimic 10 1 -mayDie -fullPaths -f "$f" >/dev/null 2>&1 &
  spid=$!
  sleep 0.35            # let sample enter its wait loop before the target starts
  t0=$(python3 -c 'import time; print(time.perf_counter())')
  ./mimic "$CFG" -q > "$OUT/run_$(printf %02d "$i").log" 2>&1
  rc=$?
  t1=$(python3 -c 'import time; print(time.perf_counter())')

  wait "$spid"
  srn=$?
  if [ $rc -ne 0 ]; then
    echo "ABORT: mimic rc=$rc on repeat $i; see $OUT/run_$(printf %02d "$i").log" >&2
    exit 1
  fi
  if [ $srn -ne 0 ]; then
    echo "ABORT: sample rc=$srn on repeat $i; no usable profile for this run" >&2
    exit 1
  fi
  if ! grep -q '^Call graph:' "$f"; then
    echo "ABORT: $f has no call graph; the sampler produced an unusable report" >&2
    exit 1
  fi

  wall=$(python3 -c "print(f'{$t1 - $t0:.4f}')")
  echo "repeat $i rc=$rc wall=$wall lines=$(wc -l < "$f")"
done
exit 0
