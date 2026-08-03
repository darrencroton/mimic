#!/bin/bash
###############################################################################
# run_tests.sh - Unit test runner for Mimic
#
# This script compiles and runs all C unit tests in the tests/unit/ directory.
# It provides colored output, tracks pass/fail statistics, and returns
# appropriate exit codes for CI integration.
#
# Usage:
#   ./run_tests.sh              # Run all tests
#   ./run_tests.sh test_memory  # Run specific test
#
# Exit codes:
#   0 - All tests passed
#   1 - One or more tests failed (including compilation failures)
#   2 - Test preamble failed (code generation / registry refresh)
###############################################################################

# Flags and object lists are space-separated strings that must word-split into
# separate argv entries at each $CC call; quoting them would break every compile.
# shellcheck disable=SC2086,SC2089,SC2090

# Detect compiler failures even when piping to tee for logs
set -o pipefail

# Counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
SKIPPED_TESTS=0
COMPILE_ERRORS=0
FAILED_TEST_NAMES=""

# Get repository root (two levels up from tests/unit/)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT" || exit 1
TEST_FAILURES_FILE="${REPO_ROOT}/build/.test_failures"

# Package selection: the Makefile exports MODEL/SIMULATION when invoking this
# runner; standalone runs fall back to the shared project defaults. Exported so
# the generation scripts below see the same selection.
. "${REPO_ROOT}/scripts/lib/defaults.sh"
. "${REPO_ROOT}/scripts/lib/colors.sh"
. "${REPO_ROOT}/scripts/lib/hdf5.sh"
MODEL="${MODEL:-$DEFAULT_MODEL}"
SIMULATION="${SIMULATION:-$DEFAULT_SIMULATION}"
export MODEL SIMULATION
if [ -z "${MODEL}" ]; then
    echo "ERROR: no MODEL selected and no DEFAULT_MODEL in the Makefile" >&2
    exit 2
fi
MODEL_ROOT="models/${MODEL}"

summary_enabled() {
    [ "${TEST_SUMMARY:-0}" = "1" ]
}

# In summary mode, print only non-pass MIMIC_RESULT: lines from a captured log file.
print_markers() {
    local log_file=$1
    grep "^MIMIC_RESULT: \(FAIL\|SKIP\|WARN\|ERROR\)" "$log_file" || true
}

record_failed_test() {
    local test_name=$1

    FAILED_TEST_NAMES="${FAILED_TEST_NAMES} ${test_name}"

    if [ "${MIMIC_RECORD_TEST_FAILURES:-0}" = "1" ]; then
        mkdir -p "$(dirname "$TEST_FAILURES_FILE")"
        local failure="unit: ${test_name}"
        grep -qxF "$failure" "$TEST_FAILURES_FILE" 2>/dev/null || echo "$failure" >> "$TEST_FAILURES_FILE"
    fi
}

# Unit tests are always a test build: include the framework test fixture modules
# and their test-only properties (e.g. TestDummyProperty) in the generated code
# and module registry. scripts/discovery.py reads MIMIC_TEST_BUILD.
export MIMIC_TEST_BUILD=1

# Refresh generated metadata so direct single-test runs use current model properties/modules/tests
if ! python3 scripts/generate_properties.py > /dev/null; then
    echo -e "${RED}ERROR: Failed to refresh property code. Run 'make MODEL=<name> generate'${NC}"
    exit 2
fi

if ! python3 scripts/generate_module_registry.py > /dev/null; then
    echo -e "${RED}ERROR: Failed to refresh module registry. Run 'make generate'${NC}"
    exit 2
fi

if ! python3 scripts/generate_test_registry.py --strict > /dev/null; then
    echo -e "${RED}ERROR: Failed to refresh test registry. Run 'make generate'${NC}"
    exit 2
fi

if ! python3 scripts/generate_test_inputs.py > /dev/null; then
    echo -e "${RED}ERROR: Failed to refresh generated test inputs. Run 'make generate-test-inputs'${NC}"
    exit 2
fi

