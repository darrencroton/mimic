#!/bin/bash
#
# benchmark_mimic.sh - Performance benchmark script for Mimic
#
# This script provides comprehensive performance benchmarking for the Mimic
# galaxy formation framework. It measures runtime, memory usage, and other
# performance metrics to help developers track performance changes over time.
#
# USAGE:
#   ./scripts/benchmark_mimic.sh                              # Run with default settings
#   ./scripts/benchmark_mimic.sh --verbose                    # Run with detailed output
#   ./scripts/benchmark_mimic.sh --help                       # Show help information
#   ./scripts/benchmark_mimic.sh --param-file custom.yaml     # Explicit parameter file
#   ./scripts/benchmark_mimic.sh custom.yaml                  # Same, positional shorthand
#   ./scripts/benchmark_mimic.sh --compress                   # Benchmark HDF5 compression overhead
#   EXTRA_CFLAGS="-O3 -march=native" ./scripts/benchmark_mimic.sh  # Release-optimised build
#
# REQUIREMENTS:
#   - Can be run from any directory
#   - GNU Make must be available
#   - Parameter file must exist (default: models/sage16/input/sage16_mini-millennium.yaml)
#
# OUTPUT:
#   Results are stored in JSON format in the benchmarks/ directory
#   with timestamp-based filenames for easy comparison across runs.
#
# ENVIRONMENT VARIABLES:
#   MIMIC_EXECUTABLE  - Override mimic executable location
#   MPI_RUN_COMMAND   - Run with MPI (e.g., "mpirun -np 4")
#   MAKE_FLAGS        - Additional make flags (e.g., "USE-HDF5=no USE-MPI=yes")
#   MIMIC_FLAGS       - Additional runtime flags passed to the mimic executable
#                       (e.g., "--compress"). Use --compress shorthand for convenience.
#   EXTRA_CFLAGS      - Additional compiler flags for the build step only
#                       (e.g., "-O3 -march=native"). Not for production builds.
#
# EXAMPLES:
#   # Basic benchmark (uses default models/sage16/input/sage16_mini-millennium.yaml)
#   ./scripts/benchmark_mimic.sh
#
#   # Benchmark with custom parameter file
#   make MODEL=sage16 SIMULATION=mini-millennium generate-test-inputs
#   ./scripts/benchmark_mimic.sh --param-file build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml
#   ./scripts/benchmark_mimic.sh build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml
#
#   # Benchmark with MPI
#   MPI_RUN_COMMAND="mpirun -np 4" MAKE_FLAGS="USE-MPI=yes" ./scripts/benchmark_mimic.sh
#
#   # Binary-only benchmark (opt out of HDF5)
#   MAKE_FLAGS="USE-HDF5=no" ./scripts/benchmark_mimic.sh
#
#   # HDF5 compression benchmark (measures CPU/time/disk trade-off)
#   ./scripts/benchmark_mimic.sh --compress
#   MIMIC_FLAGS="--compress" ./scripts/benchmark_mimic.sh
#
#   # Release-optimised build benchmark
#   EXTRA_CFLAGS="-O3 -march=native" ./scripts/benchmark_mimic.sh
#
#   # Compare two benchmark runs
#   diff benchmarks/baseline_20250101_120000.json benchmarks/baseline_20250102_120000.json
#

set -e  # Exit on any error

# Configuration
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
BENCHMARK_RESULTS="baseline_${TIMESTAMP}.json"
VERBOSE=0
SHOW_HELP=0
PARAM_FILE=""  # Will be set to default later if not specified
RUN_PARAM_FILE=""
TEMP_PARAM_DIR=""
OUTPUT_MARKER=""
MIMIC_FLAGS="${MIMIC_FLAGS:-}"  # Runtime flags passed to the mimic executable

# Process command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        --help)
            SHOW_HELP=1
            shift
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        --param-file)
            shift
            PARAM_FILE="$1"
            shift
            ;;
        --param-file=*)
            PARAM_FILE="${1#*=}"
            shift
            ;;
        --compress)
            MIMIC_FLAGS="${MIMIC_FLAGS} --compress"
            shift
            ;;
        -*)
            echo "Unknown option: $1"
            echo "Usage: $0 [--help] [--verbose] [--compress] [--param-file FILE] [PARAM_FILE]"
            exit 1
            ;;
        *)
            # Positional argument - treat as parameter file
            PARAM_FILE="$1"
            shift
            ;;
    esac
