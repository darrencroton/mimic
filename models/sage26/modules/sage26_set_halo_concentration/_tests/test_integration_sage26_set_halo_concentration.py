#!/usr/bin/env python3
"""
sage26_set_halo_concentration - Integration Test

Validates: SAGE26's halo-concentration assignment at progenitor joining through real runs of
mimic on the committed test trees: it loads and runs, writes nothing with ConcentrationOn 0,
writes only its domain with ConcentrationOn 3, and refuses a missing required key and a
misplacement at the executable's exit status.

Every configuration sets every key its modules read, explicitly. The keys come from each configured
module's module_info.yaml `parameters` list, which `make lint-parameters` holds to the code, with
the vertical run file's values (models/sage26/input/sage26_mini-millennium.yaml). Runs use the run
file's SubSteps (10) and write snapshots 63 and 16.

Minimal pipeline (Loads and runs, Off, On; ConcentrationOn 3 for Loads and runs and On), in the
run file's order:
  pre_timestep:  sage26_set_halo_concentration
  post_timestep: sage26_record_transition_state
  - sage26_set_halo_concentration: the module under test. Of its reads, Type, Vvir, infallVmax
    and infallVvir are core properties and Vmax is the simulation package's halo property.
  - sage26_record_transition_state: with ConcentrationOn 3 the module's guard requires it in
    post_timestep (check_recorder); it writes PreviousType, which selects the velocity pair
    (without it every galaxy holds -1 and no central is assigned).

Off is a no-op: with ConcentrationOn 0, the minimal pipeline with the module and the same pipeline
without it (sage26_record_transition_state alone) give byte-identical output in every field of
every row of both snapshots.

On writes its domain (the observable list):
  - Concentration: finite and non-negative on every output row of both snapshots. Domain: the
    NFW concentration of SAGE26's Vmax/Vvir inversion, 0 if (Vmax/Vvir)^2 < 1, with creation value
    0, and every other row keeps its value (models/sage26/model_properties.yaml:1371-1378; README.md,
    "Processing Contract", the assignment table).
  - Witness: at least one Type 0 row with Concentration > 0 at snapshot 63.
  - Exceptions: none. Concentration is the only property the module writes.

Required-key refusal, isolated (a real run; mimic exits non-zero with "Required model parameter
'ConcentrationOn' not found" and "Module 'sage26_set_halo_concentration' initialization failed",
and no other module's initialization failure):
  - ConcentrationOn. Configuration: the minimal pipeline, in which only this module reads the key
    (asserted from the modules' parameters lists). Control: the same configuration with
    ConcentrationOn 3 exits 0.

Placement refusal: the module in a substep phase (galaxy_physics, process_full_halo) with
sage26_record_transition_state in post_timestep, ConcentrationOn 3, every key present, is refused
naming the module (check_placement: exactly one pre_timestep entry and none elsewhere).

These run-level cases repeat, at the executable's exit status, refusals the module's C unit tests
assert; they test no new behaviour.

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 models/sage26/modules/\\
sage26_set_halo_concentration/_tests/test_integration_sage26_set_halo_concentration.py
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

MODULE = "sage26_set_halo_concentration"
RECORDER = "sage26_record_transition_state"
FH = "process_full_halo"

# The vertical run file's SubSteps and the snapshots written, with their binary output files on
# mini-millennium's committed test trees (16 is the FFB snapshot S, z = 7.272)
SUBSTEPS = 10
OUTPUT_FILES = {63: "model_z0.000_0", 16: "model_z7.272_0"}

# The vertical run file's values for every key the configurations below read
PARAMETER_VALUES = {
    "ConcentrationOn": 3,
}

MINIMAL = {
    "pre_timestep": [(MODULE, FH)],
    "post_timestep": [(RECORDER, FH)],
}
MINIMAL_WITHOUT = {"post_timestep": [(RECORDER, FH)]}
MISPLACED = {
    "galaxy_physics": [(MODULE, FH)],
    "post_timestep": [(RECORDER, FH)],
}

# ============================================================================
# HELPERS
# ============================================================================


RUNS = PackageRuns(MODULE, PARAMETER_VALUES, OUTPUT_FILES, SUBSTEPS)
parameters_for = RUNS.parameters_for
mimic_run = RUNS.mimic_run
load_snapshot = RUNS.load_snapshot
assert_outputs_identical = RUNS.assert_outputs_identical
assert_refused = RUNS.assert_refused


# ============================================================================
# TEST CASES
# ============================================================================


def test_loads_and_runs():
    """The minimal pipeline with ConcentrationOn 3 runs to completion, exit 0, no leak report."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_loads", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        assert "SAGE set halo concentration module initialized" in stdout, "no initialization log"
        assert "SAGE set halo concentration module cleaned up" in stdout, "no cleanup log"
        for snapshot in OUTPUT_FILES:
            load_snapshot(output_dir, snapshot)
    print(f"  exit status 0, no leak report, snapshots {list(OUTPUT_FILES)} written")