# Source directories
SRC_DIR="src"
TEST_DIR="tests/unit"
BUILD_DIR="tests/unit/build"

# Create build directory
mkdir -p "$BUILD_DIR"

# Create test output directories (gitignored, needed for unit tests)
mkdir -p "tests/data/output/binary"
mkdir -p "tests/data/output/hdf5"

# Generate git_version.h if it doesn't exist (needed by version.c)
GIT_VERSION_H="build/generated/git_version.h"
if [ ! -f "$GIT_VERSION_H" ]; then
    echo "Generating git version header for unit tests..."
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

# Common compiler flags
CC="${CC:-gcc}"
YAML_CFLAGS="$(pkg-config --cflags yaml-0.1 2>/dev/null || echo '')"
YAML_LDFLAGS="$(pkg-config --libs yaml-0.1 2>/dev/null || echo '-lyaml')"
CFLAGS="-Wall -Wextra -I. -I${SRC_DIR} -I${SRC_DIR}/include -I${SRC_DIR}/include/generated -I${SRC_DIR}/util -I${SRC_DIR}/core -I${SRC_DIR}/io -I${SRC_DIR}/module_system -Imodels -I${MODEL_ROOT} -Ibuild/generated -Itests -g -O0 -DMIMIC_COMPILED_MODEL=\"${MODEL}\" -DMIMIC_COMPILED_MODEL_PATH=\"${MODEL_ROOT}\" -DMIMIC_COMPILED_SIMULATION=\"${SIMULATION}\" ${YAML_CFLAGS}"
CFLAGS="${CFLAGS} -DMIMIC_TEST_BUILD"
LDFLAGS="-lm ${YAML_LDFLAGS}"

# HDF5 detection is shared with build_topology_dump.sh (scripts/lib/hdf5.sh).
detect_hdf5

if [ "$HDF5_AVAILABLE" = "1" ]; then
    LDFLAGS="${LDFLAGS} ${HDF5_LDFLAGS}"
fi

# Source files needed for tests (non-main files)
UTIL_SRCS="${SRC_DIR}/util/memory.c ${SRC_DIR}/util/error.c ${SRC_DIR}/util/numeric.c ${SRC_DIR}/util/version.c ${SRC_DIR}/util/integration.c ${SRC_DIR}/util/io.c ${SRC_DIR}/util/run_log.c ${SRC_DIR}/util/progress.c"
CORE_SRCS="${SRC_DIR}/core/allvars.c ${SRC_DIR}/core/read_parameter_file.c ${SRC_DIR}/core/init.c ${SRC_DIR}/core/tree_driver.c ${SRC_DIR}/core/virial.c ${SRC_DIR}/core/timestep.c ${SRC_DIR}/core/inheritance.c ${SRC_DIR}/core/output_buffer.c ${SRC_DIR}/core/galaxy_pool.c"
IO_SRCS="${SRC_DIR}/io/tree/interface.c ${SRC_DIR}/io/tree/binary.c ${SRC_DIR}/io/tree/registry.c ${SRC_DIR}/io/tree/chunk_plan.c ${SRC_DIR}/io/tree/read_ctrees_ascii.c ${SRC_DIR}/io/tree/ctrees/ctrees_utils.c ${SRC_DIR}/io/tree/ctrees/forest_utils.c ${SRC_DIR}/io/snapshot/interface.c ${SRC_DIR}/io/snapshot/registry.c ${SRC_DIR}/io/output/util.c ${SRC_DIR}/io/output/binary.c"
if [ "$HDF5_AVAILABLE" = "1" ]; then
    IO_SRCS="${IO_SRCS} ${SRC_DIR}/io/tree/hdf5.c ${SRC_DIR}/io/tree/read_ctrees_hdf5.c ${SRC_DIR}/io/snapshot/read_snapshot_hdf5.c ${SRC_DIR}/io/output/master_hdf5.c"
fi
TEST_STUBS="${TEST_DIR}/test_stubs.c"

