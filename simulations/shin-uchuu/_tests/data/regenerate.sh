#!/bin/bash
###############################################################################
# regenerate.sh - Rebuild this package's committed version 2 test fixture
#
# Runs the version 2 contract-fixture generator
# (simulations/micro-uchuu-horizontal/_tests/input/create_snapshot_fixture.py)
# for this package: the same synthetic Consistent-Trees forests, converted by
# the real convert/mimic-convert/convert_ctrees.py pipeline under this
# package's own simulation_info.yaml (box size, cosmology, particle mass) and
# the last six scale factors of its own snapshot list (so timesteps have the
# package's real spacing), validated by the producer battery, re-chunked small and installed here with
# shin-uchuu-fixture.a_list and fixture_manifest.json. The two packages share
# one halo schema, so one generator serves both. The generic test tiers run on
# the result through _tests/input/test_simulation.yaml.
#
# Usage (from anywhere):
#   simulations/shin-uchuu/_tests/data/regenerate.sh
#
# Exit codes: 0 on success; non-zero if any generator or converter stage fails.
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
cd "$REPO_ROOT"

# Resolved from REPO_ROOT at run time, so shellcheck cannot follow it without -x.
# shellcheck source=scripts/lib/python.sh disable=SC1091
. "${REPO_ROOT}/scripts/lib/python.sh"

exec "$MIMIC_PYTHON" simulations/micro-uchuu-horizontal/_tests/input/create_snapshot_fixture.py \
    --package shin-uchuu --package-scale-factors