done

# Determine script and root directories - can run from anywhere
SCRIPT_PATH="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_PATH")"

# Load shared project defaults (DEFAULT_MODEL, DEFAULT_SIMULATION, make_default)
# shellcheck source=scripts/lib/defaults.sh
. "${SCRIPT_PATH}/lib/defaults.sh"

# Function to display error and exit
error_exit() {
    echo "ERROR: $1"
    echo "Benchmark failed."
    exit 1
}

cleanup_temp_files() {
    if [[ -n "${TEMP_PARAM_DIR}" && -d "${TEMP_PARAM_DIR}" ]]; then
        rm -rf "${TEMP_PARAM_DIR}"
    fi
    if [[ -n "${OUTPUT_MARKER}" && -f "${OUTPUT_MARKER}" ]]; then
        rm -f "${OUTPUT_MARKER}"
    fi
}

trap cleanup_temp_files EXIT

# Function to log verbose output
verbose_log() {
    if [ $VERBOSE -eq 1 ]; then
        echo "[VERBOSE] $1"
    fi
}

# Evaluate an arithmetic expression via awk; optional second arg is a printf format.
calc() {
    local expr="$1" fmt="${2:-}"
    if [[ -n "$fmt" ]]; then
        awk "BEGIN {printf \"${fmt}\n\", ${expr}}"
    else
        awk "BEGIN {print ${expr}}"
    fi
}

# Set default parameter file if not specified
if [[ -z "$PARAM_FILE" ]]; then
    PARAM_FILE="${ROOT_DIR}/models/${DEFAULT_MODEL}/input/${DEFAULT_MODEL}_${DEFAULT_SIMULATION}.yaml"
fi

# Show help if requested
if [ $SHOW_HELP -eq 1 ]; then
    cat << 'EOF'
Usage: ./scripts/benchmark_mimic.sh [OPTIONS] [PARAM_FILE]

OPTIONS:
  --help                Show this help message
  --verbose             Run with detailed output and timing information
  --compress            Enable HDF5 gzip compression (measures CPU/time/disk trade-off;
                        only meaningful with hdf5 output format)
  --param-file FILE     Parameter file to use for benchmarking

ARGUMENTS:
  PARAM_FILE            Parameter file to benchmark (default: models/sage16/input/sage16_mini-millennium.yaml)
                        Can be specified as positional argument or with --param-file
                        Supports both absolute and relative paths

PURPOSE:
  This script establishes performance baselines for the Mimic galaxy formation
  framework. It captures comprehensive metrics including:
  - Wall clock execution time
  - Maximum memory usage (RSS)
  - System information and git version
  - Test case configuration

  Results are stored in timestamped JSON files for easy comparison
  between different versions, configurations, or optimizations.

OUTPUT:
  Results are stored in benchmarks/ directory with the format:
  baseline_YYYYMMDD_HHMMSS.json

  JSON structure includes:
  - timestamp: ISO 8601 timestamp
  - git: Git repository information (commit, branch, dirty status)
  - system: Host hardware and OS details
  - test_case: Test configuration used
  - overall_performance: Runtime and memory metrics
  - configuration: Build and runtime configuration

ENVIRONMENT VARIABLES:
  MIMIC_FLAGS           Runtime flags passed to the mimic executable
                        (e.g., MIMIC_FLAGS="--compress")
  EXTRA_CFLAGS          Additional compiler flags for benchmarking/profiling builds only
                        (e.g., EXTRA_CFLAGS="-O3 -march=native")
  MAKE_FLAGS            Additional make flags (e.g., "USE-HDF5=no USE-MPI=yes")
  MPI_RUN_COMMAND       MPI launch command (e.g., "mpirun -np 4")
  MIMIC_EXECUTABLE      Override mimic executable path

