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
# The baseline is a coherent SET: the shard, the master file, and every file
# in metadata/ must all come from the same run, so this script installs them
# together (tests/data/README.md). The committed baseline belongs to the
# default package pair, and only that pair can validate it, so a non-default
# MODEL/SIMULATION is refused rather than silently installed unvalidated.
#
# Usage:
#   ./scripts/regenerate_baseline.sh
#   ./scripts/regenerate_baseline.sh --help
#
# What this script does:
#   1. Verifies the selected package pair owns the committed baseline
#   2. Verifies mimic is compiled with HDF5 support
#   3. Validates that parameter file is physics-free (no modules enabled)
#   4. Runs mimic to generate fresh baseline
#   5. Installs the shard, master, and metadata/ set into the baseline
#   6. Validates baseline against current output, failing if it cannot
#   7. Provides git commit instructions
###############################################################################

show_help() {
    cat <<'EOF'
Usage: ./scripts/regenerate_baseline.sh [--help]

Regenerate the HDF5 baseline file used by Mimic integration tests.

Options:
  --help, -h    Show this help message and exit without changing files

Environment:
  MODEL         Model package to use (default: Makefile DEFAULT_MODEL)
  SIMULATION    Simulation package to use (default: Makefile DEFAULT_SIMULATION)

Both must match the default pair: the committed baseline belongs to that pair
and only that pair can validate it. Any other selection is refused.

What this script does:
  1. Verifies the selected package pair owns the committed baseline
  2. Verifies mimic is compiled with HDF5 support
  3. Validates that the generated HDF5 test parameter file is physics-free
  4. Runs mimic to generate fresh baseline output
  5. Backs up and replaces the baseline shard, master, and metadata/ set
  6. Runs the HDF5 baseline comparison check, failing if it cannot run

Only regenerate the baseline after deliberate, validated changes to core halo
tracking behavior. Never regenerate it merely to silence a failing test.
EOF
}

for arg in "$@"; do
    case "$arg" in
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            echo "ERROR: Unknown option: $arg" >&2
            echo "Usage: ./scripts/regenerate_baseline.sh [--help]" >&2
            exit 1
            ;;
    esac
done

# Get repository root (one level up from scripts/)
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$REPO_ROOT" || exit 1

# Load shared project defaults (DEFAULT_MODEL, DEFAULT_SIMULATION, make_default)
# and the shared ANSI colour codes (RED/GREEN/YELLOW/BLUE/NC).
# shellcheck source=scripts/lib/defaults.sh
. "${REPO_ROOT}/scripts/lib/defaults.sh"
# shellcheck source=scripts/lib/colors.sh
. "${REPO_ROOT}/scripts/lib/colors.sh"

# Paths
MIMIC_EXE="$REPO_ROOT/mimic"
MODEL="${MODEL:-${DEFAULT_MODEL}}"
export MODEL
SIMULATION="${SIMULATION:-${DEFAULT_SIMULATION}}"
export SIMULATION
PARAM_FILE="$REPO_ROOT/build/generated/test_inputs/$MODEL/$SIMULATION/core/test_hdf5.yaml"
OUTPUT_DIR="$REPO_ROOT/tests/data/output/hdf5"
BASELINE_DIR="$REPO_ROOT/tests/data/output/baseline/hdf5"

# The committed baseline is a coherent set written by one run: the shard, the
# master, and the whole metadata/ directory (schema sidecar, run and simulation
# configs, snapshot list, version info). Anything installed piecemeal leaves the
# baseline describing a run that did not produce it.
OUTPUT_FILE="$OUTPUT_DIR/model_000.hdf5"
OUTPUT_MASTER="$OUTPUT_DIR/model.hdf5"
OUTPUT_METADATA="$OUTPUT_DIR/metadata"
OUTPUT_SCHEMA="$OUTPUT_METADATA/output_schema.json"
BASELINE_FILE="$BASELINE_DIR/model_000.hdf5"
BASELINE_MASTER="$BASELINE_DIR/model.hdf5"
BASELINE_METADATA="$BASELINE_DIR/metadata"

echo "============================================================"
echo "Mimic Baseline Regeneration Script"
echo "============================================================"
echo "Repository: $REPO_ROOT"
echo "Model: $MODEL"
echo "Simulation: $SIMULATION"
echo "Parameter file: $PARAM_FILE"
echo ""

