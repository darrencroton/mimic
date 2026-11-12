#!/usr/bin/env python3
"""
sage26_regime_aware_cooling - Integration Test

Validates: that SAGE26's regime-aware cooling transaction writes nothing with CGMrecipeOn 0, through
real runs of mimic on the committed test trees. Its CGMrecipeOn 1 output, on the run file's full
pipeline, is covered by the physics baseline (test_scientific_sage26_physics_baseline.py).

Every configuration sets every key its modules read, explicitly. The keys come from each configured
module's module_info.yaml `parameters` list, which `make lint-parameters` holds to the code, with
the vertical run file's values (models/sage26/input/sage26_mini-millennium.yaml). Runs use the run
file's SubSteps (10) and write snapshots 63 and 16.

Minimal pipeline (Off), in the run file's order:
  pre_timestep:   sage26_determine_regime, sage26_prepare_infall_budget
  galaxy_physics: sage26_apply_infall, sage26_calculate_cooling_budget, sage26_apply_cooling,
                  sage26_regime_aware_cooling
  - sage26_regime_aware_cooling: the module under test.
  - sage26_apply_cooling: the module's placement guard requires it immediately before
    (validate_placement, with CGMrecipeOn 1).
  - sage26_calculate_cooling_budget: sage26_apply_cooling's guard requires it earlier in the same
    phase; its init loads the cooling tables, the only load of them (cooling_tables_init), which
    the module's hot and CGM recipes read.
  - sage26_determine_regime: with CGMrecipeOn 1 sage26_calculate_cooling_budget's guard requires
    it in pre_timestep; it writes Regime, which selects this module's CGM or hot branch.
  - sage26_prepare_infall_budget and sage26_apply_infall: they supply the HotGas and CGMgas the
    module cools (sage26_apply_infall's guard requires sage26_prepare_infall_budget). Without them
    every reservoir is 0 and nothing cools. sage26_reionization is optional to the budget and
    is left out.

Off is a no-op: with CGMrecipeOn 0, the minimal pipeline with the module and the same pipeline
without it give byte-identical output in every field of every row of both snapshots.

The required-parameter refusals and the placement refusals are asserted by the module's C unit
tests, which reach each branch directly.

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 models/sage26/modules/\\
sage26_regime_aware_cooling/_tests/test_integration_sage26_regime_aware_cooling.py
"""

import sys
from pathlib import Path

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

MODULE = "sage26_regime_aware_cooling"
FH = "process_full_halo"
BG = "process_by_galaxy"

# The vertical run file's SubSteps and the snapshots written, with their binary output files on
# mini-millennium's committed test trees (16 is the FFB snapshot S, z = 7.272)
SUBSTEPS = 10
OUTPUT_FILES = {63: "model_z0.000_0", 16: "model_z7.272_0"}

# The vertical run file's values for every key the configurations below read
PARAMETER_VALUES = {
    "GlobalBaryonFraction": 0.17,
    "CGMrecipeOn": 1,
    "MShockMsun": 6.0e11,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
}

MINIMAL = {
    "pre_timestep": [
        ("sage26_determine_regime", FH),
        ("sage26_prepare_infall_budget", FH),
    ],
    "galaxy_physics": [
        ("sage26_apply_infall", FH),
        ("sage26_calculate_cooling_budget", BG),
        ("sage26_apply_cooling", BG),
        (MODULE, BG),
    ],
}
MINIMAL_WITHOUT = {
    "pre_timestep": MINIMAL["pre_timestep"],
    "galaxy_physics": MINIMAL["galaxy_physics"][:-1],
}

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
    """With CGMrecipeOn 0 the output equals the same pipeline's without the module."""
    require_pair()
    require_mimic()
    off = {"CGMrecipeOn": 0}
    with mimic_run(f"{MODULE}_off_with", MINIMAL, parameters_for(MINIMAL, off)) as with_module:
        returncode, stdout, stderr, dir_with = with_module
        assert_runs(returncode, stdout, stderr)
        params = parameters_for(MINIMAL_WITHOUT, off)
        with mimic_run(f"{MODULE}_off_without", MINIMAL_WITHOUT, params) as without:
            returncode, stdout, stderr, dir_without = without
            assert_runs(returncode, stdout, stderr)
            assert_outputs_identical(dir_with, dir_without)


def main():
    return run_test_suite([test_off_is_noop], f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
