#!/bin/bash
###############################################################################
# convert_worked_ascii.sh - Build the worked_ascii version 3 test fixture for one
# Consistent-Trees-ASCII-sourced horizontal package
#
# The worked_ascii fixture is three synthetic forests (four trees in all) over
# six snapshots, written as Consistent-Trees ASCII by generate_source.py beside
# this script. The ASCII source embeds scale factors, so each package needs its
# own source: this script generates it at the last six scale factors of the
# vertical package's snapshot list into a temporary directory, converts it with
# convert/mimic-convert/convert_trees.py (ingest, transpose, write, then report,
# which runs the producer validation battery) under that vertical package's own
# simulation_info.yaml and converter profile, so the fixture's headers carry
# exactly the cosmology, box size, particle mass and scale factors the
# horizontal package declares and its reader checks at open, and installs the
# written dataset and its a_list under
# simulations/<horizontal package>/_tests/data/worked_ascii/, overwriting the
# files of the same names. It then checks that the installed snapshot_*.h5 files
# are exactly the ones the conversion wrote, so a source with fewer snapshots
# can never leave a stale file behind. The converter is the only producer:
# nothing here edits a written file. The generated source and the conversion
# workdir are temporary and removed on exit, so the conversion report is printed
# before they go.
#
# Usage (from anywhere; called by each package's _tests/data/regenerate.sh):
#   convert_worked_ascii.sh <horizontal package> <vertical source package>
#   e.g. convert_worked_ascii.sh shin-uchuu shin-uchuu-ascii
#
# Exit codes: 0 on success; non-zero if an argument is wrong, an input is
# missing, any converter stage fails or the installed snapshot files differ from
# the converted set.
###############################################################################

set -euo pipefail

if [ "$#" -ne 2 ]; then
    echo "usage: $0 <horizontal package> <vertical source package>" >&2
    exit 2
fi
PACKAGE="$1"
SOURCE_PACKAGE="$2"

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
cd "$REPO_ROOT"

# Resolved from REPO_ROOT at run time, so shellcheck cannot follow it without -x.
# shellcheck source=scripts/lib/python.sh disable=SC1091
. "${REPO_ROOT}/scripts/lib/python.sh"

GENERATOR="simulations/micro-uchuu-ascii-horizontal/_tests/data/source/generate_source.py"
S="simulations/${SOURCE_PACKAGE}"
FIXTURE_DIR="simulations/${PACKAGE}/_tests/data/worked_ascii"
CONVERT="convert/mimic-convert/convert_trees.py"

for required in "$GENERATOR" "${S}/simulation_info.yaml" "${S}/converter_columns.yaml"; do
    if [ ! -f "$required" ]; then
        echo "ERROR: required input not found: ${required}" >&2
        exit 1
    fi
done
if [ ! -d "simulations/${PACKAGE}" ]; then
    echo "ERROR: no such horizontal package: simulations/${PACKAGE}" >&2
    exit 1
fi

WORKROOT="$(mktemp -d "${TMPDIR:-/tmp}/mimic_worked_ascii_${PACKAGE}.XXXXXX")"
trap '/bin/rm -rf "$WORKROOT"' EXIT
source_dir="${WORKROOT}/source"
workdir="${WORKROOT}/convert"

"$MIMIC_PYTHON" "$GENERATOR" --package "$SOURCE_PACKAGE" --output-dir "$source_dir" > /dev/null

"$MIMIC_PYTHON" "$CONVERT" ingest --workdir "$workdir" --source-format consistent_trees_ascii \
    --simulation-info "${S}/simulation_info.yaml" --a-list "${source_dir}/worked_ascii.a_list" \
    --column-map "${S}/converter_columns.yaml" --forests-list "${source_dir}/forests.list" \
    --tree-file "${source_dir}/tree_0_0_0.dat"
"$MIMIC_PYTHON" "$CONVERT" transpose --workdir "$workdir"
"$MIMIC_PYTHON" "$CONVERT" write --workdir "$workdir" --simulation-info "${S}/simulation_info.yaml"
"$MIMIC_PYTHON" "$CONVERT" report --workdir "$workdir"
cat "$workdir/conversion_report.txt"

attempts=("$workdir"/write/attempt_*)
if [ "${#attempts[@]}" -ne 1 ] || [ ! -d "${attempts[0]}" ]; then
    echo "ERROR: expected exactly one write attempt under ${workdir}/write" >&2
    exit 1
fi
attempt="${attempts[0]}"

mkdir -p "$FIXTURE_DIR"
/bin/cp -f "$attempt"/snapshot_*.h5 "$attempt"/forests.h5 "$FIXTURE_DIR"/
/bin/cp -f "${source_dir}/worked_ascii.a_list" "$FIXTURE_DIR"/

# The copy overwrites by name and never removes, so a conversion that wrote fewer
# snapshots than the committed fixture holds would leave stale files installed.
written="$(cd "$attempt" && ls snapshot_*.h5)"
installed="$(cd "$FIXTURE_DIR" && ls snapshot_*.h5)"
if [ "$written" != "$installed" ]; then
    echo "ERROR: installed snapshot files differ from the converted set in ${FIXTURE_DIR}" >&2
    echo "converted:" >&2
    echo "$written" >&2
    echo "installed:" >&2
    echo "$installed" >&2
    exit 1
fi
echo "Installed the worked_ascii fixture under ${FIXTURE_DIR}"