# Step 0: Refuse to install a baseline the selected pair cannot validate.
# The comparison test skips for any non-default pair, so installing from one
# would overwrite the committed baseline with output nothing checks. The owning
# pair is read straight from the Makefile: defaults.sh honours an ambient
# DEFAULT_MODEL/DEFAULT_SIMULATION, which must not be able to unlock this gate.
echo -e "${BLUE}Step 0: Checking package pair...${NC}"
BASELINE_MODEL="$(make_default DEFAULT_MODEL sage16)"
BASELINE_SIMULATION="$(make_default DEFAULT_SIMULATION mini-millennium)"
if [ "$MODEL" != "$BASELINE_MODEL" ] || [ "$SIMULATION" != "$BASELINE_SIMULATION" ]; then
    echo -e "${RED}ERROR: The committed baseline belongs to MODEL=$BASELINE_MODEL SIMULATION=$BASELINE_SIMULATION${NC}"
    echo "Selected: MODEL=$MODEL SIMULATION=$SIMULATION"
    echo ""
    echo "The baseline comparison test skips for any other pair, so a baseline"
    echo "installed from this selection would never be validated. Re-run with the"
    echo "default pair, or leave MODEL/SIMULATION unset."
    exit 1
fi
echo -e "${GREEN}✓ Selected pair owns the committed baseline${NC}"
echo ""

# Step 1: Check mimic is compiled
echo -e "${BLUE}Step 1: Checking Mimic executable...${NC}"
if [ ! -f "$MIMIC_EXE" ]; then
    echo -e "${RED}ERROR: Mimic executable not found at $MIMIC_EXE${NC}"
    echo "Build it first with: make  # HDF5 is enabled by default"
    exit 1
fi
echo -e "${GREEN}✓ Mimic executable found${NC}"
echo ""

# Step 2: Check HDF5 support
echo -e "${BLUE}Step 2: Checking HDF5 support...${NC}"
# Run a quick test to see if HDF5 is compiled in
if ! "$MIMIC_EXE" --version 2>&1 | grep -qi "hdf5" && ! ldd "$MIMIC_EXE" 2>/dev/null | grep -q "hdf5"; then
    echo -e "${YELLOW}WARNING: Cannot verify HDF5 support from executable${NC}"
    echo "Assuming HDF5 is available. If next step fails, rebuild with: make clean && make (remove any USE-HDF5=no overrides)"
fi
echo -e "${GREEN}✓ HDF5 support appears available${NC}"
echo ""

# Step 3: Validate parameter file exists
echo -e "${BLUE}Step 3: Validating parameter file...${NC}"
python3 scripts/generate_test_inputs.py >/dev/null || {
    echo -e "${RED}ERROR: Failed to generate test input files${NC}"
    exit 1
}
if [ ! -f "$PARAM_FILE" ]; then
    echo -e "${RED}ERROR: Parameter file not found: $PARAM_FILE${NC}"
    echo "Expected generated parameter file: build/generated/test_inputs/$MODEL/$SIMULATION/core/test_hdf5.yaml"
    exit 1
fi

# Check that module lists are empty (physics-free mode).
if ! python3 - "$PARAM_FILE" <<'PY'
import sys
import yaml
from pathlib import Path

param_file = Path(sys.argv[1])
with open(param_file, "r") as handle:
    config = yaml.safe_load(handle) or {}

modules = config.get("modules", {}) or {}
active = {
    section: modules.get(section)
    for section in ("pre_timestep", "phases", "post_timestep")
    if modules.get(section)
}

if active:
    print(f"Active module configuration found: {active}")
    sys.exit(1)
PY
then
    echo -e "${RED}ERROR: Parameter file has modules enabled${NC}"
    echo ""
    echo "Baseline MUST be generated in physics-free mode"
    echo "The modules.pre_timestep, modules.phases, and modules.post_timestep lists must be empty."
    echo ""
    echo "Edit $PARAM_FILE and remove enabled modules before regenerating."
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

# Step 5: Run Mimic to generate baseline.
# The output directory persists between runs, so clear everything this script
# installs first. Otherwise a component the current build no longer emits would
# survive from an earlier run and be installed as if it were fresh.
echo -e "${BLUE}Step 5: Running Mimic to generate baseline...${NC}"
rm -rf "$OUTPUT_FILE" "$OUTPUT_MASTER" "$OUTPUT_METADATA" || {
    echo -e "${RED}ERROR: Could not clear previous output in $OUTPUT_DIR${NC}"
    echo "Refusing to run: a surviving file could be installed as if it were fresh."
    exit 1
}
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

# Step 6: Verify the whole output set exists before touching the baseline
echo -e "${BLUE}Step 6: Verifying output files...${NC}"
for output in "$OUTPUT_FILE" "$OUTPUT_MASTER" "$OUTPUT_SCHEMA"; do
    if [ ! -f "$output" ]; then
        echo -e "${RED}ERROR: Output file not created: $output${NC}"
        echo "Expected Mimic to write the shard, master, and metadata/ set"
        exit 1
    fi
    OUTPUT_SIZE=$(stat -f%z "$output" 2>/dev/null || stat -c%s "$output" 2>/dev/null)
    echo "Output file: $output ($OUTPUT_SIZE bytes)"
