#!/usr/bin/env python3
"""
sage26_set_halo_concentration - Integration Test

Validates: SAGE26's halo-concentration assignment at progenitor joining through real runs of
mimic on the committed test trees: it writes nothing with ConcentrationOn 0, writes only its
domain with ConcentrationOn 3. The parameter refusals are in the C test
test_unit_sage26_parameter_refusals.c.

Every configuration sets every key its modules read, explicitly. The keys come from each configured
module's module_info.yaml `parameters` list, which `make lint-parameters` holds to the code, with
the vertical run file's values (models/sage26/input/sage26_mini-millennium.yaml). Runs use the run
file's SubSteps (10) and write snapshots 63 and 16.

Minimal pipeline (Off, On; ConcentrationOn 3 for On), in the run file's order:
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
    0, and every other row keeps its value (README.md, "Processing Contract", the assignment
    table).
  - Witness: at least one Type 0 row with Concentration > 0 at snapshot 63.
  - Exceptions: none. Concentration is the only property the module writes.

The module's placement refusals are asserted, with their diagnostics, by the unit tests; each
configuration above runs once.

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


def main():
    return run_test_suite([test_off_is_noop, test_on_writes_domain], f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
