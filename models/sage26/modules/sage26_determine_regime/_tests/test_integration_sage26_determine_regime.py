#!/usr/bin/env python3
"""
sage26_determine_regime - Integration Test

Validates: the CGM regime classifier through real runs of mimic on the committed test trees:
it loads and runs, writes nothing with CGMrecipeOn 0, writes only its domain with CGMrecipeOn 1,
and refuses a missing required key and a misplacement at the executable's exit status.

Every configuration sets every key its modules read, explicitly. The keys come from each configured
module's module_info.yaml `parameters` list, which `make lint-parameters` holds to the code, with
the vertical run file's values (models/sage26/input/sage26_mini-millennium.yaml). Runs use the run
file's SubSteps (10) and write snapshots 63 and 16.

Minimal pipeline (Loads and runs, Off, On; CGMrecipeOn 1 for On):
  pre_timestep: sage26_determine_regime
  - sage26_determine_regime: the module under test. Its guards require nothing else: the
    placement check orders it before sage26_prepare_infall_budget only when that module is
    configured (sage26_determine_regime.c, validate_placement). Its reads, Type, Mvir and
    UniqueGalaxyID, are core properties, so no other module is needed to supply them.

Off is a no-op: with CGMrecipeOn 0, the pipeline with the module and the empty pipeline give
byte-identical output in every field of every row of both snapshots.

On writes its domain (the observable list):
  - Regime: in {0, 1} on every output row of both snapshots. Domain: the module classifies every
    live row 0 or 1 and leaves only Type 3 rows, which are never output (README.md, "Rows" and
    "Properties": "Writes: Regime"); the property's range is [-1, 1] with -1 the creation value
    and "not yet assigned" sentinel, so -1 on an
    output row would be a row the classifier missed.
  - Witness: at least one row with Regime 1 and at least one with Regime 0 at snapshot 63.
  - Exceptions: none. Regime is the only property the module writes.

Required-key refusals, isolated (a real run; mimic exits non-zero with "Required model parameter
'<key>' not found" and "Module 'sage26_determine_regime' initialization failed", and no other
module's initialization failure):
  - MShockMsun, and separately CGMrecipeOn. Configuration: sage26_determine_regime alone, so no
    other configured module reads either key (sage26_regime_aware_cooling and the ten other
    CGMrecipeOn readers are absent; the test asserts this from the modules' parameters lists).
    Control: the same configuration with both keys present (CGMrecipeOn 1) exits 0.

Placement refusal: sage26_determine_regime in a substep phase (galaxy_physics, process_full_halo),
every key present, is refused naming the module (validate_placement: exactly one pre_timestep
entry, none in a substep phase or post_timestep).

These run-level cases repeat, at the executable's exit status, refusals the module's C unit tests
assert; they test no new behaviour.

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 \\
    models/sage26/modules/sage26_determine_regime/_tests/test_integration_sage26_determine_regime.py
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
    configured_modules,
    module_parameters,
    require_mimic,
    require_pair,
)

MODULE = "sage26_determine_regime"
FH = "process_full_halo"

# The vertical run file's SubSteps and the snapshots written, with their binary output files on
# mini-millennium's committed test trees (16 is the FFB snapshot S, z = 7.272)
SUBSTEPS = 10
OUTPUT_FILES = {63: "model_z0.000_0", 16: "model_z7.272_0"}

# The vertical run file's values for every key the configurations below read
PARAMETER_VALUES = {
    "CGMrecipeOn": 1,
    "MShockMsun": 6.0e11,
}

MINIMAL = {"pre_timestep": [(MODULE, FH)]}
EMPTY = {}
MISPLACED = {"galaxy_physics": [(MODULE, FH)]}

# ============================================================================
# HELPERS
# ============================================================================


RUNS = PackageRuns(MODULE, PARAMETER_VALUES, OUTPUT_FILES, SUBSTEPS)
parameters_for = RUNS.parameters_for
mimic_run = RUNS.mimic_run
load_snapshot = RUNS.load_snapshot
assert_outputs_identical = RUNS.assert_outputs_identical
assert_refused = RUNS.assert_refused


def check_required_key(key):
    """The control (key present, exit 0), then the refusal (key missing)."""
    readers = [name for name in configured_modules(MINIMAL) if key in module_parameters(name)]
    assert readers == [MODULE], f"configured readers of {key}: {readers}"

    with mimic_run(f"{MODULE}_{key}_control", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, _ = run
        assert_runs(returncode, stdout, stderr)
        print(f"  control ({key} present): exit status {returncode}")

    with mimic_run(f"{MODULE}_{key}_missing", MINIMAL, parameters_for(MINIMAL, omit=(key,))) as run:
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
        assert "SAGE determine regime module initialized" in stdout, "no initialization log"
        assert "SAGE determine regime module cleaned up" in stdout, "no cleanup log"
        for snapshot in OUTPUT_FILES:
            load_snapshot(output_dir, snapshot)
    print(f"  exit status 0, no leak report, snapshots {list(OUTPUT_FILES)} written")


def test_off_is_noop():
    """With CGMrecipeOn 0 the output equals the empty pipeline's in every field of every row."""
    require_pair()
    require_mimic()
    params = parameters_for(MINIMAL, overrides={"CGMrecipeOn": 0})
    with mimic_run(f"{MODULE}_off_with", MINIMAL, params) as with_module:
        returncode, stdout, stderr, dir_with = with_module
        assert_runs(returncode, stdout, stderr)
        with mimic_run(f"{MODULE}_off_without", EMPTY, parameters_for(EMPTY)) as without:
            returncode, stdout, stderr, dir_without = without
            assert_runs(returncode, stdout, stderr)
            assert_outputs_identical(dir_with, dir_without)


def test_on_writes_domain():
    """With CGMrecipeOn 1 every output row's Regime is 0 or 1; both values occur at 63."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_on", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        for snapshot in OUTPUT_FILES:
            halos = load_snapshot(output_dir, snapshot)
            outside = int((~np.isin(halos["Regime"], (0, 1))).sum())
            assert outside == 0, f"snapshot {snapshot}: {outside} rows with Regime not in {{0, 1}}"
            ones = int((halos["Regime"] == 1).sum())
            zeros = int((halos["Regime"] == 0).sum())
            print(f"  snapshot {snapshot}: {len(halos)} rows, Regime 0: {zeros}, Regime 1: {ones}")
            if snapshot == 63:
                assert ones > 0, "witness: no Regime 1 row at snapshot 63"
                assert zeros > 0, "witness: no Regime 0 row at snapshot 63"


def test_required_key_mshockmsun_refused():
    """A missing MShockMsun is refused by this module alone; the control exits 0."""
    require_pair()
    require_mimic()
    check_required_key("MShockMsun")


def test_required_key_cgmrecipeon_refused():
    """A missing CGMrecipeOn is refused by this module alone; the control exits 0."""
    require_pair()
    require_mimic()
    check_required_key("CGMrecipeOn")


def test_placement_refused():
    """An entry in a substep phase is refused, naming the module."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_misplaced", MISPLACED, parameters_for(MISPLACED)) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [f"{MODULE} must be configured exactly once, in pre_timestep"],
        )


def main():
    tests = [
        test_loads_and_runs,
        test_off_is_noop,
        test_on_writes_domain,
        test_required_key_mshockmsun_refused,
        test_required_key_cgmrecipeon_refused,
        test_placement_refused,
    ]
    return run_test_suite(tests, f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
