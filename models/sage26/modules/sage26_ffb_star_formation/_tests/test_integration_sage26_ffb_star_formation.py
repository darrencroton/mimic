#!/usr/bin/env python3
"""
sage26_ffb_star_formation - Integration Test

Validates: SAGE26's feedback-free-burst star-formation transaction through real runs of mimic on
the committed test trees: it writes nothing with EnhancedStarFormationOn 0, writes only its domain
on the FFBRegime 1 rows with EnhancedStarFormationOn 1. The parameter refusals are in the C test
test_unit_sage26_parameter_refusals.c; the placement and cross-module refusals are in the C tests
of the module and test_unit_sage_dependency_contracts.c.

Every configuration sets every key its modules read, explicitly. The keys come from each configured
module's module_info.yaml `parameters` list, which `make lint-parameters` holds to the code,
with the vertical run file's values (models/sage26/input/sage26_mini-millennium.yaml), except
CGMrecipeOn, set to 0 because no CGM module is configured, so infall, cooling and feedback take the
classic hot-gas routing. Runs use the run file's SubSteps (10) and write snapshots
63 and 16 (the FFB snapshot S).

Minimal pipeline (Off, On; EnhancedStarFormationOn 1 for On), in the run file's
order:
  pre_timestep:   sage26_set_disk_scale_radius, sage26_prepare_infall_budget,
                  sage26_determine_ffb_regime
  galaxy_physics: sage26_apply_infall, sage26_calculate_cooling_budget, sage26_apply_cooling,
                  sage26_ffb_star_formation, sage26_calculate_star_formation,
                  sage26_apply_star_formation_supernova
  - sage26_ffb_star_formation: the module under test.
  - sage26_determine_ffb_regime: at EnhancedStarFormationOn 1 the module's guard requires it in
    pre_timestep; it writes FFBRegime, which selects the rows the transaction acts on.
  - sage26_calculate_star_formation: the module's guard requires it immediately after.
  - sage26_apply_star_formation_supernova: sage26_calculate_star_formation refuses to run without
    it (sage26_calculate_star_formation.c, its init's dependency check).
  - sage26_set_disk_scale_radius: writes DiskScaleRadius, which the transaction reads for its
    dynamical time; without it the time is 0 and no star forms.
  - sage26_prepare_infall_budget, sage26_apply_infall, sage26_calculate_cooling_budget and
    sage26_apply_cooling: they supply the ColdGas the transaction forms stars from (each guard
    requires the module before it: the infall budget for the infall, the cooling budget for the
    cooling). sage26_reionization is optional to the budget and is left out.

Off is a no-op: with EnhancedStarFormationOn 0, the minimal pipeline with the module and the same
pipeline without it give byte-identical output in every field of every row of both snapshots.

On writes its domain (the observable list), with EnhancedStarFormationOn 1:
  - On the FFBRegime 1 rows at snapshot S = 16, each finite and non-negative: ColdGas,
    MetalsColdGas, StellarMass, MetalsStellarMass, H1gas, StarFormationRate and
    SupernovaOutflowRate (the last written by the shared feedback commit the transaction calls).
    Domains: each is a mass or rate, with range [0.0, ...] and creation value 0.
  - Witnesses: at least one FFBRegime 1 row with StarFormationRate > 0, and at least one with
    SupernovaOutflowRate > 0 (the feedback commit's outflow accumulation ran).
  - H2gas: exactly 0 on every FFBRegime 1 row at S. The transaction writes 0.0f, its creation value,
    and sage26_calculate_star_formation returns at once on an FFBRegime 1 row.
  - MassLoading: finite and non-negative on the FFBRegime 1 rows at S, with no witness. It is
    written only with a valid FIRE scaling and otherwise left stale.
  - The FoF central's reservoirs that feedback credits, HotGas, MetalsHotGas, CGMgas, MetalsCGMgas,
    EjectedGas and MetalsEjectedGas: finite and non-negative on every row at S, since other modules
    write them too.
  - Excluded: SfrDisk and SfrDiskZ, which sage26_finalize_sfr_histories writes from the non-output
    history bins; that module is not in the pipeline. The transaction's carried values (the stars
    formed, the pre-star-formation metallicity, the renormalised stars, the reheated and ejected
    masses) are locals of the call and never output.

Runs: the file launches three, one per distinct configuration (Off with the module, Off without
it, On).

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 models/sage26/modules/\\
sage26_ffb_star_formation/_tests/test_integration_sage26_ffb_star_formation.py
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

MODULE = "sage26_ffb_star_formation"
CLASSIFIER = "sage26_determine_ffb_regime"
ORDINARY_SF = "sage26_calculate_star_formation"
APPLY_SF = "sage26_apply_star_formation_supernova"
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
    "GlobalBaryonFraction": 0.17,
    "BulgeSizeOn": 3,
}

SUPPLY_PRE = [
    ("sage26_set_disk_scale_radius", FH),
    ("sage26_prepare_infall_budget", FH),
]
SUPPLY_PHYSICS = [
    ("sage26_apply_infall", FH),
    ("sage26_calculate_cooling_budget", BG),
    ("sage26_apply_cooling", BG),
]
MINIMAL = {
    "pre_timestep": SUPPLY_PRE + [(CLASSIFIER, FH)],
    "galaxy_physics": SUPPLY_PHYSICS + [(MODULE, BG), (ORDINARY_SF, BG), (APPLY_SF, BG)],
}
MINIMAL_WITHOUT = {
    "pre_timestep": MINIMAL["pre_timestep"],
    "galaxy_physics": SUPPLY_PHYSICS + [(ORDINARY_SF, BG), (APPLY_SF, BG)],
}
FFB_ROW_NON_NEGATIVE = (
    "ColdGas",
    "MetalsColdGas",
    "StellarMass",
    "MetalsStellarMass",
    "H1gas",
    "StarFormationRate",
    "SupernovaOutflowRate",
    "MassLoading",
)
CENTRAL_RESERVOIRS = (
    "HotGas",
    "MetalsHotGas",
    "CGMgas",
    "MetalsCGMgas",
    "EjectedGas",
    "MetalsEjectedGas",
)

# ============================================================================
# HELPERS
# ============================================================================


RUNS = PackageRuns(MODULE, PARAMETER_VALUES, OUTPUT_FILES, SUBSTEPS)
parameters_for = RUNS.parameters_for
mimic_run = RUNS.mimic_run
load_snapshot = RUNS.load_snapshot
assert_outputs_identical = RUNS.assert_outputs_identical


def assert_finite_non_negative(rows, props, where):
    for prop in props:
        values = rows[prop]
        bad = int((~np.isfinite(values) | (values < 0)).sum())
        assert bad == 0, f"{where}: {bad} rows with {prop} not finite and >= 0"


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
    """At EnhancedStarFormationOn 1 the FFB rows at S hold their domains and the witnesses."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_on", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        halos = load_snapshot(output_dir, FFB_SNAPSHOT)
        ffb = halos[halos["FFBRegime"] == 1]
        where = f"snapshot {FFB_SNAPSHOT}"
        assert len(ffb) > 0, f"{where}: no FFBRegime 1 row, so the transaction never acted"
        assert_finite_non_negative(ffb, FFB_ROW_NON_NEGATIVE, f"{where}, FFBRegime 1 rows")
        h2_nonzero = int((ffb["H2gas"] != 0).sum())
        assert h2_nonzero == 0, f"{where}: {h2_nonzero} FFBRegime 1 rows with H2gas not 0"
        assert_finite_non_negative(halos, CENTRAL_RESERVOIRS, f"{where}, every row")
        forming = int((ffb["StarFormationRate"] > 0).sum())
        outflowing = int((ffb["SupernovaOutflowRate"] > 0).sum())
        print(
            f"  {where}: {len(halos)} rows, FFBRegime 1: {len(ffb)}, with StarFormationRate > 0: "
            f"{forming}, with SupernovaOutflowRate > 0: {outflowing}, H2gas 0 on all"
        )
        assert forming > 0, f"witness: no FFBRegime 1 row with StarFormationRate > 0 at {where}"
        assert (
            outflowing > 0
        ), f"witness: no FFBRegime 1 row with SupernovaOutflowRate > 0 at {where}"


def main():
    return run_test_suite([test_off_is_noop, test_on_writes_domain], f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
