#!/bin/bash
###############################################################################
# regenerate_baseline.sh - Regenerate HDF5 baseline for integration tests
#
# This script regenerates the HDF5 baseline file used for core property
# validation in integration tests. The baseline ensures that core halo
# tracking remains deterministic across code changes.
#
# IMPORTANT: Only regenerate baseline after deliberate, validated changes
#            to core halo tracking algorithms. Never regenerate to "fix"
#            a failing test - investigate the failure first!
#
# Usage:
#   ./scripts/regenerate_baseline.sh
#
# What this script does:
#   1. Verifies mimic is compiled with HDF5 support
#   2. Validates that parameter file is physics-free (no modules enabled)
#   3. Runs mimic to generate fresh baseline
#   4. Copies output to baseline directory
#   5. Validates baseline against current output
#   6. Provides git commit instructions
#
# Author: Mimic Testing Team
# Date: 2025-11-13
# Phase: Phase 4.2 (Testing Framework Refinement)
###############################################################################

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Get repository root (one level up from scripts/)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

# Paths
MIMIC_EXE="$REPO_ROOT/mimic"
PARAM_FILE="$REPO_ROOT/tests/data/test_hdf5.yaml"
OUTPUT_DIR="$REPO_ROOT/tests/data/output/hdf5"
BASELINE_DIR="$REPO_ROOT/tests/data/output/baseline/hdf5"
OUTPUT_FILE="$OUTPUT_DIR/model_000.hdf5"
BASELINE_FILE="$BASELINE_DIR/model_000.hdf5"

echo "============================================================"
echo "Mimic Baseline Regeneration Script"
echo "============================================================"
echo "Repository: $REPO_ROOT"
echo "Parameter file: $PARAM_FILE"
echo ""

# Step 1: Check mimic is compiled
echo -e "${BLUE}Step 1: Checking Mimic executable...${NC}"
if [ ! -f "$MIMIC_EXE" ]; then
    echo -e "${RED}ERROR: Mimic executable not found at $MIMIC_EXE${NC}"
    echo "Build it first with: make USE-HDF5=yes"
    exit 1
fi
echo -e "${GREEN}✓ Mimic executable found${NC}"
echo ""

# Step 2: Check HDF5 support
echo -e "${BLUE}Step 2: Checking HDF5 support...${NC}"
# Run a quick test to see if HDF5 is compiled in
if ! "$MIMIC_EXE" --version 2>&1 | grep -qi "hdf5" && ! ldd "$MIMIC_EXE" 2>/dev/null | grep -q "hdf5"; then
    echo -e "${YELLOW}WARNING: Cannot verify HDF5 support from executable${NC}"
    echo "Assuming HDF5 is available. If next step fails, rebuild with: make clean && make USE-HDF5=yes"
fi
echo -e "${GREEN}✓ HDF5 support appears available${NC}"
echo ""

# Step 3: Validate parameter file exists
echo -e "${BLUE}Step 3: Validating parameter file...${NC}"
if [ ! -f "$PARAM_FILE" ]; then
    echo -e "${RED}ERROR: Parameter file not found: $PARAM_FILE${NC}"
    echo "Expected parameter file: tests/data/test_hdf5.yaml"
    exit 1
fi

# Check that EnabledModules is empty (physics-free mode)
if grep -E "^EnabledModules\s+\S" "$PARAM_FILE" > /dev/null; then
    echo -e "${RED}ERROR: Parameter file has modules enabled${NC}"
    echo ""
    echo "Baseline MUST be generated in physics-free mode"
    echo "(EnabledModules line should be blank or commented)"
    echo ""
    echo "Edit $PARAM_FILE and ensure:"
    echo "  EnabledModules"
    echo "  (no module names after the parameter)"
    echo ""
    exit 1
fi

echo -e "${GREEN}✓ Parameter file is physics-free (no modules enabled)${NC}"
echo ""

# Step 4: Create output directory
echo -e "${BLUE}Step 4: Preparing directories...${NC}"
mkdir -p "$OUTPUT_DIR"
mkdir -p "$BASELINE_DIR"
echo -e "${GREEN}✓ Directories ready${NC}"
echo ""

# Step 5: Run Mimic to generate baseline
echo -e "${BLUE}Step 5: Running Mimic to generate baseline...${NC}"
echo "Command: $MIMIC_EXE $PARAM_FILE"
echo ""

