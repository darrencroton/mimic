#!/bin/bash
# Test script for mimic-plot.py functionality
# Tests basic plotting operations with different command-line options

set -e  # Exit on first error

# Determine script location and mimic root
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MIMIC_PLOT_DIR="$(dirname "$SCRIPT_DIR")"
MIMIC_ROOT="$(dirname "$(dirname "$MIMIC_PLOT_DIR")")"

# Default parameter file
PARAM_FILE="${PARAM_FILE:-$MIMIC_ROOT/input/millennium.yaml}"

# Check parameter file exists
if [ ! -f "$PARAM_FILE" ]; then
    echo "Error: Parameter file not found: $PARAM_FILE"
    echo "Set PARAM_FILE environment variable or ensure millennium.yaml exists"
    exit 1
fi

# Check Python environment
if ! python3 -c "import numpy, matplotlib" 2>/dev/null; then
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

echo ""
echo "=========================================="
echo "All tests passed successfully!"
echo "Output saved to: $TEST_OUTPUT_DIR"
echo "=========================================="
