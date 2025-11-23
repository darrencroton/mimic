#!/bin/bash
#
# benchmark_mimic.sh - Performance benchmark script for Mimic
#
# This script provides comprehensive performance benchmarking for the Mimic
# galaxy formation framework. It measures runtime, memory usage, and other
# performance metrics to help developers track performance changes over time.
#
# USAGE:
#   ./benchmark_mimic.sh                           # Run with default settings
#   ./benchmark_mimic.sh --verbose                 # Run with detailed output
#   ./benchmark_mimic.sh --help                    # Show help information
#   ./benchmark_mimic.sh --param-file custom.yaml   # Use custom parameter file
#   ./benchmark_mimic.sh input/millennium.yaml      # Positional parameter file argument
#
# REQUIREMENTS:
#   - Must be run from the scripts/ directory
#   - GNU Make must be available
#   - Parameter file must exist (default: input/millennium.yaml)
#
# OUTPUT:
#   Results are stored in JSON format in the benchmarks/ directory
#   with timestamp-based filenames for easy comparison across runs.
#
# ENVIRONMENT VARIABLES:
#   MIMIC_EXECUTABLE  - Override mimic executable location
#   MPI_RUN_COMMAND   - Run with MPI (e.g., "mpirun -np 4")
#   MAKE_FLAGS        - Additional make flags (e.g., "USE-HDF5=yes USE-MPI=yes")
#
# EXAMPLES:
#   # Basic benchmark (uses default input/millennium.yaml)
#   ./benchmark_mimic.sh
#
#   # Benchmark with custom parameter file
#   ./benchmark_mimic.sh --param-file ../tests/data/test_binary.yaml
#   ./benchmark_mimic.sh ../tests/data/test_binary.yaml
#
#   # Benchmark with MPI
#   MPI_RUN_COMMAND="mpirun -np 4" MAKE_FLAGS="USE-MPI=yes" ./benchmark_mimic.sh
#
#   # HDF5 build benchmark
#   MAKE_FLAGS="USE-HDF5=yes" ./benchmark_mimic.sh
#
#   # Compare two benchmark runs
#   diff ../benchmarks/baseline_20250101_120000.json ../benchmarks/baseline_20250102_120000.json
#

set -e  # Exit on any error

# Configuration
BENCHMARK_DIR="../benchmarks"
TIMESTAMP=$(date +"%Y%m%d_%H%M%S")
BENCHMARK_RESULTS="${BENCHMARK_DIR}/baseline_${TIMESTAMP}.json"
VERBOSE=0
SHOW_HELP=0
PARAM_FILE=""  # Will be set to default later if not specified

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
        -*)
            echo "Unknown option: $1"
            echo "Usage: $0 [--help] [--verbose] [--param-file FILE] [PARAM_FILE]"
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

# Function to display error and exit
error_exit() {
    echo "ERROR: $1"
    echo "Benchmark failed."
    exit 1
}

# Function to log verbose output
verbose_log() {
    if [ $VERBOSE -eq 1 ]; then
        echo "[VERBOSE] $1"
    fi
}

# Set default parameter file if not specified
if [[ -z "$PARAM_FILE" ]]; then
    PARAM_FILE="${ROOT_DIR}/input/millennium.yaml"
fi

# Show help if requested
if [ $SHOW_HELP -eq 1 ]; then
    cat << 'EOF'
Usage: ./benchmark_mimic.sh [OPTIONS] [PARAM_FILE]

OPTIONS:
  --help                Show this help message
  --verbose             Run with detailed output and timing information
  --param-file FILE     Parameter file to use for benchmarking

ARGUMENTS:
  PARAM_FILE            Parameter file to benchmark (default: input/millennium.yaml)
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
  Results are stored in ../benchmarks/ directory with the format:
  baseline_YYYYMMDD_HHMMSS.json

  JSON structure includes:
  - timestamp: ISO 8601 timestamp
  - version: Git commit hash/tag
  - system: System information
  - test_case: Test configuration used
  - overall_performance: Runtime and memory metrics
  - configuration: Build and runtime configuration

