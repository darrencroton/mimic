#!/bin/bash
#
# benchmark_mimic.sh - Performance benchmark script for Mimic
#
# This script provides comprehensive performance benchmarking for the Mimic
# galaxy formation framework. It measures runtime, memory usage, and other
# performance metrics to help developers track performance changes over time.
#
# USAGE:
#   ./benchmark_mimic.sh             # Run with default settings
#   ./benchmark_mimic.sh --verbose   # Run with detailed output
#   ./benchmark_mimic.sh --help      # Show help information
#   ./benchmark_mimic.sh --format hdf5  # Benchmark HDF5 output (default: binary)
#
# REQUIREMENTS:
#   - Must be run from the scripts/ directory
#   - GNU Make must be available
#   - Test data must exist in tests/data/input/
#   - bc calculator (for memory calculations)
#   - time command (for performance measurement)
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
#   # Basic benchmark
#   ./benchmark_mimic.sh
#
#   # Benchmark with MPI
#   MPI_RUN_COMMAND="mpirun -np 4" MAKE_FLAGS="USE-MPI=yes" ./benchmark_mimic.sh
#
#   # HDF5 build benchmark
#   MAKE_FLAGS="USE-HDF5=yes" ./benchmark_mimic.sh --format hdf5
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
OUTPUT_FORMAT="binary"  # Default to binary format

# Process command line arguments
for arg in "$@"; do
    case $arg in
        --help)
            SHOW_HELP=1
            shift
            ;;
        --verbose)
            VERBOSE=1
            shift
            ;;
        --format)
            shift
            OUTPUT_FORMAT="$1"
            shift
            ;;
        --format=*)
            OUTPUT_FORMAT="${arg#*=}"
            shift
            ;;
        *)
            echo "Unknown option: $arg"
            echo "Usage: $0 [--help] [--verbose] [--format binary|hdf5]"
            exit 1
            ;;
    esac
done

# Show help if requested
if [ $SHOW_HELP -eq 1 ]; then
    cat << 'EOF'
Usage: ./benchmark_mimic.sh [OPTIONS]

OPTIONS:
  --help                Show this help message
  --verbose             Run with detailed output and timing information
  --format FORMAT       Output format to benchmark (binary or hdf5, default: binary)

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
  # Basic benchmark
  ./benchmark_mimic.sh

  # Verbose output with HDF5 format
  ./benchmark_mimic.sh --verbose --format hdf5

  # MPI benchmark
  MPI_RUN_COMMAND="mpirun -np 4" MAKE_FLAGS="USE-MPI=yes" ./benchmark_mimic.sh

  # HDF5 build benchmark
  MAKE_FLAGS="USE-HDF5=yes" ./benchmark_mimic.sh --format hdf5

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

# Validate output format
if [[ "$OUTPUT_FORMAT" != "binary" && "$OUTPUT_FORMAT" != "hdf5" ]]; then
    echo "ERROR: Invalid output format '$OUTPUT_FORMAT'. Must be 'binary' or 'hdf5'."
    exit 1
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
TEST_DATA_DIR="${ROOT_DIR}/tests/data"

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

echo "=== Mimic Performance Benchmark ==="
echo "Timestamp: $(date)"
echo "Output format: $OUTPUT_FORMAT"
echo "Saving results to: ${ROOT_DIR}/benchmarks/$(basename "$BENCHMARK_RESULTS")"
echo

# Check if test data exists
verbose_log "Checking for test data..."
if [ ! -f "${TEST_DATA_DIR}/input/trees_063.0" ]; then
    error_exit "Test data not found. Please run ./first_run.sh to set up test data."
fi

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

# Create parameter file for benchmark
echo "Preparing benchmark run..."
PARAM_FILE=$(mktemp)
if [[ "$OUTPUT_FORMAT" == "hdf5" ]]; then
    cp "${TEST_DATA_DIR}/test_hdf5.par" "${PARAM_FILE}" || error_exit "Could not copy HDF5 parameter file"
else
    cp "${TEST_DATA_DIR}/test_binary.par" "${PARAM_FILE}" || error_exit "Could not copy binary parameter file"
fi

# Clean any previous benchmark output
if [[ "$OUTPUT_FORMAT" == "hdf5" ]]; then
    rm -f "${TEST_DATA_DIR}"/output/hdf5/model*
else
    rm -f "${TEST_DATA_DIR}"/output/binary/model*
fi

verbose_log "Parameter file configured for $OUTPUT_FORMAT output"
if [ $VERBOSE -eq 1 ]; then
    echo "Parameter file contents (key settings):"
    grep -E "^(OutputFormat|OutputDir|TreeName)" "${PARAM_FILE}"
fi

# ======= Time and memory measurement =======
echo "Running Mimic benchmark (format: $OUTPUT_FORMAT)..."
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

# Verify output was created
if [[ "$OUTPUT_FORMAT" == "hdf5" ]]; then
    OUTPUT_FILE="${TEST_DATA_DIR}/output/hdf5/model_z0.000_0"
else
    OUTPUT_FILE="${TEST_DATA_DIR}/output/binary/model_z0.000_0"
fi

if [ ! -f "$OUTPUT_FILE" ]; then
    error_exit "Mimic did not produce expected output file: $OUTPUT_FILE"
fi

verbose_log "Benchmark run completed successfully"
verbose_log "Output file created: $OUTPUT_FILE"

# Clean up temporary files
rm -f "${PARAM_FILE}"

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
    "name": "Mini-Millennium (single tree file)",
    "output_format": "$OUTPUT_FORMAT",
    "num_processes": $NUM_MIMIC_PROCS,
    "mpi_command": "${MPI_RUN_COMMAND:-none}",
    "parameter_file": "tests/data/test_${OUTPUT_FORMAT}.par"
  },
  "configuration": {
    "build_flags": "$BUILD_FLAGS",
    "build_system": "GNU Make",
    "mimic_executable": "$MIMIC_EXECUTABLE"
  },
  "overall_performance": {
    "wall_time_seconds": $REAL_TIME,
    "max_memory_mb": "$MAX_MEMORY",
    "output_file_size_bytes": $(stat -f%z "$OUTPUT_FILE" 2>/dev/null || stat -c%s "$OUTPUT_FILE" 2>/dev/null || echo 0)
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
echo "  - Use different --format options to benchmark I/O performance"
echo "  - Set MAKE_FLAGS for optimized builds (e.g., USE-HDF5=yes)"
echo "  - Use MPI_RUN_COMMAND for parallel performance testing"
echo

# Exit successfully
exit 0
