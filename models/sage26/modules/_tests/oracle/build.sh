#!/bin/sh
# build.sh -- compile the SAGE26 function-level oracle.
#
# Compiles SAGE26's twelve physics sources (the list SAGE26's own tests/Makefile
# links its unit tests against) from the staged, read-only SAGE26 source, with
# the compiler and flags that the SAGE26 reference build recorded (README.md,
# "Oracle Environment"), and links them with the oracle main oracle_c03.c into a
# standalone executable. Nothing is written in the SAGE26 source tree: objects
# and the executable go under the worktree's git-ignored output/oracle/. The
# oracle never links Mimic code. The oracle main oracle_c04.c is compiled and
# linked the same way, with the same objects, compiler and flags, into
# output/oracle/oracle_c04, and so are oracle_c05.c into output/oracle/oracle_c05,
# oracle_c06.c into output/oracle/oracle_c06, oracle_c07.c into
# output/oracle/oracle_c07 and oracle_c08.c into output/oracle/oracle_c08.
# oracle_c10.c is compiled and linked the same way into output/oracle/oracle_c10; it
# defines the test-only rand() provider (the fixture's integer queue), which is linked
# into that executable only. oracle_c11.c (the CGMrecipeOn 1 cooling routines, the
# clustering-mass table, the CGM-on radio AGN, the regime-aware wrapper and the
# transcribed writer conversion of the mdot rates) is compiled and linked the same way
# into output/oracle/oracle_c11. oracle_c12.c (the halo concentration:
# concentration_from_vmax_vvir and get_halo_concentration, ConcentrationOn 0 and 3) is
# compiled and linked the same way into output/oracle/oracle_c12. oracle_c13.c (the
# feedback-free-burst classifier: calculate_ffb_threshold_mass, calculate_ffb_fraction and
# determine_and_store_ffb_regime, EnhancedStarFormationOn 1) is compiled and linked the
# same way into output/oracle/oracle_c13; like oracle_c10.c it defines a test-only rand()
# provider, linked into that executable only. oracle_c13.c also calls SAGE26's
# starformation_ffb and the wrapper starformation_and_feedback's FFB dispatch (the same
# twelve objects; no new SAGE26 source).
#
# Environment:
#   SAGE26_SRC         staged SAGE26 source root
#                      (default /Volumes/Internal/results/sage26-campaign/reference-src)
#   ORACLE_OUT         build directory (default <repository>/output/oracle)
#   ORACLE_ALLOW_ANY_TOOLCHAIN=1
#                      build even when the compiler, the SAGE26 commit, the
#                      staged tree's cleanliness or its cooling tables differ
#                      from the pinned oracle environment (outputs are then NOT
#                      valid reference fixtures; for experiments only)
#
# Prerequisites: the staged SAGE26 source at commit 11b1ae8, clean (git status
# --porcelain --ignored empty), with its eight cooling tables, and the pinned Apple clang;
# build.sh refuses anything else. Fixtures regenerated in any other environment
# are not reference fixtures and must not be committed.
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
# The macOS build the fixtures were generated on (kern.osversion; the reader's ORACLE_MACOS_BUILD).
EXPECTED_MACOS_BUILD=26A434

# GSL location the reference build used (README.md, "Oracle Environment").
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
os_build=$(sysctl -n kern.osversion 2>/dev/null || echo unknown)
sage26_commit=$(git -C "$SAGE26_SRC" rev-parse HEAD 2>/dev/null || echo unknown)
# The staged tree must be the commit's tree exactly: no modified, deleted, untracked or ignored
# file. The eight cooling tables read_cooling_functions() opens (src/auxdata/CoolFunctions/*.cie)
# are tracked files of that commit, so a clean tree also has them, unmodified; they are counted
# as well because SAGE26 exits 0 and writes nothing when one is missing (README.md,
# "Regenerating").
sage26_status=$(git -C "$SAGE26_SRC" status --porcelain --ignored 2>/dev/null ||
  echo "not a git work tree")
cooling_tables=$(find "$SAGE26_SRC/src/auxdata/CoolFunctions" -name '*.cie' 2>/dev/null |
  wc -l | tr -d ' ')
