#!/bin/bash
###############################################################################
# regenerate.sh - Rebuild the committed horizontal-HDF5 version 3 reader fixture
#
# Regenerates tests/data/horizontal_v3/source/trees_fixture.0 from its
# generator, converts it with convert/mimic-convert/convert_trees.py (ingest,
# transpose, write, then report, which runs the producer validation battery),
# and installs the written dataset under tests/data/horizontal_v3/dataset/,
# overwriting the files of the same names. The converter is the only producer:
# nothing here edits a written file. The conversion report stays in the
# temporary workdir because it records that workdir's absolute path.
#
# The source, profile and a_list are committed alongside; the physical header
# values come from simulations/mini-millennium/simulation_info.yaml, whose
# cosmology, box size and particle mass the mini-millennium-horizontal package
# repeats unchanged.
#
# Usage (from anywhere):
#   tests/data/horizontal_v3/regenerate.sh
#
# Exit codes: 0 on success; non-zero if any converter stage fails.
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT"

# Resolved from REPO_ROOT at run time, so shellcheck cannot follow it without -x.
# shellcheck source=scripts/lib/python.sh disable=SC1091
. "${REPO_ROOT}/scripts/lib/python.sh"

FIXTURE_DIR="tests/data/horizontal_v3"
SOURCE_DIR="${FIXTURE_DIR}/source"
DATASET_DIR="${FIXTURE_DIR}/dataset"
SIM_INFO="simulations/mini-millennium/simulation_info.yaml"
HALO_PROPERTIES="simulations/mini-millennium/halo_properties.yaml"
CONVERT="convert/mimic-convert/convert_trees.py"

WORKDIR="$(mktemp -d "${TMPDIR:-/tmp}/mimic_horizontal_v3_fixture.XXXXXX")"
trap 'rm -rf "$WORKDIR"' EXIT

"$MIMIC_PYTHON" "${SOURCE_DIR}/generate_source.py"

"$MIMIC_PYTHON" "$CONVERT" ingest --workdir "$WORKDIR" --source-format lhalo_binary \
    --simulation-info "$SIM_INFO" --a-list "${SOURCE_DIR}/fixture.a_list" \
    --column-map "${SOURCE_DIR}/profile.yaml" --source-dir "$SOURCE_DIR" \
    --tree-name trees_fixture --halo-properties "$HALO_PROPERTIES" \
    --first-file 0 --last-file 0
"$MIMIC_PYTHON" "$CONVERT" transpose --workdir "$WORKDIR"
"$MIMIC_PYTHON" "$CONVERT" write --workdir "$WORKDIR" --simulation-info "$SIM_INFO"
"$MIMIC_PYTHON" "$CONVERT" report --workdir "$WORKDIR"

mkdir -p "$DATASET_DIR"
cp "$WORKDIR"/write/attempt_*/snapshot_*.h5 "$WORKDIR"/write/attempt_*/forests.h5 "$DATASET_DIR"/
cp "${SOURCE_DIR}/fixture.a_list" "$DATASET_DIR"/
echo "Installed the version 3 fixture under ${DATASET_DIR}"
