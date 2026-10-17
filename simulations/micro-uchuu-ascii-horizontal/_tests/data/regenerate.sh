#!/bin/bash
###############################################################################
# regenerate.sh - Rebuild this package's committed worked_ascii test fixture
#
# Rewrites the committed synthetic Consistent-Trees ASCII source under source/
# from its generator (source/generate_source.py: three forests over six
# snapshots at the last six scale factors of micro-uchuu-ascii's snapshot
# list), then converts that source under simulations/micro-uchuu-ascii/'s own
# metadata and converter profile and installs the validated version 3 dataset
# and its a_list under worked_ascii/ here. The generic test tiers run on it
# through _tests/input/test_simulation.yaml. See
# source/convert_worked_ascii.sh for what is converted and how.
#
# Usage (from anywhere):
#   simulations/micro-uchuu-ascii-horizontal/_tests/data/regenerate.sh
#
# Exit codes: 0 on success; non-zero if the generator or any converter stage fails.
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"

# Resolved from REPO_ROOT at run time, so shellcheck cannot follow it without -x.
# shellcheck source=scripts/lib/python.sh disable=SC1091
. "${REPO_ROOT}/scripts/lib/python.sh"

SOURCE_DIR="${REPO_ROOT}/simulations/micro-uchuu-ascii-horizontal/_tests/data/source"

"$MIMIC_PYTHON" "${SOURCE_DIR}/generate_source.py"
exec "${SOURCE_DIR}/convert_worked_ascii.sh" micro-uchuu-ascii-horizontal micro-uchuu-ascii