tree_state=clean
[ -z "$sage26_status" ] || tree_state="not clean (git status --porcelain --ignored is not empty)"
if [ "$clang_version" != "$EXPECTED_CLANG" ] || [ "$os_build" != "$EXPECTED_MACOS_BUILD" ] ||
  [ "$sage26_commit" != "$EXPECTED_SAGE26_COMMIT" ] || [ -n "$sage26_status" ] ||
  [ "$cooling_tables" != 8 ]; then
  if [ "${ORACLE_ALLOW_ANY_TOOLCHAIN:-0}" != "1" ]; then
    fail "environment differs from the pinned oracle environment
  compiler: '$clang_version' (expected '$EXPECTED_CLANG')
  macOS build: '$os_build' (expected '$EXPECTED_MACOS_BUILD')
  SAGE26:   '$sage26_commit' (expected '$EXPECTED_SAGE26_COMMIT')
  tree:     $tree_state
  cooling tables: $cooling_tables of 8 (src/auxdata/CoolFunctions/*.cie)
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

# The oracle main oracle_c04.c (cooling and radio heating): built exactly as
# oracle_c03, against the same twelve objects, with the same flags and link.
compile "$ORACLE_DIR/oracle_c04.c" "$OBJ_DIR/oracle_c04.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c04.o" -o "$ORACLE_OUT/oracle_c04" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c04"

# The oracle main oracle_c05.c (star formation, feedback, instability, histories
# and outflow): built exactly as oracle_c03, against the same twelve objects,
# with the same flags and link.
compile "$ORACLE_DIR/oracle_c05.c" "$OBJ_DIR/oracle_c05.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c05.o" -o "$ORACLE_OUT/oracle_c05" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c05"

# The oracle main oracle_c06.c (merger clocks, mergers, black holes, bursts, disruption and the
# driver transcriptions): built exactly as oracle_c03, against the same twelve objects, with the
# same flags and link.
compile "$ORACLE_DIR/oracle_c06.c" "$OBJ_DIR/oracle_c06.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c06.o" -o "$ORACLE_OUT/oracle_c06" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c06"

# The oracle main oracle_c07.c (the bulge-size routines and the central's synchronisation
# transcription): built exactly as oracle_c03, against the same twelve objects, with the same
# flags and link.
compile "$ORACLE_DIR/oracle_c07.c" "$OBJ_DIR/oracle_c07.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c07.o" -o "$ORACLE_OUT/oracle_c07" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c07"

# The oracle main oracle_c08.c (the BR06 molecular fraction and its radial integration): built
# exactly as oracle_c03, against the same twelve objects, with the same flags and link.
compile "$ORACLE_DIR/oracle_c08.c" "$OBJ_DIR/oracle_c08.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c08.o" -o "$ORACLE_OUT/oracle_c08" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c08"

# The oracle main oracle_c10.c (the CGM regime classifier, determine_and_store_regime): built
# exactly as oracle_c03, against the same twelve objects, with the same flags and link. Its own
# object defines rand(), the test-only queue provider, which the linker prefers to libc's for
# the SAGE26 objects' calls in this executable only.
compile "$ORACLE_DIR/oracle_c10.c" "$OBJ_DIR/oracle_c10.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c10.o" -o "$ORACLE_OUT/oracle_c10" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c10"

# The oracle main oracle_c11.c (the clustering-mass table, the CGMrecipeOn 1 hot recipe with its
# cold streams, bulk CGM cooling, the CGM-on radio AGN, the regime-aware wrapper and the
# transcribed writer conversion of the mdot rates): built exactly as oracle_c03, against the
# same twelve objects, with the same flags and link.
compile "$ORACLE_DIR/oracle_c11.c" "$OBJ_DIR/oracle_c11.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c11.o" -o "$ORACLE_OUT/oracle_c11" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c11"

# The oracle main oracle_c12.c (the halo concentration: the Vmax/Vvir inversion and the
# ConcentrationOn 0 and 3 dispatch of get_halo_concentration, both called directly): built
# exactly as oracle_c03, against the same twelve objects, with the same flags and link.
compile "$ORACLE_DIR/oracle_c12.c" "$OBJ_DIR/oracle_c12.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c12.o" -o "$ORACLE_OUT/oracle_c12" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c12"

# The oracle main oracle_c13.c (the feedback-free-burst classifier: the Li et al. 2024 threshold
# and fraction and determine_and_store_ffb_regime, each called directly): built exactly as
# oracle_c03, against the same twelve objects, with the same flags and link. Its own object
# defines rand(), the test-only queue provider for SAGE26's discarded draw, which the linker
# prefers to libc's for the SAGE26 objects' calls in this executable only.
compile "$ORACLE_DIR/oracle_c13.c" "$OBJ_DIR/oracle_c13.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c13.o" -o "$ORACLE_OUT/oracle_c13" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c13"
