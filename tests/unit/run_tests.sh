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
#   1 - One or more tests failed
#   2 - Compilation error
#
# Author: Mimic Testing Team
# Date: 2025-11-08
# Phase: Phase 2 (Testing Framework)
###############################################################################

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Counters
TOTAL_TESTS=0
PASSED_TESTS=0
FAILED_TESTS=0
COMPILE_ERRORS=0

# Get repository root (two levels up from tests/unit/)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$REPO_ROOT" || exit 1

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
GIT_VERSION_H="${SRC_DIR}/include/git_version.h"
if [ ! -f "$GIT_VERSION_H" ]; then
    echo "Generating git version header for unit tests..."
    mkdir -p "$(dirname "$GIT_VERSION_H")"
    cat > "$GIT_VERSION_H" << 'EOF'
#ifndef GIT_VERSION_H
#define GIT_VERSION_H
#define GIT_COMMIT "unknown"
#define GIT_BRANCH "unknown"
#define GIT_DATE "unknown"
#endif
EOF
    # Try to get actual git info if available
    if command -v git &> /dev/null && [ -d .git ]; then
        {
            echo "#ifndef GIT_VERSION_H"
            echo "#define GIT_VERSION_H"
            echo "#define GIT_COMMIT \"$(git rev-parse HEAD 2>/dev/null || echo 'unknown')\""
            echo "#define GIT_BRANCH \"$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')\""
            echo "#define GIT_DATE \"$(git log -1 --format=%cd --date=short 2>/dev/null || echo 'unknown')\""
            echo "#endif"
        } > "$GIT_VERSION_H"
    fi
fi

# Common compiler flags
CC="${CC:-gcc}"
CFLAGS="-Wall -Wextra -I${SRC_DIR}/include -I${SRC_DIR}/include/generated -I${SRC_DIR}/util -I${SRC_DIR}/core -I${SRC_DIR}/io -g -O0"
LDFLAGS="-lm"

# Source files needed for tests (non-main files)
UTIL_SRCS="${SRC_DIR}/util/memory.c ${SRC_DIR}/util/error.c ${SRC_DIR}/util/numeric.c ${SRC_DIR}/util/version.c ${SRC_DIR}/util/parameters.c ${SRC_DIR}/util/integration.c ${SRC_DIR}/util/io.c"
CORE_SRCS="${SRC_DIR}/core/allvars.c ${SRC_DIR}/core/read_parameter_file.c ${SRC_DIR}/core/init.c"
IO_SRCS="${SRC_DIR}/io/tree/interface.c ${SRC_DIR}/io/tree/binary.c ${SRC_DIR}/io/output/util.c ${SRC_DIR}/io/util.c"
TEST_STUBS="${TEST_DIR}/test_stubs.c"

# Module system sources (Phase 3 + Phase 4.2)
MODULE_SRCS="${SRC_DIR}/core/module_registry.c ${SRC_DIR}/modules/sage_infall/sage_infall.c ${SRC_DIR}/modules/simple_cooling/simple_cooling.c ${SRC_DIR}/modules/simple_sfr/simple_sfr.c ${SRC_DIR}/modules/module_init.c"

# Combine all necessary sources (excluding main.c)
ALL_SRCS="${UTIL_SRCS} ${CORE_SRCS} ${IO_SRCS} ${MODULE_SRCS} ${TEST_STUBS}"

# Test list (can be overridden by command line argument)
if [ $# -gt 0 ]; then
    # Specific test requested
    TESTS="$@"
else
    # Run all tests
    TESTS="test_memory_system test_property_metadata test_parameter_parsing test_tree_loading test_numeric_utilities test_module_configuration"
fi

echo "Repository: $REPO_ROOT"
echo "Compiler: $CC"
echo "Build directory: $BUILD_DIR"

###############################################################################
# Function: compile_and_run_test
# Compiles a test file and runs it, tracking results
###############################################################################
compile_and_run_test() {
    local test_name=$1
    local test_file="${TEST_DIR}/${test_name}.c"
    local test_exe="${BUILD_DIR}/${test_name}.test"

    TOTAL_TESTS=$((TOTAL_TESTS + 1))

    # Check if test file exists
    if [ ! -f "$test_file" ]; then
        echo -e "${RED}✗ Test file not found: ${test_file}${NC}"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        return 1
    fi

    # Compile test
    echo -e "${BLUE}Compiling ${test_name}...${NC}"
    if ! $CC $CFLAGS $test_file $ALL_SRCS -o $test_exe $LDFLAGS 2>&1 | tee "${BUILD_DIR}/${test_name}.compile.log"; then
        echo -e "${RED}✗ Compilation failed for ${test_name}${NC}"
        echo "  See ${BUILD_DIR}/${test_name}.compile.log for details"
        COMPILE_ERRORS=$((COMPILE_ERRORS + 1))
        FAILED_TESTS=$((FAILED_TESTS + 1))
        return 2
    fi

    # Run test
    echo -e "${BLUE}Running test: ${test_name}${NC}"
    echo "------------------------------------------------------------"

    if $test_exe; then
        echo "------------------------------------------------------------"
        echo -e "${GREEN}✓ ${test_name} PASSED${NC}"
        PASSED_TESTS=$((PASSED_TESTS + 1))
        return 0
    else
        echo "------------------------------------------------------------"
        echo -e "${RED}✗ ${test_name} FAILED${NC}"
        FAILED_TESTS=$((FAILED_TESTS + 1))
        return 1
    fi
}

###############################################################################
# Main execution
###############################################################################

# Run all specified tests
for test in $TESTS; do
    echo ""
    compile_and_run_test "$test"
done

# Print summary
echo ""
echo "============================================================"
echo "Unit Test Summary"
echo "============================================================"
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
echo "============================================================"

# Final result
if [ $FAILED_TESTS -eq 0 ] && [ $COMPILE_ERRORS -eq 0 ]; then
    echo -e "${GREEN}✓ All unit tests passed!${NC}"
    exit 0
else
    echo -e "${RED}✗ Some tests failed${NC}"
    exit 1
fi
