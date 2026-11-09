#!/usr/bin/env python3
"""
sage26_regime_aware_cooling - Integration Test

Validates: SAGE26's regime-aware cooling transaction through real runs of mimic on the committed
test trees: it loads and runs, writes nothing with CGMrecipeOn 0, writes only its domain with
CGMrecipeOn 1, and refuses a missing required key and a misplacement at the executable's exit
status.

Every configuration sets every key its modules read, explicitly. The keys come from each configured
module's module_info.yaml `parameters` list, which `make lint-parameters` holds to the code, with
the vertical run file's values (models/sage26/input/sage26_mini-millennium.yaml). Runs use the run
file's SubSteps (10) and write snapshots 63 and 16.

Minimal pipeline (Loads and runs, Off; CGMrecipeOn 1 for Loads and runs), in the run file's order:
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

On writes its domain: with CGMrecipeOn 1, on the vertical run file's full pipeline, built from the
run file's modules and order with every key they read set explicitly. The minimal pipeline has no
stripping or merger pass, so Type 2 orphans with Mvir 0 keep CGM gas, and SAGE26's CGM recipe gives
them tff = +inf by design: g_accel = G * Mvir / Rvir^2 is 0, so tff = sqrt(2 Rvir / g_accel) is
infinite. That reachable configuration is outside the domain below, and the full pipeline gives
no such row. The following hold on every output row of both snapshots:
  - HotGas, MetalsHotGas, CGMgas, MetalsCGMgas, ColdGas, MetalsColdGas, BlackHoleMass: finite and
    non-negative (masses; range [0.0, ...] with creation value 0).
  - CoolingRate: finite and non-negative (the applied cooled mass over the substep, a mass rate,
    creation value 0; the transaction's total of two masses each capped to a non-negative
    reservoir, sage26_regime_aware_cooling.c).
  - Rheat: finite and non-negative (range [0.0, 10.0], creation value 0).
  - tcool, tff, RcoolToRvir: finite and either non-negative or exactly the documented sentinel -1
    (creation value and sentinel -1; the diagnostic reset writes tcool 0, tff -1 and RcoolToRvir -1,
    and the hot restoration writes tff -1, by design: sage26_regime_aware_cooling.c,
    reset_cgm_diagnostics and the restoration).
  - mdot_cool, mdot_stream: finite and non-negative, with no witness (rates, creation value 0), since
    the full pipeline includes sage26_finalize_cooling_heating, which writes them from the raw
    rates; the test trees need not reach the cold streams.
  - Witness: at least one Regime 0 row (the CGM branch only this module computes) with non-zero
    CoolingRate at snapshot 63.
  - Excluded: mdot_cool_raw and mdot_stream_raw, package-private state that binary output never
    writes (output: false); Cooling and Heating, which sage26_finalize_cooling_heating finalises
    after this module's accumulation.

Required-key refusal, isolated (a real run; mimic exits non-zero with "Required model parameter
'MShockMsun' not found" and "Module 'sage26_regime_aware_cooling' initialization failed", and no
other module's initialization failure):
  - MShockMsun, with CGMrecipeOn 0 and no sage26_determine_regime. Configuration: pre_timestep
    sage26_prepare_infall_budget; galaxy_physics sage26_apply_infall,
    sage26_calculate_cooling_budget, sage26_apply_cooling, sage26_regime_aware_cooling. The
    transaction is the only configured reader of MShockMsun (asserted from the modules'
    parameters lists). Control: the same configuration with the key present exits 0.

Placement refusal: with CGMrecipeOn 1, the minimal pipeline with the module immediately before
sage26_apply_cooling instead of after it, every key present, is refused naming the module
(validate_placement: immediately after sage26_apply_cooling, by galaxy, in the same phase).

These run-level cases repeat, at the executable's exit status, refusals the module's C unit tests
assert; they test no new behaviour.

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 models/sage26/modules/\\
sage26_regime_aware_cooling/_tests/test_integration_sage26_regime_aware_cooling.py
"""

