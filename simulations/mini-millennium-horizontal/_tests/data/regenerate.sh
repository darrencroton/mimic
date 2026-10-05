#!/bin/bash
###############################################################################
# regenerate.sh - Rebuild the committed gap-retention and forest-block version 3
#                 fixtures
#
# Regenerates the four L-Halo sources under source/ from their generator,
# converts each with convert/mimic-convert/convert_trees.py (ingest, transpose, write,
# then report, which runs the producer validation battery) using the
# mini-Millennium converter profile this package declares
# (simulations/mini-millennium/converter_columns.yaml), and installs each
# written dataset under the directory of the same name here, overwriting the
# files of the same names:
#
#   worked_graph/          the design review's worked five-halo mixed-gap graph
#   three_snapshot_chain/  one progenitor chain spanning three snapshots
#   adjacent/              an all-adjacent dataset (links_adjacent = 1)
#   forest_blocks/         six forests of unequal size over seven gapped
#                          snapshots, the distributed identity gate's fixture
#
# The converter is the only producer: nothing here edits a written file. Each
# conversion report stays in its temporary workdir because it records that
# workdir's absolute path.
#
# Usage (from anywhere):
#   simulations/mini-millennium-horizontal/_tests/data/regenerate.sh
#
# Exit codes: 0 on success; non-zero if any converter stage fails.
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$REPO_ROOT"

# Resolved from REPO_ROOT at run time, so shellcheck cannot follow it without -x.
# shellcheck source=scripts/lib/python.sh disable=SC1091
. "${REPO_ROOT}/scripts/lib/python.sh"

DATA_DIR="simulations/mini-millennium-horizontal/_tests/data"
SOURCE_DIR="${DATA_DIR}/source"
SIM_INFO="simulations/mini-millennium/simulation_info.yaml"
HALO_PROPERTIES="simulations/mini-millennium/halo_properties.yaml"
PROFILE="simulations/mini-millennium/converter_columns.yaml"
CONVERT="convert/mimic-convert/convert_trees.py"

WORKROOT="$(mktemp -d "${TMPDIR:-/tmp}/mimic_gap_retention_fixtures.XXXXXX")"
trap 'rm -rf "$WORKROOT"' EXIT

"$MIMIC_PYTHON" "${SOURCE_DIR}/generate_sources.py"

for fixture in worked_graph three_snapshot_chain adjacent forest_blocks; do
    workdir="${WORKROOT}/${fixture}"
    "$MIMIC_PYTHON" "$CONVERT" ingest --workdir "$workdir" --source-format lhalo_binary \
        --simulation-info "$SIM_INFO" --a-list "${SOURCE_DIR}/${fixture}.a_list" \
        --column-map "$PROFILE" --source-dir "$SOURCE_DIR" \
        --tree-name "trees_${fixture}" --halo-properties "$HALO_PROPERTIES" \
        --first-file 0 --last-file 0
    "$MIMIC_PYTHON" "$CONVERT" transpose --workdir "$workdir"
    "$MIMIC_PYTHON" "$CONVERT" write --workdir "$workdir" --simulation-info "$SIM_INFO"
    "$MIMIC_PYTHON" "$CONVERT" report --workdir "$workdir"

    mkdir -p "${DATA_DIR}/${fixture}"
    cp "$workdir"/write/attempt_*/snapshot_*.h5 "$workdir"/write/attempt_*/forests.h5 \
        "${DATA_DIR}/${fixture}"/
    cp "${SOURCE_DIR}/${fixture}.a_list" "${DATA_DIR}/${fixture}"/
    echo "Installed the ${fixture} fixture under ${DATA_DIR}/${fixture}"
done