# Module system sources - plain list generated by scripts/generate_module_registry.py
MODULE_SOURCES_LIST="tests/generated/module_sources.txt"
if [ -f "$MODULE_SOURCES_LIST" ]; then
    MODULE_SRCS=$(grep -v '^#' "$MODULE_SOURCES_LIST" | grep -v '^[[:space:]]*$' | tr '\n' ' ')
else
    echo -e "${RED}ERROR: Module sources not generated. Run 'make generate'${NC}"
    exit 2
fi

# Combine all necessary sources (excluding main.c)
ALL_SRCS="${UTIL_SRCS} ${CORE_SRCS} ${IO_SRCS} ${MODULE_SRCS} ${TEST_STUBS}"

###############################################################################
# Compile the shared sources once. Each test then compiles only its own file
# and links these objects — previously every test recompiled all ~30 sources,
# which dominated the unit tier's wall-clock time. Objects are rebuilt on every
# runner invocation so generated-code or MODEL changes can never go stale.
###############################################################################
OBJ_DIR="${BUILD_DIR}/obj"
mkdir -p "$OBJ_DIR"
SHARED_OBJS=""
SHARED_COMPILE_LOG="${BUILD_DIR}/shared_sources.compile.log"
: > "$SHARED_COMPILE_LOG"
if ! summary_enabled; then
    echo -e "${BLUE}Compiling shared sources once into ${OBJ_DIR}...${NC}"
fi
for src in $ALL_SRCS; do
    obj="${OBJ_DIR}/$(echo "${src%.c}" | tr '/' '_').o"
    SHARED_OBJS="$SHARED_OBJS $obj"
    src_cflags="$CFLAGS"
    if [ "$HDF5_AVAILABLE" = "1" ]; then
        case "$src" in
            "${SRC_DIR}/io/tree/registry.c"|\
            "${SRC_DIR}/io/tree/hdf5.c"|\
            "${SRC_DIR}/io/tree/read_ctrees_hdf5.c"|\
            "${SRC_DIR}/io/snapshot/registry.c"|\
            "${SRC_DIR}/io/snapshot/read_snapshot_hdf5.c"|\
            "${SRC_DIR}/io/output/master_hdf5.c"|\
            "${TEST_STUBS}")
                src_cflags="${src_cflags} -DHDF5 ${HDF5_CFLAGS}"
                ;;
        esac
    fi
    if ! $CC $src_cflags -c "$src" -o "$obj" >> "$SHARED_COMPILE_LOG" 2>&1; then
        echo "MIMIC_RESULT: ERROR shared-sources -- compilation failed for ${src}"
        cat "$SHARED_COMPILE_LOG"
        exit 1
    fi
done

