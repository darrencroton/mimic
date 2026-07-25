#!/bin/bash
###############################################################################
# build_topology_dump.sh - Build the reference-topology dump harness
#
# Compiles tests/unit/tools/dump_ctrees_topology.c against the minimal
# production sources needed to read merger trees (util, parameter parsing,
# io/tree), and links a standalone executable. Does not modify run_tests.sh
# or any other existing test file; this is a new, independent build path for
# a new, independent tool.
#
# Usage:
#   ./build_topology_dump.sh
#   MODEL=halos-only SIMULATION=micro-uchuu-ascii ./build_topology_dump.sh
#
# Output: tests/unit/tools/build/dump_ctrees_topology
# Exit codes: 0 success, 1 build/link failure, 2 precondition failure
###############################################################################

set -o pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$REPO_ROOT" || exit 1

. "${REPO_ROOT}/scripts/lib/defaults.sh"
. "${REPO_ROOT}/scripts/lib/colors.sh"
. "${REPO_ROOT}/scripts/lib/hdf5.sh"
MODEL="${MODEL:-$DEFAULT_MODEL}"
SIMULATION="${SIMULATION:-$DEFAULT_SIMULATION}"
export MODEL SIMULATION
MODEL_ROOT="models/${MODEL}"

TOOL_DIR="tests/unit/tools"
BUILD_DIR="${TOOL_DIR}/build"
OBJ_DIR="${BUILD_DIR}/obj"
OUT_EXE="${BUILD_DIR}/dump_ctrees_topology"
SRC_DIR="src"

mkdir -p "$OBJ_DIR"

# Refresh generated metadata so the tool is built against current model
# properties/modules, matching run_tests.sh's precondition.
if ! python3 scripts/generate_properties.py > /dev/null; then
    echo -e "${RED}ERROR: Failed to refresh property code. Run 'make MODEL=<name> generate'${NC}"
    exit 2
fi
if ! python3 scripts/generate_module_registry.py > /dev/null; then
    echo -e "${RED}ERROR: Failed to refresh module registry. Run 'make generate'${NC}"
    exit 2
fi

# Generate git_version.h if it doesn't exist (needed by version.c)
GIT_VERSION_H="build/generated/git_version.h"
if [ ! -f "$GIT_VERSION_H" ]; then
    mkdir -p "$(dirname "$GIT_VERSION_H")"
    if command -v git &> /dev/null && [ -d .git ]; then
        {
            echo "#ifndef GIT_VERSION_H"
            echo "#define GIT_VERSION_H"
            echo "#define GIT_COMMIT \"$(git rev-parse HEAD 2>/dev/null || echo 'unknown')\""
            echo "#define GIT_BRANCH \"$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')\""
            echo "#define GIT_DATE \"$(git log -1 --format=%cd --date=short 2>/dev/null || echo 'unknown')\""
            echo "#endif"
        } > "$GIT_VERSION_H"
    else
        cat > "$GIT_VERSION_H" << 'EOF'
#ifndef GIT_VERSION_H
#define GIT_VERSION_H
#define GIT_COMMIT "unknown"
#define GIT_BRANCH "unknown"
#define GIT_DATE "unknown"
#endif
EOF
    fi
fi

CC="${CC:-gcc}"
YAML_CFLAGS="$(pkg-config --cflags yaml-0.1 2>/dev/null || echo '')"
YAML_LDFLAGS="$(pkg-config --libs yaml-0.1 2>/dev/null || echo '-lyaml')"
CFLAGS="-Wall -Wextra -I. -I${SRC_DIR} -I${SRC_DIR}/include -I${SRC_DIR}/include/generated -I${SRC_DIR}/util -I${SRC_DIR}/core -I${SRC_DIR}/io -I${SRC_DIR}/module_system -Imodels -I${MODEL_ROOT} -Ibuild/generated -Itests -g -O0 -DMIMIC_COMPILED_MODEL=\"${MODEL}\" -DMIMIC_COMPILED_MODEL_PATH=\"${MODEL_ROOT}\" -DMIMIC_COMPILED_SIMULATION=\"${SIMULATION}\" ${YAML_CFLAGS}"
CFLAGS="${CFLAGS} -DMIMIC_TEST_BUILD"
LDFLAGS="-lm ${YAML_LDFLAGS}"