EXAMPLES:
  # Basic benchmark (uses default models/sage16/input/sage16_mini-millennium.yaml)
  # Can run from anywhere; build selectors come from the run file, environment,
  # or Makefile defaults, in that order:
  ./scripts/benchmark_mimic.sh
  cd scripts && ./benchmark_mimic.sh

  # Benchmark with custom parameter file from the repository root
  make MODEL=sage16 SIMULATION=mini-millennium generate-test-inputs
  ./scripts/benchmark_mimic.sh --param-file build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml
  ./scripts/benchmark_mimic.sh build/generated/test_inputs/sage16/mini-millennium/core/test_binary.yaml

  # Verbose output
  ./scripts/benchmark_mimic.sh --verbose

  # MPI benchmark
  MPI_RUN_COMMAND="mpirun -np 4" MAKE_FLAGS="USE-MPI=yes" ./scripts/benchmark_mimic.sh

  # Binary output only benchmark (opt out of HDF5)
  MAKE_FLAGS="USE-HDF5=no" ./scripts/benchmark_mimic.sh

  # HDF5 compression benchmark (measures CPU/time/disk trade-off)
  ./scripts/benchmark_mimic.sh --compress
  MIMIC_FLAGS="--compress" ./scripts/benchmark_mimic.sh

  # Release-optimised build benchmark
  EXTRA_CFLAGS="-O3 -march=native" ./scripts/benchmark_mimic.sh

COMPARING RESULTS:
  # Simple diff
  diff benchmarks/baseline_old.json benchmarks/baseline_new.json

  # Extract key metrics
  grep "wall_time\|max_memory" benchmarks/baseline_*.json

  # Plot trends over time (requires custom analysis script)
  python plot_benchmark_trends.py benchmarks/

TROUBLESHOOTING:
  - Script can be run from any directory
  - Check that GNU Make and build tools are installed
  - Verify parameter file exists and contains valid settings
  - For best accuracy, ensure /usr/bin/time is available
  - For MPI issues, verify MPI installation and mpirun availability
EOF
    exit 0
fi

verbose_log "Script location: ${SCRIPT_PATH}"
verbose_log "Root directory: ${ROOT_DIR}"

# Create benchmark directory if it doesn't exist
mkdir -p "${ROOT_DIR}/benchmarks"

