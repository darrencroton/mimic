#!/bin/sh
# Build and run the controls of the SAGE26 T1 parity instrument.
#
# Run from the repository root:
#   T1_REFERENCE=/path/to/model_0.hdf5 sh models/sage26/tools/controls/run_controls.sh
#   sh models/sage26/tools/controls/run_controls.sh --synthetic
#
# Builds each control under a new directory output/instrument-controls/run-<UTC stamp>/
# (nothing outside output/instrument-controls/ is written, and nothing is deleted), runs
# compare_sage26.py on each with the mimic_venv Python, and prints each control's
# expected and actual verdict, exit status and the facts its criterion names. With no
# argument it runs every control, which needs the T1 reference (SAGE26's model_0.hdf5 in
# its default configuration, not in the repository) named by the environment variable
# T1_REFERENCE; its SHA-256 is printed before and after the run. With --synthetic it runs
# only the 23 synthetic controls (S0-S7c), which need no reference.
#
# Exit status: 0 when every control meets its expectation and the reference digest is
# unchanged; 1 when any control misses its expectation or the digest changed; 2 when the
# controls cannot be built (an unknown argument, wrong directory, missing Python,
# T1_REFERENCE unset or not a file, no shasum, a reference whose SHA-256 is not T1's, a
# generator failure), checked before building.

set -u

HERE=$(cd "$(dirname "$0")" && pwd) || exit 2
INSTRUMENT=$(dirname "$HERE")
COMPARATOR=$INSTRUMENT/compare_sage26.py
PY=./mimic_venv/bin/python
T1_REFERENCE=${T1_REFERENCE:-}
# The comparator's T1_REFERENCE_SHA256, repeated here because this is shell; the Python
# generators read it from compare_sage26.py itself.
T1_SHA256=19977093c0497339b954cb4fc0417ff249fde0de794319c224a8b9ea9e001d18
OUT_BASE=output/instrument-controls

CONTROLS="S0 S0e S1a S1b S1c S1d S1e S1f S1g S2a S2b S3a S3b S3c S4a S4b S5a S5b S5c S6
S7a S7b S7c G1 G1s63 B1 B2 B3 B4 G2 B5 B6 B7 B8a B8b B8c B9 B10a B10b B10c"

SYNTHETIC=0
case "$#:${1:-}" in
    0:) ;;
    1:--synthetic) SYNTHETIC=1 ;;
    *)
        echo "usage: sh models/sage26/tools/controls/run_controls.sh [--synthetic]" >&2
        exit 2
        ;;
esac
if [ "$SYNTHETIC" -eq 1 ]; then
    # shellcheck disable=SC2086  # CONTROLS is a space-separated list; the split is wanted
    CONTROLS=$(printf '%s\n' $CONTROLS | grep '^S' | tr '\n' ' ')
fi

if [ ! -d .git ] && [ ! -f .git ]; then
    echo "run_controls.sh: run from the repository root" >&2
    exit 2
fi
if [ ! -x "$PY" ]; then
    echo "run_controls.sh: $PY not found (the mimic_venv Python)" >&2
    exit 2
fi

if [ "$SYNTHETIC" -eq 0 ]; then
    if [ -z "$T1_REFERENCE" ]; then
        echo "run_controls.sh: set T1_REFERENCE to the T1 reference, or pass --synthetic" >&2
        exit 2
    fi
    if [ ! -f "$T1_REFERENCE" ]; then
        echo "run_controls.sh: T1 reference $T1_REFERENCE not found" >&2
        exit 2
    fi
    if ! command -v shasum > /dev/null 2>&1; then
        echo "run_controls.sh: shasum not found; the reference digest cannot be checked" >&2
        exit 2
    fi
    SHA_BEFORE=$(shasum -a 256 "$T1_REFERENCE" | cut -d' ' -f1)
    if [ -z "$SHA_BEFORE" ]; then
        echo "run_controls.sh: could not hash $T1_REFERENCE" >&2
        exit 2
    fi
    if [ "$SHA_BEFORE" != "$T1_SHA256" ]; then
        echo "run_controls.sh: $T1_REFERENCE has SHA-256 $SHA_BEFORE, not T1's $T1_SHA256" >&2
        exit 2
    fi
fi

# Imported generator modules must not leave bytecode caches beside the sources.
PYTHONDONTWRITEBYTECODE=1
export PYTHONDONTWRITEBYTECODE

RUN=$OUT_BASE/run-$(date -u +%Y%m%dT%H%M%SZ)
mkdir -p "$OUT_BASE" || exit 2
mkdir "$RUN" || exit 2
echo "controls directory: $RUN"

[ "$SYNTHETIC" -eq 0 ] && echo "T1 reference SHA-256 before: $SHA_BEFORE"

echo
echo "== building controls"
"$PY" "$HERE/make_fixture.py" --out "$RUN" --comparator "$COMPARATOR" || exit 2
"$PY" "$HERE/alter_map.py" --out "$RUN" --comparator "$COMPARATOR" || exit 2
if [ "$SYNTHETIC" -eq 0 ]; then
    "$PY" "$HERE/to_mimic_layout.py" --reference "$T1_REFERENCE" --reference-sha256 "$T1_SHA256" \
        --out "$RUN" --comparator "$COMPARATOR" || exit 2
    "$PY" "$HERE/perturb.py" --reference "$T1_REFERENCE" --out "$RUN" \
        --comparator "$COMPARATOR" || exit 2
fi

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

echo
STATUS=0
if [ "$SYNTHETIC" -eq 0 ]; then
    SHA_AFTER=$(shasum -a 256 "$T1_REFERENCE" | cut -d' ' -f1)
    echo "T1 reference SHA-256 before: $SHA_BEFORE"
    echo "T1 reference SHA-256 after:  $SHA_AFTER"
    if [ "$SHA_BEFORE" != "$SHA_AFTER" ]; then
        echo "the T1 reference changed during the run"
        STATUS=1
    fi
fi
TOTAL=$(printf '%s\n' "$CONTROLS" | wc -w | tr -d ' ')
echo "controls meeting their expectation: $MET of $TOTAL"
if [ -n "$MISSED" ]; then
    echo "controls missing their expectation:$MISSED"
    STATUS=1
fi
echo "run_controls: $([ "$STATUS" -eq 0 ] && echo "all controls met" || echo "FAILED")"
exit $STATUS