def test_off_is_noop():
    """With ConcentrationOn 0 the output equals the same pipeline's without the module."""
    require_pair()
    require_mimic()
    off = {"ConcentrationOn": 0}
    with mimic_run(f"{MODULE}_off_with", MINIMAL, parameters_for(MINIMAL, off)) as with_module:
        returncode, stdout, stderr, dir_with = with_module
        assert_runs(returncode, stdout, stderr)
        params = parameters_for(MINIMAL_WITHOUT)
        with mimic_run(f"{MODULE}_off_without", MINIMAL_WITHOUT, params) as without:
            returncode, stdout, stderr, dir_without = without
            assert_runs(returncode, stdout, stderr)
            assert_outputs_identical(dir_with, dir_without)


def test_on_writes_domain():
    """With ConcentrationOn 3 every Concentration is finite and >= 0; a Type 0 row has > 0."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_on", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, output_dir = run
        assert_runs(returncode, stdout, stderr)
        for snapshot in OUTPUT_FILES:
            halos = load_snapshot(output_dir, snapshot)
            values = halos["Concentration"]
            bad = int((~np.isfinite(values) | (values < 0)).sum())
            assert bad == 0, f"snapshot {snapshot}: {bad} rows with Concentration not finite, >= 0"
            centrals = halos["Type"] == 0
            positive = int((centrals & (values > 0)).sum())
            print(
                f"  snapshot {snapshot}: {len(halos)} rows in domain; Type 0 rows: "
                f"{int(centrals.sum())}, with Concentration > 0: {positive}"
            )
            if snapshot == 63:
                assert positive > 0, "witness: no Type 0 row with Concentration > 0 at 63"


def test_required_key_concentrationon_refused():
    """A missing ConcentrationOn is refused by this module alone; the control exits 0."""
    require_pair()
    require_mimic()
    key = "ConcentrationOn"
    readers = [name for name in configured_modules(MINIMAL) if key in module_parameters(name)]
    assert readers == [MODULE], f"configured readers of {key}: {readers}"

    with mimic_run(f"{MODULE}_key_control", MINIMAL, parameters_for(MINIMAL)) as run:
        returncode, stdout, stderr, _ = run
        assert_runs(returncode, stdout, stderr)
        print(f"  control ({key} present): exit status {returncode}")

    with mimic_run(f"{MODULE}_key_missing", MINIMAL, parameters_for(MINIMAL, omit=(key,))) as run:
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
    """An entry in a substep phase is refused, naming the module."""
    require_pair()
    require_mimic()
    with mimic_run(f"{MODULE}_misplaced", MISPLACED, parameters_for(MISPLACED)) as run:
        returncode, stdout, stderr, _ = run
        assert_refused(
            returncode,
            stdout,
            stderr,
            [f"{MODULE} is configured in substep phase 'galaxy_physics'"],
        )


def main():
    tests = [
        test_loads_and_runs,
        test_off_is_noop,
        test_on_writes_domain,
        test_required_key_concentrationon_refused,
        test_placement_refused,
    ]
    return run_test_suite(tests, f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
