#!/bin/bash

# Simple script to test the mimic-plot.py functionality
# This script assumes you're in the Mimic root directory

# Find your parameter file
if [ -f "input/millennium.yaml" ]; then
    PARAM_FILE="input/millennium.yaml"
    echo "Using millennium.yaml for testing"
elif [ -f "input/mini-millennium.yaml" ]; then
    PARAM_FILE="input/mini-millennium.yaml"
    echo "Using mini-millennium.yaml for testing"
else
    # Create a test parameter file if none exists
    PARAM_FILE="input/test.yaml"
    mkdir -p input
    echo "Creating test parameter file at $PARAM_FILE"
    echo "OutputDir = ./output/" > $PARAM_FILE
    echo "FileNameGalaxies = model" >> $PARAM_FILE
    echo "LastSnapshotNr = 63" >> $PARAM_FILE
    echo "FirstFile = 0" >> $PARAM_FILE
    echo "NumFiles = 8" >> $PARAM_FILE
    echo "Hubble_h = 0.73" >> $PARAM_FILE
    echo "IMF_Type = 1" >> $PARAM_FILE
fi

# Create output directory if it doesn't exist
mkdir -p output/test-plots

echo "=== Testing Stellar Mass Function ==="
python output/mimic-plot/mimic-plot.py --param-file=$PARAM_FILE --output-dir=output/test-plots --plots=stellar_mass_function

echo "=== Testing Evolution Plots ==="
python output/mimic-plot/mimic-plot.py --param-file=$PARAM_FILE --output-dir=output/test-plots --plots=sfr_density_evolution --evolution-plots

echo "=== Tests Complete ==="
echo "Check output/test-plots directory for results"