if ! "$MIMIC_EXE" "$PARAM_FILE"; then
    echo ""
    echo -e "${RED}ERROR: Mimic execution failed${NC}"
    echo "Check output above for error messages"
    exit 1
fi

echo ""
echo -e "${GREEN}✓ Mimic executed successfully${NC}"
echo ""

# Step 6: Verify output file exists
echo -e "${BLUE}Step 6: Verifying output file...${NC}"
if [ ! -f "$OUTPUT_FILE" ]; then
    echo -e "${RED}ERROR: Output file not created: $OUTPUT_FILE${NC}"
    echo "Expected Mimic to create HDF5 output"
    exit 1
fi

OUTPUT_SIZE=$(stat -f%z "$OUTPUT_FILE" 2>/dev/null || stat -c%s "$OUTPUT_FILE" 2>/dev/null)
echo "Output file: $OUTPUT_FILE"
echo "File size: $OUTPUT_SIZE bytes"
echo -e "${GREEN}✓ Output file created successfully${NC}"
echo ""

# Step 7: Backup existing baseline (if it exists)
if [ -f "$BASELINE_FILE" ]; then
    echo -e "${BLUE}Step 7: Backing up existing baseline...${NC}"
    BACKUP_FILE="${BASELINE_FILE}.backup.$(date +%Y%m%d_%H%M%S)"
    cp "$BASELINE_FILE" "$BACKUP_FILE"
    echo "Backed up to: $BACKUP_FILE"
    echo -e "${GREEN}✓ Existing baseline backed up${NC}"
    echo ""
else
    echo -e "${BLUE}Step 7: No existing baseline to backup${NC}"
    echo ""
fi

# Step 8: Copy to baseline directory
echo -e "${BLUE}Step 8: Installing new baseline...${NC}"
cp "$OUTPUT_FILE" "$BASELINE_FILE"
BASELINE_SIZE=$(stat -f%z "$BASELINE_FILE" 2>/dev/null || stat -c%s "$BASELINE_FILE" 2>/dev/null)
echo "Baseline file: $BASELINE_FILE"
echo "File size: $BASELINE_SIZE bytes"
echo -e "${GREEN}✓ New baseline installed${NC}"
echo ""

# Step 9: Validate baseline
echo -e "${BLUE}Step 9: Validating baseline...${NC}"
echo "Running baseline comparison test..."
echo ""

cd "$REPO_ROOT/tests/integration" || exit 1
if python3 -c "
import sys
sys.path.insert(0, '..')
from test_output_formats import test_hdf5_baseline_comparison
try:
    test_hdf5_baseline_comparison()
    print('${GREEN}✓ Baseline validation passed${NC}')
    sys.exit(0)
except AssertionError as e:
    print('${RED}✗ Baseline validation failed:${NC}')
    print(str(e))
    sys.exit(1)
except Exception as e:
    print('${YELLOW}⚠ Could not run validation:${NC}')
    print(str(e))
    print('${YELLOW}Baseline installed but not validated${NC}')
    sys.exit(0)
"; then
    echo ""
else
    echo ""
    echo -e "${YELLOW}WARNING: Validation had issues (see above)${NC}"
    echo "Baseline has been installed but may need manual verification"
    echo ""
fi

cd "$REPO_ROOT" || exit 1

# Step 10: Git status and commit instructions
echo ""
echo "============================================================"
echo -e "${GREEN}Baseline regeneration complete!${NC}"
echo "============================================================"
echo ""
echo "Next steps:"
echo ""
echo "1. Review the change:"
echo "   git diff tests/data/output/baseline/hdf5/model_000.hdf5"
echo ""
echo "2. Verify tests pass:"
echo "   make test-integration"
echo ""
echo "3. Commit the new baseline:"
echo "   git add tests/data/output/baseline/hdf5/model_000.hdf5"
echo "   git commit -m \"test: regenerate HDF5 baseline after [reason]"
echo ""
echo "   [Describe why baseline needed regeneration, e.g.:]"
echo "   - Updated virial radius calculation"
echo "   - Fixed dT inheritance bug"
echo "   - Changed tree traversal algorithm"
echo "   \""
echo ""
echo "4. Document the change:"
echo "   Add entry to CHANGELOG.md or relevant documentation"
echo ""
echo -e "${YELLOW}IMPORTANT:${NC}"
echo "  - Baseline changes affect all future tests"
echo "  - Only commit after thorough validation"
echo "  - Include detailed reason in commit message"
echo "  - Update documentation if behavior changed"
echo ""

exit 0
