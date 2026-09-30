#!/usr/bin/env bash
# Write the build-time git version header: scripts/generate_git_version.sh <output-path>
#
# The one source of the header's contents for the Makefile, tests/unit/run_tests.sh
# and tests/unit/tools/build_topology_dump.sh, so every build of version.c,
# run_log.c and the HDF5 metadata writer sees the same macros. Outside a git
# checkout (an exported tarball) every git-derived value degrades to "unknown";
# a checkout without tags (a shallow CI clone) records the commit hash as the
# version through `git describe --always`.
set -u

if [ $# -ne 1 ]; then
    echo "usage: $0 <output-path>" >&2
    exit 2
fi
out="$1"
mkdir -p "$(dirname "$out")"

git_value() {
    "$@" 2>/dev/null || echo 'unknown'
}

{
    echo "#ifndef GIT_VERSION_H"
    echo "#define GIT_VERSION_H"
    echo "#define GIT_VERSION \"$(git_value git describe --tags --always --dirty)\""
    echo "#define GIT_COMMIT \"$(git_value git rev-parse HEAD)\""
    echo "#define GIT_BRANCH \"$(git_value git rev-parse --abbrev-ref HEAD)\""
    echo "#define GIT_DATE \"$(git_value git log -1 --format=%cd --date=short)\""
    echo "#define BUILD_DATE \"$(date '+%Y-%m-%d')\""
    echo "#endif"
} > "$out"