# Validate parameter file exists and resolve path
if [[ "$PARAM_FILE" = /* ]]; then
    # Absolute path
    if [[ ! -f "${PARAM_FILE}" ]]; then
        error_exit "Parameter file not found: ${PARAM_FILE}"
    fi
else
    # Relative path - try as-is first, then relative to ROOT_DIR
    if [[ -f "${PARAM_FILE}" ]]; then
        # Resolve to absolute path
        PARAM_FILE="$(cd "$(dirname "${PARAM_FILE}")" && pwd)/$(basename "${PARAM_FILE}")"
    elif [[ -f "${ROOT_DIR}/${PARAM_FILE}" ]]; then
        PARAM_FILE="${ROOT_DIR}/${PARAM_FILE}"
    else
        error_exit "Parameter file not found: ${PARAM_FILE} (also tried ${ROOT_DIR}/${PARAM_FILE})"
    fi
fi

verbose_log "Using parameter file: ${PARAM_FILE}"

# Parse benchmark-relevant settings from the run YAML using the same precedence
# as read_parameter_file.c: simulation config defaults first, then run-file
# sections override them.
CONFIG_VALUES=$(PARAM_FILE="${PARAM_FILE}" ROOT_DIR="${ROOT_DIR}" python3 - << 'PY'
import os
import sys
import yaml
from pathlib import Path

param_file = Path(os.environ["PARAM_FILE"])
repo_root = Path(os.environ["ROOT_DIR"])


def resolve_repo_path(path_value):
    path = Path(path_value)
    if path.is_absolute():
        return path

    repo_candidate = repo_root / path
    if repo_candidate.is_file():
        return repo_candidate

    param_candidate = param_file.parent / path
    if param_candidate.is_file():
        return param_candidate

    return repo_candidate


try:
    with open(param_file, 'r') as f:
        config = yaml.safe_load(f) or {}

    output_dir = config.get('output', {}).get('output_directory', './output/')
    output_format = config.get('output', {}).get('output_format', 'binary')
    output_basename = config.get('output', {}).get('output_filename', 'model')
    input_config = {}

    simulation_section = config.get('simulation', {}) or {}
    simulation_package = simulation_section.get('name', '')
    simulation_config_path = simulation_section.get('config')
    if not simulation_config_path and simulation_package:
        simulation_config_path = f"simulations/{simulation_package}/simulation_info.yaml"
    if simulation_config_path:
        simulation_config_path = resolve_repo_path(simulation_config_path)
        with open(simulation_config_path, 'r') as f:
            simulation_config = yaml.safe_load(f) or {}
        input_config.update(simulation_config.get('input', {}))

    input_config.update(config.get('input', {}))

    tree_type = input_config.get('tree_type', 'lhalo_binary')
    model_name = config.get('model', {}).get('name', '')

    # Remove trailing slash from output_dir
    output_dir = output_dir.rstrip('/')

    print(output_dir)
    print(output_format)
    print(output_basename)
    print(tree_type)
    print(model_name)
    print(simulation_package)
except Exception as e:
    print(f'ERROR: Failed to parse YAML: {e}', file=sys.stderr)
    sys.exit(1)
PY
) || error_exit "Failed to parse parameter file"

CONFIG_LINE=0
while IFS= read -r line; do
    case $CONFIG_LINE in
        0) OUTPUT_DIR="$line" ;;
        1) OUTPUT_FORMAT="$line" ;;
        2) OUTPUT_BASENAME="$line" ;;
        3) TREE_TYPE="$line" ;;
        4) CONFIG_MODEL="$line" ;;
        5) CONFIG_SIMULATION="$line" ;;
    esac
    CONFIG_LINE=$((CONFIG_LINE + 1))
done <<< "$CONFIG_VALUES"

# Resolve OUTPUT_DIR if it's a relative path
if [[ "$OUTPUT_DIR" != /* ]]; then
    # Relative path - resolve relative to ROOT_DIR
    OUTPUT_DIR="${ROOT_DIR}/${OUTPUT_DIR}"
fi

SELECTED_MODEL="${MODEL:-${CONFIG_MODEL:-${DEFAULT_MODEL}}}"
SELECTED_SIMULATION="${SIMULATION:-${SIM:-${CONFIG_SIMULATION:-${DEFAULT_SIMULATION}}}}"

if [[ -z "${SELECTED_MODEL}" ]]; then
    error_exit "Could not determine MODEL from environment, run file, or Makefile default"
fi

if [[ -z "${SELECTED_SIMULATION}" ]]; then
    error_exit "Could not determine SIMULATION from environment, run file, or Makefile default"
fi

if [[ -n "${CONFIG_MODEL}" && "${SELECTED_MODEL}" != "${CONFIG_MODEL}" ]]; then
    error_exit "Run file selects model.name='${CONFIG_MODEL}' but benchmark would build MODEL=${SELECTED_MODEL}"
fi

if [[ -n "${CONFIG_SIMULATION}" && "${SELECTED_SIMULATION}" != "${CONFIG_SIMULATION}" ]]; then
    error_exit "Run file selects simulation.name='${CONFIG_SIMULATION}' but benchmark would build SIMULATION=${SELECTED_SIMULATION}"
fi

RUN_PARAM_FILE="${PARAM_FILE}"
case "${PARAM_FILE}" in
    "${ROOT_DIR}/build/"*)
        TEMP_PARAM_DIR=$(mktemp -d "${TMPDIR:-/tmp}/mimic-benchmark-param.XXXXXX")
        RUN_PARAM_FILE="${TEMP_PARAM_DIR}/$(basename "${PARAM_FILE}")"
        cp "${PARAM_FILE}" "${RUN_PARAM_FILE}" || error_exit "Failed to preserve generated parameter file before clean build"
        verbose_log "Copied generated parameter file to ${RUN_PARAM_FILE} for clean build"
        ;;
esac

# CRITICAL SAFETY CHECK: Ensure variables are not empty
if [[ -z "${OUTPUT_DIR}" ]] || [[ -z "${OUTPUT_BASENAME}" ]]; then
    error_exit "CRITICAL: Failed to parse parameter file. OUTPUT_DIR='${OUTPUT_DIR}' OUTPUT_BASENAME='${OUTPUT_BASENAME}'. Aborting to prevent accidental deletion."
fi

verbose_log "Detected OutputDir: ${OUTPUT_DIR}"
verbose_log "Detected OutputFormat: ${OUTPUT_FORMAT}"
verbose_log "Detected OutputFileBaseName: ${OUTPUT_BASENAME}"
verbose_log "Detected TreeType: ${TREE_TYPE}"
verbose_log "Selected MODEL: ${SELECTED_MODEL}"
verbose_log "Selected SIMULATION: ${SELECTED_SIMULATION}"

# Auto-detect if HDF5 support is needed and add to MAKE_FLAGS
if [[ "$OUTPUT_FORMAT" == "hdf5" ]] || [[ "$TREE_TYPE" == *"hdf5"* ]]; then
    if [[ "$MAKE_FLAGS" =~ USE-HDF5=no ]]; then
        error_exit "HDF5 output/tree requested but MAKE_FLAGS disables HDF5 (USE-HDF5=no). Remove the opt-out."
    elif [[ "$MAKE_FLAGS" =~ USE-HDF5=yes ]]; then
        verbose_log "HDF5 required; using explicit USE-HDF5=yes from MAKE_FLAGS"
    else
        verbose_log "HDF5 required; relying on default HDF5-enabled build"
    fi
fi

# Warn if --compress was requested but output is not HDF5 (mimic silently ignores it)
if [[ "${MIMIC_FLAGS}" == *"--compress"* ]] && [[ "$OUTPUT_FORMAT" != "hdf5" ]]; then
    echo "WARNING: --compress has no effect with output_format='${OUTPUT_FORMAT}' (HDF5 only). Continuing without compression."
    MIMIC_FLAGS="${MIMIC_FLAGS/--compress/}"
    MIMIC_FLAGS="${MIMIC_FLAGS# }"  # trim leading space
fi

# Set file search pattern based on output format
if [[ "$OUTPUT_FORMAT" == "hdf5" ]]; then
    # HDF5 format uses: model_000.hdf5, model_001.hdf5, model.hdf5
    FILE_PATTERN="${OUTPUT_BASENAME}_*.hdf5"
else
    # Binary format uses: model_z2.070_0, model_z2.070_1, etc.
    FILE_PATTERN="${OUTPUT_BASENAME}_z*"
fi

verbose_log "Using file pattern: ${FILE_PATTERN}"

echo "=== Mimic Performance Benchmark ==="
echo "Timestamp: $(date)"
echo "Parameter file: ${PARAM_FILE}"
if [[ "${RUN_PARAM_FILE}" != "${PARAM_FILE}" ]]; then
    echo "Runtime parameter file: ${RUN_PARAM_FILE}"
fi
echo "Output format: ${OUTPUT_FORMAT}"
echo "Model set: ${SELECTED_MODEL}"
echo "Simulation package: ${SELECTED_SIMULATION}"
echo "Saving results to: ${ROOT_DIR}/benchmarks/${BENCHMARK_RESULTS}"
echo

# Build Mimic using Make
echo "Building Mimic using Make..."
cd "${ROOT_DIR}" || error_exit "Could not change to Mimic root directory"

# Clean and build
verbose_log "Cleaning previous build..."
make clean > /dev/null 2>&1 || true

verbose_log "Building Mimic with flags: ${MAKE_FLAGS}${EXTRA_CFLAGS:+ EXTRA_CFLAGS=${EXTRA_CFLAGS}}"
make MODEL="${SELECTED_MODEL}" SIMULATION="${SELECTED_SIMULATION}" -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1) ${MAKE_FLAGS} ${EXTRA_CFLAGS:+EXTRA_CFLAGS="${EXTRA_CFLAGS}"} || error_exit "Build failed"

echo "Build successful."
echo

# Verify executable exists
MIMIC_EXECUTABLE="${MIMIC_EXECUTABLE:-${ROOT_DIR}/mimic}"
if [ ! -f "$MIMIC_EXECUTABLE" ]; then
    error_exit "Mimic executable not found at $MIMIC_EXECUTABLE"
fi

verbose_log "Using Mimic executable: $MIMIC_EXECUTABLE"

# Prepare benchmark run
echo "Preparing benchmark run..."

OUTPUT_MARKER=$(mktemp)
if [[ -d "${OUTPUT_DIR}" ]]; then
    verbose_log "Existing output files in ${OUTPUT_DIR} may be overwritten by Mimic"
fi

if [ $VERBOSE -eq 1 ]; then
    echo "Parameter file settings:"
    echo "  OutputDir: ${OUTPUT_DIR}"
    echo "  OutputFormat: ${OUTPUT_FORMAT}"
    echo "  OutputBaseName: ${OUTPUT_BASENAME}"
    echo "  TreeType: ${TREE_TYPE}"
fi

# ======= Time and memory measurement =======
echo "Running Mimic benchmark..."
cd "${ROOT_DIR}" || error_exit "Could not change to root directory"

# Get number of MPI processes for later use
NUM_MIMIC_PROCS=$(echo ${MPI_RUN_COMMAND} | awk '{print $NF}')
if [[ -z "${NUM_MIMIC_PROCS}" ]]; then
   NUM_MIMIC_PROCS=1
fi

verbose_log "Running with $NUM_MIMIC_PROCS processes"

# Suppress mimic's INFO log output during the timed run to reduce I/O noise;
# use --verbose when the benchmark is run with --verbose so diagnostic output
# is still available.
VERBOSITY_FLAG="--quiet"
if [ $VERBOSE -eq 1 ]; then
    VERBOSITY_FLAG="--verbose"
fi

# Build the full argument list for the mimic invocation
MIMIC_ARGS="${VERBOSITY_FLAG} ${MIMIC_FLAGS} ${RUN_PARAM_FILE}"

verbose_log "Mimic runtime flags: ${VERBOSITY_FLAG} ${MIMIC_FLAGS}"

# Platform-specific timing and memory measurement
RUN_OUTPUT=$(mktemp)
TIME_OUTPUT=$(mktemp)

set +e

# Check if /usr/bin/time is available
if [ -x "/usr/bin/time" ]; then
    verbose_log "Using /usr/bin/time for accurate measurement"

    # Platform-specific time command
    if [ "$(uname)" = "Darwin" ]; then
        # macOS version
        verbose_log "Detected macOS"
        if [[ -n "${MPI_RUN_COMMAND}" ]]; then
            # shellcheck disable=SC2086
            /usr/bin/time -l ${MPI_RUN_COMMAND} "${MIMIC_EXECUTABLE}" ${MIMIC_ARGS} > "${RUN_OUTPUT}" 2> "${TIME_OUTPUT}"
            RUN_STATUS=$?
        else
            # shellcheck disable=SC2086
            /usr/bin/time -l "${MIMIC_EXECUTABLE}" ${MIMIC_ARGS} > "${RUN_OUTPUT}" 2> "${TIME_OUTPUT}"
            RUN_STATUS=$?
        fi

        # Extract wall clock time (user + system)
        USER_TIME=$(grep "user" "${TIME_OUTPUT}" | awk '{print $1}')
        SYS_TIME=$(grep "sys" "${TIME_OUTPUT}" | awk '{print $1}')
        REAL_TIME=$(calc "$USER_TIME + $SYS_TIME")

        # Memory usage (convert bytes to MB)
        MAX_MEMORY=$(grep "maximum resident set size" "${TIME_OUTPUT}" | awk '{print $1}')
        MAX_MEMORY=$(calc "${MAX_MEMORY:-0} / 1048576" "%.2f")
    else
        # Linux version
        verbose_log "Detected Linux"
        if [[ -n "${MPI_RUN_COMMAND}" ]]; then
            # shellcheck disable=SC2086
            /usr/bin/time -v ${MPI_RUN_COMMAND} "${MIMIC_EXECUTABLE}" ${MIMIC_ARGS} > "${RUN_OUTPUT}" 2> "${TIME_OUTPUT}"
            RUN_STATUS=$?
        else
            # shellcheck disable=SC2086
            /usr/bin/time -v "${MIMIC_EXECUTABLE}" ${MIMIC_ARGS} > "${RUN_OUTPUT}" 2> "${TIME_OUTPUT}"
            RUN_STATUS=$?
        fi

        # Extract wall clock time from format "h:mm:ss or m:ss.ss"
        TIME_STR=$(grep "Elapsed (wall clock) time" "${TIME_OUTPUT}" | sed 's/.*: //')
        # Convert to seconds
        if [[ $TIME_STR == *:*:* ]]; then
            # Format h:mm:ss
            REAL_TIME=$(echo "$TIME_STR" | awk -F: '{print ($1 * 3600) + ($2 * 60) + $3}')
        else
            # Format m:ss.ss
            REAL_TIME=$(echo "$TIME_STR" | awk -F: '{print ($1 * 60) + $2}')
        fi

        # Memory usage (convert KB to MB)
        MAX_MEMORY=$(grep "Maximum resident set size" "${TIME_OUTPUT}" | awk '{print $NF}')
        MAX_MEMORY=$(calc "${MAX_MEMORY:-0} / 1024" "%.2f")
    fi

    if [ $VERBOSE -eq 1 ]; then
        echo "Time measurement output:"
        cat "${TIME_OUTPUT}"
    fi
else
    # Fallback: simple timing without /usr/bin/time
    verbose_log "Using fallback time measurement (no /usr/bin/time)"
    start_time=$(date +%s)

    if [[ -n "${MPI_RUN_COMMAND}" ]]; then
        # shellcheck disable=SC2086
        ${MPI_RUN_COMMAND} "${MIMIC_EXECUTABLE}" ${MIMIC_ARGS} > "${RUN_OUTPUT}" 2>&1
        RUN_STATUS=$?
    else
        # shellcheck disable=SC2086
        "${MIMIC_EXECUTABLE}" ${MIMIC_ARGS} > "${RUN_OUTPUT}" 2>&1
        RUN_STATUS=$?
    fi

    end_time=$(date +%s)
    REAL_TIME=$((end_time - start_time))
    MAX_MEMORY="N/A"
    verbose_log "Memory measurement not available without /usr/bin/time"
fi

set -e

# Check if run was successful
if [ $RUN_STATUS -ne 0 ]; then
    echo "ERROR: Mimic execution failed with exit code $RUN_STATUS"
    echo "Output:"
    cat "${RUN_OUTPUT}"
    rm -f "${RUN_OUTPUT}" "${TIME_OUTPUT}"
    error_exit "Mimic execution failed"
fi

if [ $VERBOSE -eq 1 ]; then
    echo "Mimic output:"
    cat "${RUN_OUTPUT}"
fi

rm -f "${RUN_OUTPUT}" "${TIME_OUTPUT}"

# Get system and build information
if git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    GIT_COMMIT=$(git rev-parse --short HEAD 2>/dev/null || echo 'unknown')
    GIT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo 'unknown')
    if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then
        GIT_DIRTY=true
    else
        GIT_DIRTY=false
    fi
else
    GIT_COMMIT='unknown'
    GIT_BRANCH='unknown'
    GIT_DIRTY=false
fi
KERNEL_RELEASE=$(uname -r 2>/dev/null || echo 'unknown')
KERNEL_VERSION=$(uname -v 2>/dev/null || echo 'unknown')
SYSTEM_PLATFORM=$(uname -s 2>/dev/null || echo 'unknown')
SYSTEM_ARCH=$(uname -m 2>/dev/null || echo 'unknown')
OS_VERSION='unknown'
if [[ "$SYSTEM_PLATFORM" == "Darwin" ]]; then
    OS_VERSION=$(sw_vers -productVersion 2>/dev/null || echo '')
elif command -v lsb_release > /dev/null 2>&1; then
    OS_VERSION=$(lsb_release -ds 2>/dev/null | tr -d '"')
elif [[ -f /etc/os-release ]]; then
    OS_VERSION=$(grep '^PRETTY_NAME=' /etc/os-release | head -1 | cut -d= -f2- | tr -d '"')
fi
OS_VERSION=${OS_VERSION:-unknown}
BUILD_FLAGS="${MAKE_FLAGS:-none}"

# Verify output was created - find any output file
if [[ -d "${OUTPUT_DIR}" ]]; then
    OUTPUT_FILE=$(find "${OUTPUT_DIR}" -maxdepth 1 -name "${FILE_PATTERN}" -type f -newer "${OUTPUT_MARKER}" 2>/dev/null | head -1)
    if [[ -z "$OUTPUT_FILE" ]]; then
        error_exit "Mimic did not produce any new output files in: ${OUTPUT_DIR}"
    fi
else
    error_exit "Output directory not found: ${OUTPUT_DIR}"
fi

verbose_log "Benchmark run completed successfully"
verbose_log "Output file created: $OUTPUT_FILE"

# Get total size of all output files
TOTAL_OUTPUT_SIZE=$(find "${OUTPUT_DIR}" -maxdepth 1 -name "${FILE_PATTERN}" -type f -newer "${OUTPUT_MARKER}" -exec stat -f%z {} + 2>/dev/null | awk '{s+=$1} END {print s}')
if [[ -z "$TOTAL_OUTPUT_SIZE" ]]; then
    TOTAL_OUTPUT_SIZE=$(find "${OUTPUT_DIR}" -maxdepth 1 -name "${FILE_PATTERN}" -type f -newer "${OUTPUT_MARKER}" -exec stat -c%s {} + 2>/dev/null | awk '{s+=$1} END {print s}')
fi
TOTAL_OUTPUT_SIZE=${TOTAL_OUTPUT_SIZE:-0}

# Create comprehensive JSON output
echo "Generating benchmark results..."

cat > "${ROOT_DIR}/benchmarks/${BENCHMARK_RESULTS}" << EOF
{
  "timestamp": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "git": {
    "commit": "${GIT_COMMIT}",
    "branch": "${GIT_BRANCH}",
    "dirty": ${GIT_DIRTY}
  },
  "system": {
    "hostname": "$(hostname -s 2>/dev/null || hostname)",
    "hw_model": "$(sysctl -n hw.model 2>/dev/null || echo 'unknown')",
    "platform": "${SYSTEM_PLATFORM}",
    "architecture": "${SYSTEM_ARCH}",
    "kernel_version": "${KERNEL_VERSION}",
    "os_version": "${OS_VERSION}",
    "cpu_count": "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 'unknown')"
  },
  "test_case": {
    "parameter_file": "$PARAM_FILE",
    "runtime_parameter_file": "$RUN_PARAM_FILE",
    "output_format": "$OUTPUT_FORMAT",
    "output_directory": "$OUTPUT_DIR",
    "num_processes": $NUM_MIMIC_PROCS,
    "mpi_command": "${MPI_RUN_COMMAND:-none}"
  },
  "configuration": {
    "model": "${SELECTED_MODEL}",
    "simulation": "${SELECTED_SIMULATION}",
    "build_flags": "$BUILD_FLAGS",
    "extra_cflags": "${EXTRA_CFLAGS:-none}",
    "runtime_flags": "${MIMIC_FLAGS:-none}",
    "build_system": "GNU Make",
    "mimic_executable": "$MIMIC_EXECUTABLE"
  },
  "overall_performance": {
    "wall_time_seconds": $REAL_TIME,
    "max_memory_mb": "$MAX_MEMORY",
    "total_output_size_bytes": $TOTAL_OUTPUT_SIZE,
    "sample_output_file": "$OUTPUT_FILE"
  },
  "notes": {
    "memory_measurement": "Memory measured using $([[ -x /usr/bin/time ]] && echo '/usr/bin/time' || echo 'fallback method (less accurate)')"
  }
}
EOF

# Display results summary
echo "Benchmark completed successfully."
echo
echo "=== Performance Summary ==="
echo "Parameter File: ${PARAM_FILE}"
echo "Wall Clock Time: ${REAL_TIME} seconds"
echo "Maximum Memory Usage: ${MAX_MEMORY} MB"
echo "Output Format: $OUTPUT_FORMAT"
echo "MPI Processes: $NUM_MIMIC_PROCS"
echo "Model Set: $SELECTED_MODEL"
echo "Simulation Package: $SELECTED_SIMULATION"
echo "Build Flags: $BUILD_FLAGS"
echo "Extra CFLAGS: ${EXTRA_CFLAGS:-none}"
echo "Runtime Flags: ${MIMIC_FLAGS:-none}"
echo "Git Commit: $GIT_COMMIT"
echo "Git Branch: $GIT_BRANCH"
echo "Git Dirty: $GIT_DIRTY"
echo
echo "Full results saved to: ${ROOT_DIR}/benchmarks/${BENCHMARK_RESULTS}"
echo
echo "USAGE NOTES:"
echo "  - Run this script regularly to track performance changes"
echo "  - Compare JSON files to identify performance regressions"
echo "  - Specify different parameter files to benchmark different datasets"
echo "  - Set MAKE_FLAGS for optimized builds (e.g., USE-HDF5=no for binary output-only, USE-MPI=yes)"
echo "  - Use MPI_RUN_COMMAND for parallel performance testing"
echo

# Exit successfully
exit 0