import sys
from pathlib import Path

import numpy as np
import yaml

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (  # noqa: E402
    run_test_suite,
)

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "_tests"))
from sage26_run_helpers import (  # noqa: E402
    PackageRuns,
    assert_runs,
    configured_modules,
    module_parameters,
    require_mimic,
    require_pair,
)

MODULE = "sage26_regime_aware_cooling"
MODULES_DIR = Path(__file__).resolve().parents[2]
RUN_FILE = MODULES_DIR.parent / "input" / "sage26_mini-millennium.yaml"
FH = "process_full_halo"
BG = "process_by_galaxy"

# The vertical run file's SubSteps and the snapshots written, with their binary output files on
# mini-millennium's committed test trees (16 is the FFB snapshot S, z = 7.272)
SUBSTEPS = 10
OUTPUT_FILES = {63: "model_z0.000_0", 16: "model_z7.272_0"}

# The vertical run file's values for every key the configurations below read; the full pipeline
# reads every key the run file sets
PARAMETER_VALUES = {
    "GlobalBaryonFraction": 0.17,
    "CGMrecipeOn": 1,
    "MShockMsun": 6.0e11,
    "EnhancedStarFormationOn": 1,
    "EnhancedSFEfficiency": 0.2,
    "ConcentrationOn": 3,
    "SfrEfficiency": 0.05,
    "StarFormingDiskFactor": 3.0,
    "SFprescription": 1,
    "H2RadialIntegrationOn": 1,
    "H2RadialNBins": 25,
    "H2RadialRMaxFactor": 5.0,
    "StarburstColdGasOn": 1,
    "FeedbackReheatingEpsilon": 2.9,
    "FeedbackEjectionEfficiency": 0.3,
    "FIREmodeOn": 1,
    "ReIncorporationFactor": 0.19,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "BlackHoleGrowthRate": 0.015,
    "QuasarModeEfficiency": 0.005,
    "RecycleFraction": 0.43,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
    "ThresholdMajorMerger": 0.3,
    "ThresholdSatDisruption": 1.0,
    "MergerTimeFactor": 2.0,
    "BulgeSizeOn": 3,
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
KEY_ISOLATED = {
    "pre_timestep": [("sage26_prepare_infall_budget", FH)],
    "galaxy_physics": MINIMAL["galaxy_physics"],
}
MISPLACED = {
    "pre_timestep": MINIMAL["pre_timestep"],
    "galaxy_physics": [
        ("sage26_apply_infall", FH),
        ("sage26_calculate_cooling_budget", BG),
        (MODULE, BG),
        ("sage26_apply_cooling", BG),
    ],
}

NON_NEGATIVE = (
    "HotGas",
    "MetalsHotGas",
    "CGMgas",
    "MetalsCGMgas",
    "ColdGas",
    "MetalsColdGas",
    "BlackHoleMass",
    "CoolingRate",
    "Rheat",
    "mdot_cool",
    "mdot_stream",
)
NON_NEGATIVE_OR_SENTINEL = ("tcool", "tff", "RcoolToRvir")

# ============================================================================
# HELPERS
# ============================================================================


RUNS = PackageRuns(MODULE, PARAMETER_VALUES, OUTPUT_FILES, SUBSTEPS)
parameters_for = RUNS.parameters_for
mimic_run = RUNS.mimic_run
load_snapshot = RUNS.load_snapshot
assert_outputs_identical = RUNS.assert_outputs_identical
assert_refused = RUNS.assert_refused


def run_file_pipeline():
    """The vertical run file's modules, in its phases and order."""
    with open(RUN_FILE, encoding="utf-8") as handle:
        modules = yaml.safe_load(handle)["modules"]

    def entries(items):
        return [next(iter(entry.items())) for entry in items or []]

    phases = {"pre_timestep": entries(modules.get("pre_timestep"))}
    for name, items in (modules.get("phases") or {}).items():
        phases[name] = entries(items)
    phases["post_timestep"] = entries(modules.get("post_timestep"))
    return phases


# ============================================================================
# TEST CASES
# ============================================================================


def test_loads_and_runs():
    """The minimal pipeline with CGMrecipeOn 1 runs to completion, exit 0, no leak report."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_loads", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        assert "SAGE regime-aware cooling module initialized" in stdout, "no initialization log"
        assert "SAGE regime-aware cooling module cleaned up" in stdout, "no cleanup log"
        for snapshot in OUTPUT_FILES:
            load_snapshot(output_dir, snapshot)
    print(f"  exit status 0, no leak report, snapshots {list(OUTPUT_FILES)} written")


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


def test_on_writes_domain():
    """On the run file's full pipeline every observable lies in its cited domain."""
    require_pair()
    require_mimic()
    full = run_file_pipeline()
    assert MODULE in configured_modules(full), f"{MODULE} is not in the run file's pipeline"
    assert "sage26_finalize_cooling_heating" in configured_modules(full)
    params = parameters_for(full)
    assert params["CGMrecipeOn"] == 1, "the domain case runs with CGMrecipeOn 1"
    with mimic_run(f"{MODULE}_on", full, params) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        for snapshot in OUTPUT_FILES:
            halos = load_snapshot(output_dir, snapshot)
            for prop in NON_NEGATIVE:
                values = halos[prop]
                bad = int((~np.isfinite(values) | (values < 0)).sum())
                assert bad == 0, f"snapshot {snapshot}: {bad} rows with {prop} not finite and >= 0"
            for prop in NON_NEGATIVE_OR_SENTINEL:
                values = halos[prop]
                valid = np.isfinite(values) & ((values >= 0) | (values == -1.0))
                bad = int((~valid).sum())
                assert bad == 0, f"snapshot {snapshot}: {bad} rows with {prop} not >= 0 or -1"
            cgm = halos["Regime"] == 0
            witnesses = int((cgm & (halos["CoolingRate"] != 0)).sum())
            print(
                f"  snapshot {snapshot}: {len(halos)} rows in domain; Regime 0 rows: "
                f"{int(cgm.sum())}, with non-zero CoolingRate: {witnesses}"
            )
            if snapshot == 63:
                assert witnesses > 0, "witness: no Regime 0 row with non-zero CoolingRate at 63"


def test_required_key_mshockmsun_refused():
    """A missing MShockMsun is refused by this module alone; the control exits 0."""
    require_pair()
    require_mimic()
    key = "MShockMsun"
    readers = [name for name in configured_modules(KEY_ISOLATED) if key in module_parameters(name)]
    assert readers == [MODULE], f"configured readers of {key}: {readers}"
    off = {"CGMrecipeOn": 0}

    with mimic_run(f"{MODULE}_key_control", KEY_ISOLATED, parameters_for(KEY_ISOLATED, off)) as r:
        returncode, stdout, stderr, _ = r
        assert_runs(returncode, stdout, stderr)
        print(f"  control ({key} present): exit status {returncode}")

    params = parameters_for(KEY_ISOLATED, off, omit=(key,))
    with mimic_run(f"{MODULE}_key_missing", KEY_ISOLATED, params) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [
                f"Required model parameter '{key}' not found",
                f"Module '{MODULE}' initialization failed",
            ],
        )


def test_placement_refused():
    """With CGMrecipeOn 1, an entry before sage26_apply_cooling is refused, naming the module."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_misplaced", MISPLACED, parameters_for(MISPLACED)) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [f"{MODULE} must run immediately after sage26_apply_cooling"],
        )


def main():
    tests = [
        test_loads_and_runs,
        test_off_is_noop,
        test_on_writes_domain,
        test_required_key_mshockmsun_refused,
        test_placement_refused,
    ]
    return run_test_suite(tests, f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
