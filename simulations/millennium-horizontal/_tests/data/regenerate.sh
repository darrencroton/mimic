#!/bin/bash
###############################################################################
# regenerate.sh - Rebuild this package's committed worked_graph test fixture
#
# Converts the simulation-neutral worked_graph L-Halo source
# (simulations/mini-millennium-horizontal/_tests/data/source/) under
# simulations/millennium/'s own metadata and converter profile, and installs the
# validated version 3 dataset under worked_graph/ here. The generic test tiers
# run on it through _tests/input/test_simulation.yaml. See
# simulations/mini-millennium-horizontal/_tests/data/source/convert_worked_graph.sh
# for what is converted and how.
#
# Usage (from anywhere):
#   simulations/millennium-horizontal/_tests/data/regenerate.sh
#
# Exit codes: 0 on success; non-zero if any converter stage fails.
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
exec "${REPO_ROOT}/simulations/mini-millennium-horizontal/_tests/data/source/convert_worked_graph.sh" \
    millennium-horizontal millennium millennium.a_list
