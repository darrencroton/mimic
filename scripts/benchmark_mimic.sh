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
#   ./benchmark_mimic.sh --param-file custom.par   # Use custom parameter file
#   ./benchmark_mimic.sh input/millennium.par      # Positional parameter file argument
#
# REQUIREMENTS:
#   - Must be run from the scripts/ directory
#   - GNU Make must be available
#   - Parameter file must exist (default: input/millennium.par)
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
#   # Basic benchmark (uses default input/millennium.par)
#   ./benchmark_mimic.sh
#
#   # Benchmark with custom parameter file
#   ./benchmark_mimic.sh --param-file ../tests/data/test_binary.par
#   ./benchmark_mimic.sh ../tests/data/test_binary.par
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

# Set default parameter file if not specified
if [[ -z "$PARAM_FILE" ]]; then
    PARAM_FILE="../input/millennium.par"
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
  PARAM_FILE            Parameter file to benchmark (default: ../input/millennium.par)
                        Can be specified as positional argument or with --param-file

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
  # Basic benchmark (uses default input/millennium.par)
  ./benchmark_mimic.sh

  # Benchmark with custom parameter file
  ./benchmark_mimic.sh --param-file ../tests/data/test_binary.par
  ./benchmark_mimic.sh ../tests/data/test_binary.par

  # Verbose output
  ./benchmark_mimic.sh --verbose

  # MPI benchmark
  MPI_RUN_COMMAND="mpirun -np 4" MAKE_FLAGS="USE-MPI=yes" ./benchmark_mimic.sh

  # HDF5 build benchmark
  MAKE_FLAGS="USE-HDF5=yes" ./benchmark_mimic.sh

COMPARING RESULTS:
  # Simple diff
  diff ../benchmarks/baseline_old.json ../benchmarks/baseline_new.json

  # Extract key metrics
  grep "wall_time\|max_memory" ../benchmarks/baseline_*.json

  # Plot trends over time (requires custom analysis script)
  python plot_benchmark_trends.py ../benchmarks/

TROUBLESHOOTING:
  - Ensure you're running from the scripts/ directory
  - Check that GNU Make and build tools are installed
  - Verify test data exists in tests/data/input/
  - For memory measurement issues, check 'time' command availability
  - For MPI issues, verify MPI installation and mpirun availability
EOF
    exit 0
fi

# Determine script directories - we should be in scripts/ directory
SCRIPT_DIR=$(pwd)
if [[ ! "$(basename "$SCRIPT_DIR")" == "scripts" ]]; then
    echo "ERROR: This script must be run from the scripts/ directory"
    echo "Current directory: $SCRIPT_DIR"
    echo "Please cd to the scripts/ directory and run ./benchmark_mimic.sh"
    exit 1
fi

ROOT_DIR="$(dirname "$SCRIPT_DIR")"

# Create benchmark directory if it doesn't exist
mkdir -p "${ROOT_DIR}/benchmarks"

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

# Validate parameter file exists
if [[ ! -f "${PARAM_FILE}" ]]; then
    # Try with ROOT_DIR prepended if it's a relative path
    if [[ ! -f "${ROOT_DIR}/${PARAM_FILE}" ]]; then
        error_exit "Parameter file not found: ${PARAM_FILE}"
    fi
    PARAM_FILE="${ROOT_DIR}/${PARAM_FILE}"
fi

verbose_log "Using parameter file: ${PARAM_FILE}"

# Parse output directory and format from parameter file
OUTPUT_DIR=$(grep "^OutputDir" "${PARAM_FILE}" | awk '{print $2}' | sed 's|/$||')
OUTPUT_FORMAT=$(grep "^OutputFormat" "${PARAM_FILE}" | awk '{print $2}' | tr -d '\r')
OUTPUT_BASENAME=$(grep "^OutputFileBaseName" "${PARAM_FILE}" | awk '{print $2}' | tr -d '\r')

