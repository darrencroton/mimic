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
# the same objects, compiler and flags, into output/oracle/oracle_c04, and so is
# the c05 oracle main, oracle_c05.c, into output/oracle/oracle_c05, the c06
# oracle main, oracle_c06.c, into output/oracle/oracle_c06, the c07 oracle
# main, oracle_c07.c, into output/oracle/oracle_c07, and the c08 oracle main,
# oracle_c08.c, into output/oracle/oracle_c08. The c10 oracle main, oracle_c10.c,
# is compiled and linked the same way into output/oracle/oracle_c10; it defines the
# test-only rand() provider (the fixture's integer queue), which is linked into that
# executable only. The c11 oracle main, oracle_c11.c (the CGMrecipeOn 1 cooling routines,
# the clustering-mass table, the CGM-on radio AGN, the regime-aware wrapper and the
# transcribed writer conversion of the mdot rates), is compiled and linked the same way
# into output/oracle/oracle_c11. The c12 oracle main, oracle_c12.c (the halo concentration:
# concentration_from_vmax_vvir and get_halo_concentration, ConcentrationOn 0 and 3), is
# compiled and linked the same way into output/oracle/oracle_c12.
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

# The c05 oracle main (star formation, feedback, instability, histories and
# outflow): built exactly as oracle_c03, against the same twelve objects, with
# the same flags and link.
compile "$ORACLE_DIR/oracle_c05.c" "$OBJ_DIR/oracle_c05.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c05.o" -o "$ORACLE_OUT/oracle_c05" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c05"

# The c06 oracle main (merger clocks, mergers, black holes, bursts, disruption and the
# driver transcriptions): built exactly as oracle_c03, against the same twelve objects,
# with the same flags and link.
compile "$ORACLE_DIR/oracle_c06.c" "$OBJ_DIR/oracle_c06.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c06.o" -o "$ORACLE_OUT/oracle_c06" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c06"

# The c07 oracle main (the bulge-size routines and the central's synchronisation
# transcription): built exactly as oracle_c03, against the same twelve objects, with the
# same flags and link.
compile "$ORACLE_DIR/oracle_c07.c" "$OBJ_DIR/oracle_c07.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c07.o" -o "$ORACLE_OUT/oracle_c07" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c07"

# The c08 oracle main (the BR06 molecular fraction and its radial integration): built
# exactly as oracle_c03, against the same twelve objects, with the same flags and link.
compile "$ORACLE_DIR/oracle_c08.c" "$OBJ_DIR/oracle_c08.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c08.o" -o "$ORACLE_OUT/oracle_c08" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c08"

# The c10 oracle main (the CGM regime classifier, determine_and_store_regime): built exactly
# as oracle_c03, against the same twelve objects, with the same flags and link. Its own
# object defines rand(), the test-only queue provider, which the linker prefers to libc's for
# the SAGE26 objects' calls in this executable only.
compile "$ORACLE_DIR/oracle_c10.c" "$OBJ_DIR/oracle_c10.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c10.o" -o "$ORACLE_OUT/oracle_c10" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c10"

# The c11 oracle main (the clustering-mass table, the CGMrecipeOn 1 hot recipe with its cold
# streams, bulk CGM cooling, the CGM-on radio AGN, the regime-aware wrapper and the transcribed
# writer conversion of the mdot rates): built exactly as oracle_c03, against the same twelve
# objects, with the same flags and link.
compile "$ORACLE_DIR/oracle_c11.c" "$OBJ_DIR/oracle_c11.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c11.o" -o "$ORACLE_OUT/oracle_c11" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c11"

# The c12 oracle main (the halo concentration: the Vmax/Vvir inversion and the ConcentrationOn 0
# and 3 dispatch of get_halo_concentration, both called directly): built exactly as oracle_c03,
# against the same twelve objects, with the same flags and link.
compile "$ORACLE_DIR/oracle_c12.c" "$OBJ_DIR/oracle_c12.o" -I"$SAGE26_SRC/src"

set -x
# shellcheck disable=SC2086 # $objects is a deliberately split list of paths
clang $objects "$OBJ_DIR/oracle_c12.o" -o "$ORACLE_OUT/oracle_c12" \
  -L"$GSL_PREFIX/lib" -lgsl -lgslcblas -Xlinker -rpath -Xlinker "$GSL_PREFIX/lib" -lm
{ set +x; } 2>/dev/null

echo "build.sh: built $ORACLE_OUT/oracle_c12"