# HDF5 detection is shared with tests/unit/run_tests.sh (scripts/lib/hdf5.sh).
detect_hdf5
if [ "$HDF5_AVAILABLE" = "1" ]; then
    LDFLAGS="${LDFLAGS} ${HDF5_LDFLAGS}"
fi

# Same production source set as tests/unit/run_tests.sh's ALL_SRCS, minus
# TEST_STUBS (a test-framework-only file this tool never references).
UTIL_SRCS="${SRC_DIR}/util/memory.c ${SRC_DIR}/util/error.c ${SRC_DIR}/util/numeric.c ${SRC_DIR}/util/version.c ${SRC_DIR}/util/integration.c ${SRC_DIR}/util/io.c ${SRC_DIR}/util/run_log.c ${SRC_DIR}/util/progress.c"
# Deliberately excludes core/tree_driver.c, core/build_model.c, core/virial.c,
# core/timestep.c, core/inheritance.c, core/output_buffer.c, io/output/*, and
# the module system: this harness reimplements only the small partition/unit
# loop (reader hooks + tree/interface.c), never calls build_halo_tree() or any
# output writer, and registers no physics modules. Excluding them keeps the
# harness's dependency surface exactly as small as what it actually calls.
CORE_SRCS="${SRC_DIR}/core/allvars.c ${SRC_DIR}/core/read_parameter_file.c ${SRC_DIR}/core/init.c ${SRC_DIR}/core/galaxy_pool.c"
IO_SRCS="${SRC_DIR}/io/tree/interface.c ${SRC_DIR}/io/tree/binary.c ${SRC_DIR}/io/tree/registry.c ${SRC_DIR}/io/tree/chunk_plan.c ${SRC_DIR}/io/tree/read_ctrees_ascii.c ${SRC_DIR}/io/tree/ctrees/ctrees_utils.c ${SRC_DIR}/io/tree/ctrees/forest_utils.c"
if [ "$HDF5_AVAILABLE" = "1" ]; then
    IO_SRCS="${IO_SRCS} ${SRC_DIR}/io/tree/hdf5.c ${SRC_DIR}/io/tree/read_ctrees_hdf5.c"
fi

ALL_SRCS="${UTIL_SRCS} ${CORE_SRCS} ${IO_SRCS} ${TOOL_DIR}/dump_ctrees_topology.c"

echo -e "${BLUE}Compiling topology dump harness (MODEL=${MODEL} SIMULATION=${SIMULATION})...${NC}"
OBJS=""
COMPILE_LOG="${BUILD_DIR}/compile.log"
: > "$COMPILE_LOG"
for src in $ALL_SRCS; do
    obj="${OBJ_DIR}/$(echo "${src%.c}" | tr '/' '_').o"
    OBJS="$OBJS $obj"
    src_cflags="$CFLAGS"
    if [ "$HDF5_AVAILABLE" = "1" ]; then
        case "$src" in
            "${SRC_DIR}/io/tree/registry.c"|"${SRC_DIR}/io/tree/hdf5.c"|"${SRC_DIR}/io/tree/read_ctrees_hdf5.c")
                src_cflags="${src_cflags} -DHDF5 ${HDF5_CFLAGS}"
                ;;
        esac
    fi
    if ! $CC $src_cflags -c "$src" -o "$obj" >> "$COMPILE_LOG" 2>&1; then
        echo -e "${RED}ERROR: compilation failed for ${src}${NC}"
        cat "$COMPILE_LOG"
        exit 1
    fi
done

if ! $CC $OBJS -o "$OUT_EXE" $LDFLAGS >> "$COMPILE_LOG" 2>&1; then
    echo -e "${RED}ERROR: link failed${NC}"
    cat "$COMPILE_LOG"
    exit 1
fi

echo -e "${GREEN}Built ${OUT_EXE}${NC}"