done
echo "Output metadata: $OUTPUT_METADATA ($(find "$OUTPUT_METADATA" -type f | wc -l | tr -d ' ') files)"
echo -e "${GREEN}✓ Output set created successfully${NC}"
echo ""

# Step 7: Backup existing baseline set (whatever part of it exists).
# Backups go OUTSIDE the baseline tree: tests/data/output/baseline/** is
# deliberately un-ignored (.gitignore), so a backup written beside the baseline
# would be picked up by the staging command this script prints.
echo -e "${BLUE}Step 7: Backing up existing baseline...${NC}"
BACKUP_STAMP="$(date +%Y%m%d_%H%M%S)"
if [ -z "$BACKUP_STAMP" ]; then
    echo -e "${RED}ERROR: Could not generate a backup timestamp${NC}"
    exit 1
fi
BACKUP_DIR="$REPO_ROOT/archive/baseline-backups/hdf5-${BACKUP_STAMP}"
BACKED_UP=0
for baseline in "$BASELINE_FILE" "$BASELINE_MASTER" "$BASELINE_METADATA"; do
    if [ -e "$baseline" ]; then
        mkdir -p "$BACKUP_DIR" || {
            echo -e "${RED}ERROR: Could not create backup directory: $BACKUP_DIR${NC}"
            exit 1
        }
        cp -R "$baseline" "$BACKUP_DIR/" || {
            echo -e "${RED}ERROR: Could not back up $baseline${NC}"
            echo "Refusing to overwrite a baseline that cannot be restored."
            exit 1
        }
        BACKED_UP=$((BACKED_UP + 1))
    fi
done
if [ "$BACKED_UP" -gt 0 ]; then
    echo "Backed up $BACKED_UP items to: $BACKUP_DIR"
    echo -e "${GREEN}✓ Existing baseline backed up${NC}"
    RESTORE_HINT="restore the copies in $BACKUP_DIR"
else
    echo "No existing baseline to backup"
    RESTORE_HINT="note there was no previous baseline to restore"
fi
echo ""

# Step 8: Install the complete set, so every committed baseline file describes
# the run that produced the records beside it (tests/data/README.md). Every copy
# is checked: Step 9 compares halo values and the two files' embedded layouts, so
# a half-installed metadata/ directory would survive it unnoticed.
echo -e "${BLUE}Step 8: Installing new baseline...${NC}"
install_baseline_path() {
    cp -R "$1" "$2" || {
        echo -e "${RED}ERROR: Could not install $2${NC}"
        echo "The baseline set is now PARTIAL - $RESTORE_HINT."
        exit 1
    }
}
rm -rf "$BASELINE_METADATA" || {
    echo -e "${RED}ERROR: Could not clear $BASELINE_METADATA${NC}"
    echo "Nothing was installed; the existing baseline is untouched."
    exit 1
}
install_baseline_path "$OUTPUT_FILE" "$BASELINE_FILE"
install_baseline_path "$OUTPUT_MASTER" "$BASELINE_MASTER"
install_baseline_path "$OUTPUT_METADATA" "$BASELINE_METADATA"
for baseline in "$BASELINE_FILE" "$BASELINE_MASTER"; do
    BASELINE_SIZE=$(stat -f%z "$baseline" 2>/dev/null || stat -c%s "$baseline" 2>/dev/null)
    echo "Baseline file: $baseline ($BASELINE_SIZE bytes)"
done
echo "Baseline metadata: $BASELINE_METADATA ($(find "$BASELINE_METADATA" -type f | wc -l | tr -d ' ') files)"
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
    print('${RED}✗ Could not run validation:${NC}')
    print(str(e))
    sys.exit(1)
"; then
    echo ""
else
    # An unvalidated baseline is worse than no baseline: it looks authoritative
    # and nothing checked it. Fail loudly and point at the backups.
    echo ""
    echo -e "${RED}ERROR: Baseline validation did not pass${NC}"
    echo "The new baseline is installed but UNVALIDATED. Investigate before"
    echo "committing it, or $RESTORE_HINT."
    echo ""
    cd "$REPO_ROOT" || exit 1
    exit 1
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
echo "   git diff --stat tests/data/output/baseline/hdf5/"
echo ""
echo "2. Verify tests pass:"
echo "   make tests-integration"
echo ""
echo "3. Commit the new baseline as one set (records plus their metadata):"
echo "   git add tests/data/output/baseline/hdf5/model_000.hdf5 \\"
echo "           tests/data/output/baseline/hdf5/model.hdf5 \\"
echo "           tests/data/output/baseline/hdf5/metadata/"
echo "   git status --short   # confirm nothing else was picked up"
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
