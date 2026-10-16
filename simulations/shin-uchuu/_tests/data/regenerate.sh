#!/bin/bash
###############################################################################
# regenerate.sh - Rebuild this package's committed version 3 test fixture
#
# Writes the synthetic Consistent-Trees ASCII forests of
# simulations/micro-uchuu-ascii-horizontal/_tests/data/source/generate_source.py
# (three forests over six snapshots) at the last six scale factors of
# shin-uchuu-ascii's snapshot list, the same list this package carries, into a
# temporary directory, converts them with convert/mimic-convert/convert_trees.py
# (ingest, transpose, write, then report, which runs the producer validation
# battery) using the Shin-Uchuu converter profile
# (simulations/shin-uchuu-ascii/converter_columns.yaml) and the metadata of
# simulations/shin-uchuu-ascii/simulation_info.yaml, the vertical package this
# one is converted from and whose cosmology, box size, particle mass and
# identity multiplier it repeats unchanged, and installs the written dataset
# with its a_list under worked_ascii/, overwriting the files of the same names.
# The two packages share one halo schema, so one generator serves both. The
# generic test tiers run on the result through _tests/input/test_simulation.yaml.
#
# This is test-sized synthetic data: it exercises this package's compiled schema
# and reader path, not the Shin-Uchuu halo population. The converter is the only
# producer: nothing here edits a written file. The source and conversion workdir
# are temporary and removed on exit, so the conversion report is printed before
# it goes.
#
# Usage (from anywhere):
#   simulations/shin-uchuu/_tests/data/regenerate.sh
#
# Exit codes: 0 on success; non-zero if the generator or any converter stage fails.
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$REPO_ROOT"

# Resolved from REPO_ROOT at run time, so shellcheck cannot follow it without -x.
# shellcheck source=scripts/lib/python.sh disable=SC1091
. "${REPO_ROOT}/scripts/lib/python.sh"

FIXTURE_DIR="simulations/shin-uchuu/_tests/data/worked_ascii"
GENERATOR="simulations/micro-uchuu-ascii-horizontal/_tests/data/source/generate_source.py"
SIM_INFO="simulations/shin-uchuu-ascii/simulation_info.yaml"
PROFILE="simulations/shin-uchuu-ascii/converter_columns.yaml"
CONVERT="convert/mimic-convert/convert_trees.py"

WORKROOT="$(mktemp -d "${TMPDIR:-/tmp}/mimic_shin_uchuu_fixture.XXXXXX")"
trap 'rm -rf "$WORKROOT"' EXIT
SOURCE_DIR="${WORKROOT}/source"
WORKDIR="${WORKROOT}/convert"

"$MIMIC_PYTHON" "$GENERATOR" --package shin-uchuu-ascii --output-dir "$SOURCE_DIR"

"$MIMIC_PYTHON" "$CONVERT" ingest --workdir "$WORKDIR" --source-format consistent_trees_ascii \
    --simulation-info "$SIM_INFO" --a-list "${SOURCE_DIR}/worked_ascii.a_list" \
    --column-map "$PROFILE" --forests-list "${SOURCE_DIR}/forests.list" \
    --tree-file "${SOURCE_DIR}/tree_0_0_0.dat"
"$MIMIC_PYTHON" "$CONVERT" transpose --workdir "$WORKDIR"
"$MIMIC_PYTHON" "$CONVERT" write --workdir "$WORKDIR" --simulation-info "$SIM_INFO"
"$MIMIC_PYTHON" "$CONVERT" report --workdir "$WORKDIR"
cat "$WORKDIR/conversion_report.txt"

mkdir -p "$FIXTURE_DIR"
cp "$WORKDIR"/write/attempt_*/snapshot_*.h5 "$WORKDIR"/write/attempt_*/forests.h5 "$FIXTURE_DIR"/
cp "${SOURCE_DIR}/worked_ascii.a_list" "$FIXTURE_DIR"/
echo "Installed the worked_ascii fixture under ${FIXTURE_DIR}"