# Test list (can be overridden by command line argument)
if [ $# -gt 0 ]; then
    # Specific test requested. "$*" not "$@": TESTS is a space-separated string
    # consumed by `for test in $TESTS`, so concatenation is the intent.
    TESTS="$*"
else
    # Auto-discover core, framework, and selected-model unit tests from registry.
    REGISTRY_TESTS=""
    MODULE_TEST_REGISTRY="${REPO_ROOT}/build/generated/unit_tests.txt"
    if [ -f "$MODULE_TEST_REGISTRY" ]; then
        # Read test paths and extract test names
        while IFS= read -r test_path; do
            # Skip comments and empty lines
            [[ "$test_path" =~ ^#.*$ ]] && continue
            [[ -z "$test_path" ]] && continue

            REGISTRY_TESTS="$REGISTRY_TESTS $test_path"
        done < "$MODULE_TEST_REGISTRY"
    fi

    TESTS="$REGISTRY_TESTS"
fi

if ! summary_enabled; then
    echo "Repository: $REPO_ROOT"
    echo "Compiler: $CC"
    echo "Build directory: $BUILD_DIR"
fi

###############################################################################
# Function: compile_and_run_test
# Compiles a test file and runs it, tracking results
###############################################################################
compile_and_run_test() {
    local test_arg=$1
    local test_name
    local test_file
    local test_display

    if [[ "$test_arg" == */* ]] || [[ "$test_arg" == *.c ]]; then
        test_display="$test_arg"
        test_file="$test_arg"
        [[ "$test_file" != /* ]] && test_file="${REPO_ROOT}/${test_file}"
        test_name=$(basename "$test_arg" .c)
    else
        test_name=$test_arg
        test_file="${TEST_DIR}/${test_name}.c"
        test_display="${test_name}.c"
    fi

    local test_exe="${BUILD_DIR}/${test_name}.test"

    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    # Check if test file exists in tests/unit/ (core tests)
    if [ ! -f "$test_file" ]; then
        # Try module directory (auto-discovered tests)
        # Look up full path from registry
        local registry_path
        registry_path=$(grep "/${test_name}.c$" "${REPO_ROOT}/build/generated/unit_tests.txt" 2>/dev/null | head -1)
        if [ -n "$registry_path" ]; then
            test_file="${REPO_ROOT}/${registry_path}"
        fi
    fi

    # Final check if test file exists
    if [ ! -f "$test_file" ]; then
        echo "MIMIC_RESULT: ERROR ${test_display} -- test file not found"
        echo -e "${RED}✗ Test file not found: ${test_name}.c${NC}"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        record_failed_test "$test_display"
        return 1
    fi
    test_display="${test_file#"${REPO_ROOT}"/}"

    # Add module directory to include path if this is a module test
    local module_include=""
    if [[ "$test_file" == *"/modules/"*"/_tests/"* ]] || [[ "$test_file" == *"/modules/"*"/tests/"* ]]; then
        # Extract module directory (parent of _tests/ or tests/)
        module_dir=$(dirname "$(dirname "$test_file")")
        module_include="-I${module_dir}"
    fi

    local test_cflags="$CFLAGS"
    local test_ldflags="$LDFLAGS"
    local extra_sources=""
    if [ "$test_name" = "test_ctrees_hdf5_reader" ] || \
       [ "$test_name" = "test_master_hdf5_partitions" ] || \
       [ "$test_name" = "test_unit_snapshot_reader_open" ]; then
        if [ "$HDF5_AVAILABLE" != "1" ]; then
            echo "MIMIC_RESULT: SKIP ${test_display} -- HDF5 development library not available"
            if ! summary_enabled; then
                echo -e "${YELLOW}– Skipping ${test_name}: HDF5 development library not available${NC}"
            fi
            SKIPPED_TESTS=$((SKIPPED_TESTS + 1))
            return 0
        fi
        test_cflags="${test_cflags} -DHDF5 ${HDF5_CFLAGS}"
    fi

    # Compile the test file and link the pre-built shared objects
    local compile_log="${BUILD_DIR}/${test_name}.compile.log"
    if summary_enabled; then
        if ! $CC $test_cflags $module_include $test_file $extra_sources $SHARED_OBJS -o $test_exe $test_ldflags > "$compile_log" 2>&1; then
            echo "MIMIC_RESULT: ERROR ${test_display} -- compilation failed"
            cat "$compile_log"
            COMPILE_ERRORS=$((COMPILE_ERRORS + 1))
            FAILED_TESTS=$((FAILED_TESTS + 1))
            record_failed_test "$test_display"
            return 2
        fi
    else
        echo -e "${BLUE}Compiling ${test_name}...${NC}"
        if ! $CC $test_cflags $module_include $test_file $extra_sources $SHARED_OBJS -o $test_exe $test_ldflags 2>&1 | tee "$compile_log"; then
            echo -e "${RED}✗ Compilation failed for ${test_name}${NC}"
            echo "  See ${compile_log} for details"
            COMPILE_ERRORS=$((COMPILE_ERRORS + 1))
            FAILED_TESTS=$((FAILED_TESTS + 1))
            record_failed_test "$test_display"
            return 2
        fi
    fi

    # Run test
    if ! summary_enabled; then
        echo -e "${BLUE}Running test: ${test_name}${NC}"
    fi

    local run_log="${BUILD_DIR}/${test_name}.run.log"
    if summary_enabled; then
        if $test_exe > "$run_log" 2>&1; then
            print_markers "$run_log"
            PASSED_TESTS=$((PASSED_TESTS + 1))
            return 0
        else
            # On failure: show markers if any were emitted; otherwise show full log
            # (the full log path handles crashes/signals that emit no markers at all)
            if grep -q "^MIMIC_RESULT:" "$run_log"; then
                print_markers "$run_log"
            else
                cat "$run_log"
            fi
            FAILED_TESTS=$((FAILED_TESTS + 1))
            record_failed_test "$test_display"
            return 1
        fi
    elif $test_exe; then
        echo -e "${GREEN}✓ ${test_name} PASSED${NC}"
        PASSED_TESTS=$((PASSED_TESTS + 1))
        return 0
    else
        echo -e "${RED}✗ ${test_name} FAILED${NC}"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        record_failed_test "$test_display"
        return 1
    fi
}

###############################################################################
# Main execution
###############################################################################

# Run all specified tests
for test in $TESTS; do
    if ! summary_enabled; then
        echo ""
    fi
    compile_and_run_test "$test"
done

# Restore production-mode generated source files. Unit tests build with
# MIMIC_TEST_BUILD set (fixtures + TestDummyProperty merged in); leaving the
# shared generated files in that state would make a later production
# `make check-generated` report a false mismatch. Regenerate without the flag.
unset MIMIC_TEST_BUILD
python3 scripts/generate_properties.py > /dev/null 2>&1 || true
python3 scripts/generate_module_registry.py > /dev/null 2>&1 || true

# Print summary
if summary_enabled; then
    echo -n "Unit Test Summary: total=${TOTAL_TESTS} passed="
    echo -en "${GREEN}${PASSED_TESTS}${NC}"
    echo -n " failed="
    if [ $FAILED_TESTS -gt 0 ]; then
        echo -en "${RED}${FAILED_TESTS}${NC}"
    else
        echo -n "$FAILED_TESTS"
    fi
    if [ $COMPILE_ERRORS -gt 0 ]; then
        echo -n " compile_errors="
        echo -en "${YELLOW}${COMPILE_ERRORS}${NC}"
    fi
    if [ $SKIPPED_TESTS -gt 0 ]; then
        echo -n " skipped="
        echo -en "${YELLOW}${SKIPPED_TESTS}${NC}"
    fi
    echo ""
else
    echo ""
    echo -e "${BLUE}============================================================${NC}"
    echo -e "${BLUE}Unit Test Summary${NC}"
    echo -e "${BLUE}============================================================${NC}"
    echo "Total tests:    $TOTAL_TESTS"
    echo -e "Passed:         ${GREEN}$PASSED_TESTS${NC}"
    if [ $FAILED_TESTS -gt 0 ]; then
        echo -e "Failed:         ${RED}$FAILED_TESTS${NC}"
    else
        echo "Failed:         $FAILED_TESTS"
    fi
    if [ $COMPILE_ERRORS -gt 0 ]; then
        echo -e "Compile errors: ${YELLOW}$COMPILE_ERRORS${NC}"
    fi
    if [ $SKIPPED_TESTS -gt 0 ]; then
        echo -e "Skipped:        ${YELLOW}$SKIPPED_TESTS${NC}"
    fi
    if [ $FAILED_TESTS -gt 0 ]; then
        echo -e "${RED}Failed tests:${NC}"
        for test_name in $FAILED_TEST_NAMES; do
            echo "  - $test_name"
        done
    fi
    echo -e "${BLUE}============================================================${NC}"
    echo ""
fi

# Final result
if [ $FAILED_TESTS -eq 0 ] && [ $COMPILE_ERRORS -eq 0 ]; then
    echo -e "${GREEN}=== TLDR: ALL UNIT TESTS PASSED ===${NC}"
    echo ""
    exit 0
else
    echo -e "${RED}=== TLDR: UNIT TESTS FAILED ===${NC}"
    echo ""
    exit 1
fi
