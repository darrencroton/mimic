#!/bin/sh
# build.sh -- compile the SAGE26 function-level oracle.
#
# Compiles SAGE26's twelve physics sources (the list SAGE26's own tests/Makefile
# links its unit tests against) from the staged, read-only SAGE26 source, with
# the compiler and flags that the T1 reference build recorded, and links them
# with the oracle main oracle_c03.c into a standalone executable. Nothing is
# written in the SAGE26 source tree: objects and the executable go under the
# worktree's git-ignored output/oracle/. The oracle never links Mimic code.
# The c04 oracle main, oracle_c04.c, is compiled and linked the same way, with
# the same objects, compiler and flags, into output/oracle/oracle_c04.
#
# Environment:
#   SAGE26_SRC         staged SAGE26 source root
#                      (default /Volumes/Internal/results/sage26-campaign/reference-src)
#   ORACLE_OUT         build directory (default <repository>/output/oracle)
#   ORACLE_ALLOW_ANY_TOOLCHAIN=1
#                      build even when the compiler or SAGE26 commit differ from
#                      the pinned oracle environment (outputs are then NOT valid
#                      reference fixtures; for experiments only)
#
# See README.md in this directory for the environment and the bit-for-bit scope.

set -eu

ORACLE_DIR=$(cd "$(dirname "$0")" && pwd)
REPO_ROOT=$(cd "$ORACLE_DIR/../../../../.." && pwd)

SAGE26_SRC=${SAGE26_SRC:-/Volumes/Internal/results/sage26-campaign/reference-src}
ORACLE_OUT=${ORACLE_OUT:-$REPO_ROOT/output/oracle}
OBJ_DIR=$ORACLE_OUT/obj

# Pinned oracle environment (README.md, "Oracle environment").
EXPECTED_CLANG='Apple clang version 21.0.0 (clang-2100.4.1.1)'
EXPECTED_SAGE26_COMMIT=11b1ae851524baadb76cfd17e0b38f8a89c07224

# GSL location the reference build used (reference-build.log).
GSL_PREFIX=/opt/homebrew/Cellar/gsl/2.8

# SAGE26 tests/Makefile PROJECT_SOURCES, in that order.
SAGE26_SOURCES="core_utils model_misc model_h2_chemistry model_halo_properties model_regimes
model_starformation_and_feedback model_cooling_heating model_reincorporation model_infall
model_disk_instability model_mergers core_cool_func"

fail() {
  echo "build.sh: error: $*" >&2
  exit 1
}

[ -f "$SAGE26_SRC/src/model_infall.c" ] || fail "SAGE26 source not found at $SAGE26_SRC"

clang_version=$(clang --version | head -n 1)
sage26_commit=$(git -C "$SAGE26_SRC" rev-parse HEAD 2>/dev/null || echo unknown)
if [ "$clang_version" != "$EXPECTED_CLANG" ] || [ "$sage26_commit" != "$EXPECTED_SAGE26_COMMIT" ]; then
  if [ "${ORACLE_ALLOW_ANY_TOOLCHAIN:-0}" != "1" ]; then
    fail "environment differs from the pinned oracle environment
  compiler: '$clang_version' (expected '$EXPECTED_CLANG')
  SAGE26:   '$sage26_commit' (expected '$EXPECTED_SAGE26_COMMIT')
  set ORACLE_ALLOW_ANY_TOOLCHAIN=1 to build anyway (outputs are not reference fixtures)"
  fi
  echo "build.sh: warning: building outside the pinned oracle environment" >&2
fi

mkdir -p "$OBJ_DIR"

# Compile one translation unit with the reference build's recorded flags, in
# the reference build's argument order. Only -DROOT_DIR differs in value: it
# names the staged source instead of the reference workspace's copy.
compile() {
  # $1 = source path, $2 = object path, remaining = extra arguments
  src=$1
  obj=$2
  shift 2
  set -x
  clang -DROOT_DIR="\"$SAGE26_SRC\"" -DVERBOSE -DGSL_FOUND -DHDF5 -DGITREF_STR='"unknown"' \
    -O2 -march=native -std=gnu99 -fPIC -I"$GSL_PREFIX/include" -DUSE_BUFFERED_WRITE \
    -I/opt/homebrew/include -g -Wextra -Wshadow -Wall -Wno-unused-local-typedefs \
    "$@" -c "$src" -o "$obj"
  { set +x; } 2>/dev/null
}

objects=""
for name in $SAGE26_SOURCES; do
  # Run from the SAGE26 root and pass src/<name>.c, as the reference build did.
  (cd "$SAGE26_SRC" && compile "src/$name.c" "$OBJ_DIR/$name.o")
  objects="$objects $OBJ_DIR/$name.o"
done

# The oracle main: same flags, plus the SAGE26 include path for its headers.
compile "$ORACLE_DIR/oracle_c03.c" "$OBJ_DIR/oracle_c03.o" -I"$SAGE26_SRC/src"

# Link as the reference build linked: the GSL libraries with an rpath, and -lm.
set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c03.o" -o "$ORACLE_OUT/oracle_c03" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c03"

# The c04 oracle main (cooling and radio heating): built exactly as oracle_c03,
# against the same twelve objects, with the same flags and link.
compile "$ORACLE_DIR/oracle_c04.c" "$OBJ_DIR/oracle_c04.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c04.o" -o "$ORACLE_OUT/oracle_c04" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c04"
