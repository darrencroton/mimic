#!/usr/bin/env python3
"""
sage26_determine_ffb_regime - Integration Test

Validates: SAGE26's feedback-free-burst classifier through real runs of mimic on the committed
test trees: it writes nothing with EnhancedStarFormationOn 0 and writes only its domain with
EnhancedStarFormationOn 1. The parameter refusals are in the C test
test_unit_sage26_parameter_refusals.c; the placement and cross-module refusals are in the C tests
of the module and test_unit_sage_dependency_contracts.c.

Every configuration sets every key its modules read, explicitly. The keys come from each configured
module's module_info.yaml `parameters` list, which `make lint-parameters` holds to the code,
with the vertical run file's values (models/sage26/input/sage26_mini-millennium.yaml), except
CGMrecipeOn, set to 0 because no CGM module is configured, so the star-formation modules take the
classic hot-gas routing. Runs use the run file's SubSteps (10) and write snapshots
63 and 16 (the FFB snapshot S).

Minimal pipeline (Off, On; EnhancedStarFormationOn 1 for On), in the run file's order:
  pre_timestep:   sage26_determine_ffb_regime
  galaxy_physics: sage26_ffb_star_formation, sage26_calculate_star_formation,
                  sage26_apply_star_formation_supernova
  - sage26_determine_ffb_regime: the module under test. Its reads Type, Mvir and MostBoundID are
    core and simulation halo properties.
  - sage26_ffb_star_formation: at EnhancedStarFormationOn 1 the module's guard requires it in a
    substep phase, by galaxy.
  - sage26_calculate_star_formation: the transaction's guard requires it immediately after.
  - sage26_apply_star_formation_supernova: sage26_calculate_star_formation refuses to run without
    it (sage26_calculate_star_formation.c, its init's dependency check).

Off is a no-op: with EnhancedStarFormationOn 0, the minimal pipeline with the module and the same
pipeline without it give byte-identical output in every field of every row of both snapshots.

On writes its domain (the observable list):
  - FFBRegime: in {0, 1} on every output row of both snapshots (range [0, 1], creation value 0;
    README.md, "Properties": "Writes: FFBRegime").
  - Witness: at least one row with FFBRegime 1 at snapshot S = 16.
  - Exceptions: none. FFBRegime is the only property the module writes.

Runs: the file launches three, one per distinct configuration (Off with the module, Off without
it, On).

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 models/sage26/modules/\\
sage26_determine_ffb_regime/_tests/test_integration_sage26_determine_ffb_regime.py
"""

import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    run_test_suite,
)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "_tests"))
from sage26_run_helpers import (  # noqa: E402
    PackageRuns,
    assert_runs,
    require_mimic,
    require_pair,
)

MODULE = "sage26_determine_ffb_regime"
TRANSACTION = "sage26_ffb_star_formation"
FH = "process_full_halo"
BG = "process_by_galaxy"

# The vertical run file's SubSteps and the snapshots written, with their binary output files on
# mini-millennium's committed test trees (16 is the FFB snapshot S, z = 7.272)
SUBSTEPS = 10
FFB_SNAPSHOT = 16
OUTPUT_FILES = {63: "model_z0.000_0", FFB_SNAPSHOT: "model_z7.272_0"}

# The vertical run file's values for every key the configurations below read, except CGMrecipeOn: 0,
# as no CGM module is configured
PARAMETER_VALUES = {
    "EnhancedStarFormationOn": 1,
    "EnhancedSFEfficiency": 0.2,
    "StarFormingDiskFactor": 3.0,
    "SFprescription": 1,
    "RecycleFraction": 0.43,
    "CGMrecipeOn": 0,
    "FIREmodeOn": 1,
    "FeedbackReheatingEpsilon": 2.9,
    "FeedbackEjectionEfficiency": 0.3,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
    "SfrEfficiency": 0.05,
    "H2RadialIntegrationOn": 1,
    "H2RadialNBins": 25,
    "H2RadialRMaxFactor": 5.0,
}

STAR_FORMATION = [
    (TRANSACTION, BG),
    ("sage26_calculate_star_formation", BG),
    ("sage26_apply_star_formation_supernova", BG),
]
MINIMAL = {
    "pre_timestep": [(MODULE, FH)],
    "galaxy_physics": STAR_FORMATION,
}
MINIMAL_WITHOUT = {"galaxy_physics": STAR_FORMATION}

# ============================================================================
# HELPERS
# ============================================================================


RUNS = PackageRuns(MODULE, PARAMETER_VALUES, OUTPUT_FILES, SUBSTEPS)
parameters_for = RUNS.parameters_for
mimic_run = RUNS.mimic_run
load_snapshot = RUNS.load_snapshot
assert_outputs_identical = RUNS.assert_outputs_identical


# ============================================================================
# TEST CASES
# ============================================================================


def test_off_is_noop():
    """At EnhancedStarFormationOn 0 the output equals the same pipeline's without the module."""
    require_pair()
    require_mimic()
    off = {"EnhancedStarFormationOn": 0}
    with mimic_run(f"{MODULE}_off_with", MINIMAL, parameters_for(MINIMAL, off)) as with_module:
        returncode, stdout, stderr, dir_with = with_module
        assert_runs(returncode, stdout, stderr)
        params = parameters_for(MINIMAL_WITHOUT, off)
        with mimic_run(f"{MODULE}_off_without", MINIMAL_WITHOUT, params) as without:
            returncode, stdout, stderr, dir_without = without
            assert_runs(returncode, stdout, stderr)
            assert_outputs_identical(dir_with, dir_without)


def test_on_writes_domain():
    """At EnhancedStarFormationOn 1 every FFBRegime is 0 or 1; snapshot S has an FFB row."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_on", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        for snapshot in OUTPUT_FILES:
            halos = load_snapshot(output_dir, snapshot)
            outside = int((~np.isin(halos["FFBRegime"], (0, 1))).sum())
            assert outside == 0, f"snapshot {snapshot}: {outside} rows with FFBRegime not 0 or 1"
            ffb = int((halos["FFBRegime"] == 1).sum())
            print(f"  snapshot {snapshot}: {len(halos)} rows in domain, FFBRegime 1: {ffb}")
            if snapshot == FFB_SNAPSHOT:
                assert ffb > 0, f"witness: no FFBRegime 1 row at snapshot {FFB_SNAPSHOT}"


def main():
    return run_test_suite([test_off_is_noop, test_on_writes_domain], f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
