#!/bin/sh
# Build and run every control of the SAGE26 T1 parity instrument.
#
# Run from the worktree root:   sh models/sage26/instrument/controls/run_controls.sh
#
# Builds each control under a new directory output/instrument-controls/run-<UTC stamp>/
# (nothing outside output/instrument-controls/ is written, and nothing is deleted), runs
# compare_sage26.py on each with the mimic_venv Python, and prints each control's
# expected and actual verdict, exit status and the facts its criterion names. The T1
# reference's SHA-256 is printed before and after the run.
#
# Exit status: 0 when every control meets its expectation and the reference digest is
# unchanged; 1 when any control misses its expectation or the digest changed; 2 when the
# controls cannot be built (wrong directory, missing Python or reference, a generator
# failure).
#
# Environment: T1_REFERENCE overrides the T1 reference path.

set -u

HERE=$(cd "$(dirname "$0")" && pwd) || exit 2
INSTRUMENT=$(dirname "$HERE")
COMPARATOR=$INSTRUMENT/compare_sage26.py
PY=./mimic_venv/bin/python
T1_REFERENCE=${T1_REFERENCE:-/Volumes/Internal/results/sage26-campaign/workspace/e01-reference/out/ref/model_0.hdf5}
T1_SHA256=19977093c0497339b954cb4fc0417ff249fde0de794319c224a8b9ea9e001d18
OUT_BASE=output/instrument-controls

CONTROLS="S0 S0e S1a S1b S1c S1d S1e S1f S1g S2a S2b S3a S3b S3c S4a S4b S5a S5b S5c S6
S7a S7b S7c G1 G1s63 B1 B2 B3 B4 G2 B5 B6 B7 B8a B8b B8c B9 B10a B10b B10c"

if [ ! -d .git ] && [ ! -f .git ]; then
    echo "run_controls.sh: run from the worktree root" >&2
    exit 2
fi
if [ ! -x "$PY" ]; then
    echo "run_controls.sh: $PY not found (the mimic_venv Python)" >&2
    exit 2
fi
if [ ! -f "$T1_REFERENCE" ]; then
    echo "run_controls.sh: T1 reference $T1_REFERENCE not found" >&2
    exit 2
fi

# Imported generator modules must not leave bytecode caches beside the sources.
PYTHONDONTWRITEBYTECODE=1
export PYTHONDONTWRITEBYTECODE

RUN=$OUT_BASE/run-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$OUT_BASE" || exit 2
mkdir "$RUN" || exit 2
echo "controls directory: $RUN"

SHA_BEFORE=$(shasum -a 256 "$T1_REFERENCE" | cut -d' ' -f1)
echo "T1 reference SHA-256 before: $SHA_BEFORE"

echo
echo "== building controls"
"$PY" "$HERE/make_fixture.py" --out "$RUN" --comparator "$COMPARATOR" || exit 2
"$PY" "$HERE/alter_map.py" --out "$RUN" --comparator "$COMPARATOR" || exit 2
"$PY" "$HERE/to_mimic_layout.py" --reference "$T1_REFERENCE" --reference-sha256 "$T1_SHA256" \
    --out "$RUN" --comparator "$COMPARATOR" || exit 2
"$PY" "$HERE/perturb.py" --reference "$T1_REFERENCE" --out "$RUN" --comparator "$COMPARATOR" \
    || exit 2

echo
echo "== running controls"
MET=0
MISSED=""
for c in $CONTROLS; do
    echo
    if [ ! -f "$RUN/$c/expect.json" ]; then
        echo "== $c: not built"
        MISSED="$MISSED $c"
        continue
    fi
    if "$PY" "$HERE/run_control.py" "$RUN/$c"; then
        MET=$((MET + 1))
    else
        MISSED="$MISSED $c"
    fi
done

SHA_AFTER=$(shasum -a 256 "$T1_REFERENCE" | cut -d' ' -f1)
echo
echo "T1 reference SHA-256 before: $SHA_BEFORE"
echo "T1 reference SHA-256 after:  $SHA_AFTER"
STATUS=0
if [ "$SHA_BEFORE" != "$SHA_AFTER" ]; then
    echo "the T1 reference changed during the run"
    STATUS=1
fi
TOTAL=$(printf '%s\n' "$CONTROLS" | wc -w | tr -d ' ')
echo "controls meeting their expectation: $MET of $TOTAL"
if [ -n "$MISSED" ]; then
    echo "controls missing their expectation:$MISSED"
    STATUS=1
fi
echo "run_controls: $([ "$STATUS" -eq 0 ] && echo "all controls met" || echo "FAILED")"
exit $STATUS
