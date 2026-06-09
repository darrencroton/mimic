#!/bin/bash
# scripts/lib/defaults.sh — Project defaults derived from the Makefile.
#
# Source this file from scripts that need DEFAULT_MODEL / DEFAULT_SIMULATION.
# Do not execute it directly.
#
# After sourcing, the following are available:
#   make_default KEY [FALLBACK]  — reads KEY from the Makefile, falls back to FALLBACK
#   DEFAULT_MODEL                — resolved default model package name
#   DEFAULT_SIMULATION           — resolved default simulation package name
#
# The Makefile is the single source of truth. The fallback values in the
# make_default calls below are last-resort safety nets only.

_LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_REPO_ROOT="$(cd "${_LIB_DIR}/../.." && pwd)"

make_default() {
    local key="$1" fallback="${2:-}" value
    value=$(awk -v key="$key" '$1 == key && $2 == ":=" { print $3; exit }' "${_REPO_ROOT}/Makefile")
    echo "${value:-$fallback}"
}

DEFAULT_MODEL="${DEFAULT_MODEL:-$(make_default DEFAULT_MODEL sage16)}"
DEFAULT_SIMULATION="${DEFAULT_SIMULATION:-$(make_default DEFAULT_SIMULATION mini-millennium)}"
export DEFAULT_MODEL DEFAULT_SIMULATION
