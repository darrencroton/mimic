#!/bin/bash
###############################################################################
# regenerate.sh - Rebuild this package's generic-tier test fixture
#
# The contract fixture one directory up (_tests/data/) is frozen by
# `make check-horizontal-fixture` and uses synthetic scale factors 0.25-1.0,
# whose ~2 Gyr timesteps are not a real snapshot spacing. The generic test
# tiers (through _tests/input/test_simulation.yaml) instead run on this copy:
# the same synthetic forests, converted by the same generator
# (../../input/create_snapshot_fixture.py) against the last six scale factors
# of this package's own snapshot list, and installed here.
#
# Usage (from anywhere):
#   simulations/micro-uchuu-horizontal/_tests/data/generic/regenerate.sh
#
# Exit codes: 0 on success; non-zero if any generator or converter stage fails.
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../../.." && pwd)"
cd "$REPO_ROOT"

# Resolved from REPO_ROOT at run time, so shellcheck cannot follow it without -x.
# shellcheck source=scripts/lib/python.sh disable=SC1091
. "${REPO_ROOT}/scripts/lib/python.sh"

exec "$MIMIC_PYTHON" simulations/micro-uchuu-horizontal/_tests/input/create_snapshot_fixture.py \
    --package-scale-factors --output-subdir generic
