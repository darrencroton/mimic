#!/bin/bash
###############################################################################
# regenerate.sh - Rebuild this package's committed worked_ascii test fixture
#
# Writes the synthetic Consistent-Trees ASCII forests of
# simulations/micro-uchuu-ascii-horizontal/_tests/data/source/ (the two packages
# share one halo schema, so one generator serves both) at the last six scale
# factors of shin-uchuu-ascii's snapshot list, converts them under
# simulations/shin-uchuu-ascii/'s own metadata and converter profile, and
# installs the validated version 3 dataset and its a_list under worked_ascii/
# here. This is test-sized synthetic data: it exercises this package's compiled
# schema and reader path, not the Shin-Uchuu halo population. The generic test
# tiers run on it through _tests/input/test_simulation.yaml. See
# simulations/micro-uchuu-ascii-horizontal/_tests/data/source/convert_worked_ascii.sh
# for what is converted and how.
#
# Usage (from anywhere):
#   simulations/shin-uchuu/_tests/data/regenerate.sh
#
# Exit codes: 0 on success; non-zero if the generator or any converter stage fails.
###############################################################################

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../.." && pwd)"
SOURCE_DIR="${REPO_ROOT}/simulations/micro-uchuu-ascii-horizontal/_tests/data/source"
exec "${SOURCE_DIR}/convert_worked_ascii.sh" shin-uchuu shin-uchuu-ascii
