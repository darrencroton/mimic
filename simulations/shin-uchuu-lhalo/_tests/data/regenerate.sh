#!/bin/bash
###############################################################################
# regenerate.sh - Rebuild this package's committed worked_ascii_lhalo test fixture
#
# Converts simulations/shin-uchuu/'s committed worked_ascii fixture (three
# synthetic forests over six snapshots, a horizontal HDF5 version 3 dataset) to
# L-Halo binary files with convert/mimic-convert/write_vertical_trees.py, in the
# record this package's halo_properties.yaml declares and under its tree_name,
# and installs them under worked_ascii_lhalo/ here beside the conversion's
# file_plan.json and a copy of the source fixture's a_list. The plan caps files at
# two forests, so the fixture holds two files: file 0 with forests 0 and 1, file 1
# with forest 2. This is test-sized synthetic data: it exercises this package's
# compiled record and the lhalo_binary reader path, not the Shin-Uchuu halo
# population. The generic test tiers run on it through
# _tests/input/test_simulation.yaml.
#
# The conversion's manifest.json is not installed: it records the converting
# commit, command line and wall times, so it differs on every run, while the data
# files and file_plan.json depend only on the source fixture and
# halo_properties.yaml. The conversion directory is temporary and removed on exit.
# The script then checks that the installed data files are exactly the converted
# set, so a conversion that wrote fewer files can never leave a stale one behind.
#
# Usage (from anywhere):
#   simulations/shin-uchuu-lhalo/_tests/data/regenerate.sh
#
# Exit codes: 0 on success; non-zero if an input is missing, the plan or the
# conversion fails, or the installed data files differ from the converted set.
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$REPO_ROOT"

# Resolved from REPO_ROOT at run time, so shellcheck cannot follow it without -x.
# shellcheck source=scripts/lib/python.sh disable=SC1091
. "${REPO_ROOT}/scripts/lib/python.sh"

PACKAGE="simulations/shin-uchuu-lhalo"
SOURCE="simulations/shin-uchuu/_tests/data/worked_ascii"
HALO_PROPERTIES="${PACKAGE}/halo_properties.yaml"
FIXTURE_DIR="${PACKAGE}/_tests/data/worked_ascii_lhalo"
CONVERTER="convert/mimic-convert/write_vertical_trees.py"
TREE_NAME="shin-uchuu-cut"
MAX_FORESTS_PER_FILE=2

for required in "${SOURCE}/forests.h5" "${SOURCE}/worked_ascii.a_list" "$HALO_PROPERTIES" \
    "$CONVERTER"; do
    if [ ! -f "$required" ]; then
        echo "ERROR: required input not found: ${required}" >&2
        exit 1
    fi
done

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/mimic_worked_ascii_lhalo.XXXXXX")"
trap '/bin/rm -rf "$WORKDIR"' EXIT

"$MIMIC_PYTHON" "$CONVERTER" plan --input "$SOURCE" --halo-properties "$HALO_PROPERTIES" \
    --output "$WORKDIR" --max-forests-per-file "$MAX_FORESTS_PER_FILE"
"$MIMIC_PYTHON" "$CONVERTER" convert --input "$SOURCE" --halo-properties "$HALO_PROPERTIES" \
    --output "$WORKDIR" --tree-name "$TREE_NAME"

converted="$(cd "$WORKDIR" && ls "${TREE_NAME}".*)"
if [ "$(echo "$converted" | wc -l)" -lt 2 ]; then
    echo "ERROR: the conversion wrote fewer than two files:" >&2
    echo "$converted" >&2
    exit 1
fi

mkdir -p "$FIXTURE_DIR"
for name in $converted; do
    /bin/cp -f "${WORKDIR}/${name}" "$FIXTURE_DIR"/
done
/bin/cp -f "${WORKDIR}/file_plan.json" "${SOURCE}/worked_ascii.a_list" "$FIXTURE_DIR"/

# The copy overwrites by name and never removes, so a conversion that wrote fewer
# files than the committed fixture holds would leave stale files installed.
installed="$(cd "$FIXTURE_DIR" && ls "${TREE_NAME}".*)"
if [ "$converted" != "$installed" ]; then
    echo "ERROR: installed data files differ from the converted set in ${FIXTURE_DIR}" >&2
    echo "converted:" >&2
    echo "$converted" >&2
    echo "installed:" >&2
    echo "$installed" >&2
    exit 1
fi
echo "Installed the worked_ascii_lhalo fixture under ${FIXTURE_DIR}"