verbose_log "Detected OutputDir: ${OUTPUT_DIR}"
verbose_log "Detected OutputFormat: ${OUTPUT_FORMAT}"
verbose_log "Detected OutputFileBaseName: ${OUTPUT_BASENAME}"

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
if [[ -d "${OUTPUT_DIR}" ]]; then
    verbose_log "Cleaning previous output in ${OUTPUT_DIR}"
    rm -f "${OUTPUT_DIR}"/${OUTPUT_BASENAME}*
fi

if [ $VERBOSE -eq 1 ]; then
    echo "Parameter file settings:"
    grep -E "^(OutputFormat|OutputDir|TreeName|FirstFile|LastFile)" "${PARAM_FILE}"
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

# Simple portable timing measurement
verbose_log "Using portable time measurement"
start_time=$(date +%s)

# Run Mimic
RUN_OUTPUT=$(mktemp)
if [[ -n "${MPI_RUN_COMMAND}" ]]; then
    ${MPI_RUN_COMMAND} "${MIMIC_EXECUTABLE}" "${PARAM_FILE}" > "${RUN_OUTPUT}" 2>&1
    RUN_STATUS=$?
else
    "${MIMIC_EXECUTABLE}" "${PARAM_FILE}" > "${RUN_OUTPUT}" 2>&1
    RUN_STATUS=$?
fi

end_time=$(date +%s)

# Check if run was successful
if [ $RUN_STATUS -ne 0 ]; then
    echo "ERROR: Mimic execution failed with exit code $RUN_STATUS"
    if [ $VERBOSE -eq 1 ]; then
        echo "Output:"
        cat "${RUN_OUTPUT}"
    fi
    rm -f "${RUN_OUTPUT}"
    error_exit "Mimic execution failed"
fi

# Calculate elapsed time
REAL_TIME=$((end_time - start_time))

# Try to get memory usage from /proc if available (Linux)
MAX_MEMORY=0
if [ -f /proc/meminfo ]; then
    # This is a simple approximation - actual peak memory would require monitoring during execution
    # For production use, consider using /usr/bin/time if available, or add instrumentation to Mimic
    verbose_log "Memory measurement not available in this environment (no /usr/bin/time)"
    MAX_MEMORY="N/A"
else
    verbose_log "Memory measurement not available on this platform"
    MAX_MEMORY="N/A"
fi

if [ $VERBOSE -eq 1 ]; then
    echo "Mimic output:"
    cat "${RUN_OUTPUT}"
fi

rm -f "${RUN_OUTPUT}"

# Get system and build information
GIT_VERSION=$(git describe --always --dirty 2>/dev/null || echo 'unknown')
BUILD_FLAGS="${MAKE_FLAGS:-none}"

# Verify output was created - find any output file
if [[ -d "${OUTPUT_DIR}" ]]; then
    OUTPUT_FILE=$(find "${OUTPUT_DIR}" -name "${OUTPUT_BASENAME}_z*" -type f 2>/dev/null | head -1)
    if [[ -z "$OUTPUT_FILE" ]]; then
        error_exit "Mimic did not produce any output files in: ${OUTPUT_DIR}"
    fi
else
    error_exit "Output directory not found: ${OUTPUT_DIR}"
fi

verbose_log "Benchmark run completed successfully"
verbose_log "Output file created: $OUTPUT_FILE"

# Get total size of all output files
TOTAL_OUTPUT_SIZE=$(find "${OUTPUT_DIR}" -name "${OUTPUT_BASENAME}_z*" -type f -exec stat -f%z {} + 2>/dev/null | awk '{s+=$1} END {print s}')
if [[ -z "$TOTAL_OUTPUT_SIZE" ]]; then
    TOTAL_OUTPUT_SIZE=$(find "${OUTPUT_DIR}" -name "${OUTPUT_BASENAME}_z*" -type f -exec stat -c%s {} + 2>/dev/null | awk '{s+=$1} END {print s}')
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
    "memory_measurement": "Memory measurement requires /usr/bin/time or similar tools. Consider adding instrumentation to Mimic for accurate memory tracking."
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