EXAMPLES:
  # Basic benchmark (uses default input/millennium.yaml)
  # Can run from anywhere:
  ./scripts/benchmark_mimic.sh
  cd scripts && ./benchmark_mimic.sh

  # Benchmark with custom parameter file
  ./scripts/benchmark_mimic.sh --param-file tests/data/test_binary.yaml
  ./scripts/benchmark_mimic.sh tests/data/test_binary.yaml

  # Verbose output
  ./scripts/benchmark_mimic.sh --verbose

  # MPI benchmark
  MPI_RUN_COMMAND="mpirun -np 4" MAKE_FLAGS="USE-MPI=yes" ./scripts/benchmark_mimic.sh

  # HDF5 build benchmark
  MAKE_FLAGS="USE-HDF5=yes" ./scripts/benchmark_mimic.sh

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

# Parse output directory and format from YAML parameter file using Python
read -r OUTPUT_DIR OUTPUT_FORMAT OUTPUT_BASENAME TREE_TYPE <<< $(python3 -c "
import sys
import yaml

param_file = '${PARAM_FILE}'

try:
    with open(param_file, 'r') as f:
        config = yaml.safe_load(f)

    output_dir = config.get('output', {}).get('directory', './output/')
    output_format = config.get('output', {}).get('format', 'binary')
    output_basename = config.get('output', {}).get('file_base_name', 'model')
    tree_type = config.get('input', {}).get('tree_type', 'lhalo_binary')

    # Remove trailing slash from output_dir
    output_dir = output_dir.rstrip('/')

    print(f'{output_dir} {output_format} {output_basename} {tree_type}')
except Exception as e:
    print(f'ERROR: Failed to parse YAML: {e}', file=sys.stderr)
    sys.exit(1)
")

# Resolve OUTPUT_DIR if it's a relative path
if [[ "$OUTPUT_DIR" != /* ]]; then
    # Relative path - resolve relative to ROOT_DIR
    OUTPUT_DIR="${ROOT_DIR}/${OUTPUT_DIR}"
fi

# CRITICAL SAFETY CHECK: Ensure variables are not empty
if [[ -z "${OUTPUT_DIR}" ]] || [[ -z "${OUTPUT_BASENAME}" ]]; then
    error_exit "CRITICAL: Failed to parse parameter file. OUTPUT_DIR='${OUTPUT_DIR}' OUTPUT_BASENAME='${OUTPUT_BASENAME}'. Aborting to prevent accidental deletion."
fi

verbose_log "Detected OutputDir: ${OUTPUT_DIR}"
verbose_log "Detected OutputFormat: ${OUTPUT_FORMAT}"
verbose_log "Detected OutputFileBaseName: ${OUTPUT_BASENAME}"
verbose_log "Detected TreeType: ${TREE_TYPE}"

# Auto-detect if HDF5 support is needed and add to MAKE_FLAGS
if [[ "$OUTPUT_FORMAT" == "hdf5" ]] || [[ "$TREE_TYPE" == *"hdf5"* ]]; then
    # Check if USE-HDF5 is already in MAKE_FLAGS
    if [[ ! "$MAKE_FLAGS" =~ USE-HDF5 ]]; then
        if [[ -z "$MAKE_FLAGS" ]]; then
            MAKE_FLAGS="USE-HDF5=yes"
        else
            MAKE_FLAGS="${MAKE_FLAGS} USE-HDF5=yes"
        fi
        verbose_log "Auto-detected HDF5 requirement, added USE-HDF5=yes to build flags"
    fi
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
echo "Output format: ${OUTPUT_FORMAT}"
echo "Saving results to: ${ROOT_DIR}/benchmarks/$(basename "$BENCHMARK_RESULTS")"
echo

# Build Mimic using Make
echo "Building Mimic using Make..."
cd "${ROOT_DIR}" || error_exit "Could not change to Mimic root directory"

# Clean and build
verbose_log "Cleaning previous build..."
make clean > /dev/null 2>&1 || true

# Generate module registration code
verbose_log "Generating module registration code..."
make generate > /dev/null 2>&1 || error_exit "Code generation failed"

verbose_log "Building Mimic with flags: ${MAKE_FLAGS}"
make -j$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1) ${MAKE_FLAGS} || error_exit "Build failed"

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

# Clean any previous benchmark output
# CRITICAL SAFETY CHECK: Double-check variables before deletion
if [[ -z "${OUTPUT_DIR}" ]] || [[ -z "${OUTPUT_BASENAME}" ]]; then
    error_exit "CRITICAL SAFETY: OUTPUT_DIR or OUTPUT_BASENAME is empty. Refusing to delete files."
fi

if [[ -d "${OUTPUT_DIR}" ]]; then
    verbose_log "Cleaning previous output in ${OUTPUT_DIR}"
    # Additional safety: ensure OUTPUT_DIR is an absolute path and contains 'output' or 'benchmark'
    if [[ "${OUTPUT_DIR}" == /* ]] && [[ "${OUTPUT_DIR}" == *output* || "${OUTPUT_DIR}" == *benchmark* ]]; then
        rm -f "${OUTPUT_DIR}"/${OUTPUT_BASENAME}*
    else
        error_exit "CRITICAL SAFETY: OUTPUT_DIR='${OUTPUT_DIR}' does not look like a safe output directory. Refusing to delete files."
    fi
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

# Platform-specific timing and memory measurement
RUN_OUTPUT=$(mktemp)
TIME_OUTPUT=$(mktemp)

# Check if /usr/bin/time is available
if [ -x "/usr/bin/time" ]; then
    verbose_log "Using /usr/bin/time for accurate measurement"

    # Platform-specific time command
    if [ "$(uname)" = "Darwin" ]; then
        # macOS version
        verbose_log "Detected macOS"
        if [[ -n "${MPI_RUN_COMMAND}" ]]; then
            /usr/bin/time -l ${MPI_RUN_COMMAND} "${MIMIC_EXECUTABLE}" "${PARAM_FILE}" > "${RUN_OUTPUT}" 2> "${TIME_OUTPUT}"
            RUN_STATUS=$?
        else
            /usr/bin/time -l "${MIMIC_EXECUTABLE}" "${PARAM_FILE}" > "${RUN_OUTPUT}" 2> "${TIME_OUTPUT}"
            RUN_STATUS=$?
        fi

        # Extract wall clock time (user + system)
        USER_TIME=$(grep "user" "${TIME_OUTPUT}" | awk '{print $1}')
        SYS_TIME=$(grep "sys" "${TIME_OUTPUT}" | awk '{print $1}')
        if command -v bc > /dev/null 2>&1; then
            REAL_TIME=$(echo "$USER_TIME + $SYS_TIME" | bc)
        else
            REAL_TIME=$(awk "BEGIN {print $USER_TIME + $SYS_TIME}")
        fi

        # Memory usage (convert bytes to MB)
        MAX_MEMORY=$(grep "maximum resident set size" "${TIME_OUTPUT}" | awk '{print $1}')
        if command -v bc > /dev/null 2>&1 && [[ -n "$MAX_MEMORY" ]]; then
            MAX_MEMORY=$(echo "scale=2; ${MAX_MEMORY} / 1048576" | bc)
        else
            MAX_MEMORY=$(awk "BEGIN {printf \"%.2f\", ${MAX_MEMORY:-0} / 1048576}")
        fi
    else
        # Linux version
        verbose_log "Detected Linux"
        if [[ -n "${MPI_RUN_COMMAND}" ]]; then
            /usr/bin/time -v ${MPI_RUN_COMMAND} "${MIMIC_EXECUTABLE}" "${PARAM_FILE}" > "${RUN_OUTPUT}" 2> "${TIME_OUTPUT}"
            RUN_STATUS=$?
        else
            /usr/bin/time -v "${MIMIC_EXECUTABLE}" "${PARAM_FILE}" > "${RUN_OUTPUT}" 2> "${TIME_OUTPUT}"
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
        if command -v bc > /dev/null 2>&1 && [[ -n "$MAX_MEMORY" ]]; then
            MAX_MEMORY=$(echo "scale=2; ${MAX_MEMORY} / 1024" | bc)
        else
            MAX_MEMORY=$(awk "BEGIN {printf \"%.2f\", ${MAX_MEMORY:-0} / 1024}")
        fi
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
        ${MPI_RUN_COMMAND} "${MIMIC_EXECUTABLE}" "${PARAM_FILE}" > "${RUN_OUTPUT}" 2>&1
        RUN_STATUS=$?
    else
        "${MIMIC_EXECUTABLE}" "${PARAM_FILE}" > "${RUN_OUTPUT}" 2>&1
        RUN_STATUS=$?
    fi

    end_time=$(date +%s)
    REAL_TIME=$((end_time - start_time))
    MAX_MEMORY="N/A"
    verbose_log "Memory measurement not available without /usr/bin/time"
fi

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
GIT_VERSION=$(git describe --always --dirty 2>/dev/null || echo 'unknown')
BUILD_FLAGS="${MAKE_FLAGS:-none}"

# Verify output was created - find any output file
if [[ -d "${OUTPUT_DIR}" ]]; then
    OUTPUT_FILE=$(find "${OUTPUT_DIR}" -maxdepth 1 -name "${FILE_PATTERN}" -type f 2>/dev/null | head -1)
    if [[ -z "$OUTPUT_FILE" ]]; then
        error_exit "Mimic did not produce any output files in: ${OUTPUT_DIR}"
    fi
else
    error_exit "Output directory not found: ${OUTPUT_DIR}"
fi

verbose_log "Benchmark run completed successfully"
verbose_log "Output file created: $OUTPUT_FILE"

# Get total size of all output files
TOTAL_OUTPUT_SIZE=$(find "${OUTPUT_DIR}" -maxdepth 1 -name "${FILE_PATTERN}" -type f -exec stat -f%z {} + 2>/dev/null | awk '{s+=$1} END {print s}')
if [[ -z "$TOTAL_OUTPUT_SIZE" ]]; then
    TOTAL_OUTPUT_SIZE=$(find "${OUTPUT_DIR}" -maxdepth 1 -name "${FILE_PATTERN}" -type f -exec stat -c%s {} + 2>/dev/null | awk '{s+=$1} END {print s}')
fi
TOTAL_OUTPUT_SIZE=${TOTAL_OUTPUT_SIZE:-0}

# Create comprehensive JSON output
echo "Generating benchmark results..."

cat > "${ROOT_DIR}/benchmarks/baseline_${TIMESTAMP}.json" << EOF
{
  "timestamp": "$(date -u +"%Y-%m-%dT%H:%M:%SZ")",
  "version": "${GIT_VERSION}",
  "system": {
    "uname": "$(uname -a)",
    "platform": "$(uname -s)",
    "architecture": "$(uname -m)",
    "cpu_count": "$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 'unknown')"
  },
  "test_case": {
    "parameter_file": "$PARAM_FILE",
    "output_format": "$OUTPUT_FORMAT",
    "output_directory": "$OUTPUT_DIR",
    "num_processes": $NUM_MIMIC_PROCS,
    "mpi_command": "${MPI_RUN_COMMAND:-none}"
  },
  "configuration": {
    "build_flags": "$BUILD_FLAGS",
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
echo "Build Flags: $BUILD_FLAGS"
echo "Git Version: $GIT_VERSION"
echo
echo "Full results saved to: ${ROOT_DIR}/benchmarks/baseline_${TIMESTAMP}.json"
echo
echo "USAGE NOTES:"
echo "  - Run this script regularly to track performance changes"
echo "  - Compare JSON files to identify performance regressions"
echo "  - Specify different parameter files to benchmark different datasets"
echo "  - Set MAKE_FLAGS for optimized builds (e.g., USE-HDF5=yes)"
echo "  - Use MPI_RUN_COMMAND for parallel performance testing"
echo

# Exit successfully
exit 0
