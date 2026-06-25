#!/bin/bash
# Test script for mimic-plot.py functionality
# Tests basic plotting operations with different command-line options
#
# This suite is deliberately standalone (not part of `make tests`): it needs
# the plotting virtual environment (mimic_venv) and real Mimic output data
# from a prior run of the default model. The Python unit tests it invokes
# emit the standard MIMIC_RESULT markers; this driver itself gates on exit
# codes via `set -e`.

set -e  # Exit on first error

# Determine script location and mimic root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MIMIC_PLOT_DIR="$(dirname "$SCRIPT_DIR")"
MIMIC_ROOT="$(dirname "$(dirname "$MIMIC_PLOT_DIR")")"

# Load shared project defaults (DEFAULT_MODEL, DEFAULT_SIMULATION, make_default)
# shellcheck source=scripts/lib/defaults.sh
. "${MIMIC_ROOT}/scripts/lib/defaults.sh"

# Default parameter file
PARAM_FILE="${PARAM_FILE:-$MIMIC_ROOT/models/${DEFAULT_MODEL}/input/${DEFAULT_MODEL}_${DEFAULT_SIMULATION}.yaml}"

# Check parameter file exists
if [ ! -f "$PARAM_FILE" ]; then
    echo "Error: Parameter file not found: $PARAM_FILE"
    echo "Set PARAM_FILE environment variable or ensure the default run file (models/${DEFAULT_MODEL}/input/...) exists"
    exit 1
fi

# Check Python environment
if ! python3 -c "import numpy, matplotlib, h5py" 2>/dev/null; then
    echo "Error: Required Python packages not available"
    echo "Activate virtual environment: source $MIMIC_ROOT/mimic_venv/bin/activate"
    exit 1
fi

# Create test output directory
TEST_OUTPUT_DIR="$MIMIC_PLOT_DIR/tests/test_output"
mkdir -p "$TEST_OUTPUT_DIR"

echo "=========================================="
echo "Mimic Plotting System Tests"
echo "=========================================="
echo "Parameter file: $PARAM_FILE"
echo "Output directory: $TEST_OUTPUT_DIR"
echo ""

# Test 1: Single snapshot plot
echo "Test 1: Single snapshot plot (halo_mass_function)"
python3 "$MIMIC_PLOT_DIR/mimic-plot.py" \
    --param-file="$PARAM_FILE" \
    --output-dir="$TEST_OUTPUT_DIR" \
    --plots=halo_mass_function \
    --quiet

# Test 2: Single evolution plot
echo "Test 2: Single evolution plot (hmf_evolution)"
python3 "$MIMIC_PLOT_DIR/mimic-plot.py" \
    --param-file="$PARAM_FILE" \
    --output-dir="$TEST_OUTPUT_DIR" \
    --plots=hmf_evolution \
    --quiet

# Test 3: Multiple plots
echo "Test 3: Multiple plots (snapshot and evolution)"
python3 "$MIMIC_PLOT_DIR/mimic-plot.py" \
    --param-file="$PARAM_FILE" \
    --output-dir="$TEST_OUTPUT_DIR" \
    --plots=halo_occupation,spin_distribution \
    --quiet

# Test 4: Snapshot plots only
echo "Test 4: Snapshot plots only flag"
python3 "$MIMIC_PLOT_DIR/mimic-plot.py" \
    --param-file="$PARAM_FILE" \
    --output-dir="$TEST_OUTPUT_DIR" \
    --snapshot-plots \
    --plots=velocity_distribution \
    --quiet

# Test 5: Evolution plots only
echo "Test 5: Evolution plots only flag"
python3 "$MIMIC_PLOT_DIR/mimic-plot.py" \
    --param-file="$PARAM_FILE" \
    --output-dir="$TEST_OUTPUT_DIR" \
    --evolution-plots \
    --plots=hmf_evolution \
    --quiet

echo "Test 6: SAGE-native HDF5 reader unit tests"
python3 "$SCRIPT_DIR/test_sage_native_hdf5.py"

echo "Test 7: Plot profile inheritance unit tests"
python3 "$SCRIPT_DIR/test_profile_inheritance.py"

echo "Test 8: Snapshot redshift mapper unit tests"
python3 "$SCRIPT_DIR/test_snapshot_redshift_mapper.py"

echo "Test 9: Validation helper unit tests"
python3 "$SCRIPT_DIR/test_validation_helpers.py"

echo "Test 10: Chunked output consumer unit tests"
python3 "$SCRIPT_DIR/test_chunked_consumers.py"

echo ""
echo "=========================================="
echo "All tests passed successfully!"
echo "Output saved to: $TEST_OUTPUT_DIR"
echo "=========================================="
