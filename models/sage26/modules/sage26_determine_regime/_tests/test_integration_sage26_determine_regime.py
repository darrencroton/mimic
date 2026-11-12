#!/usr/bin/env python3
"""
sage26_determine_regime - Integration Test

Validates: the CGM regime classifier through a real run of mimic on the committed test trees: with
CGMrecipeOn 1 it writes only its domain, Regime, and both regimes occur at the final snapshot.

Every configuration sets every key its modules read, explicitly. The keys come from each configured
module's module_info.yaml `parameters` list, which `make lint-parameters` holds to the code, with
the vertical run file's values (models/sage26/input/sage26_mini-millennium.yaml). The run uses the
run file's SubSteps (10) and writes snapshots 63 and 16.

Minimal pipeline (CGMrecipeOn 1):
  pre_timestep: sage26_determine_regime
  - sage26_determine_regime: the module under test. Its guards require nothing else: the
    placement check orders it before sage26_prepare_infall_budget only when that module is
    configured (sage26_determine_regime.c, validate_placement). Its reads, Type, Mvir and
    UniqueGalaxyID, are core properties, so no other module is needed to supply them. The pipeline
    has no gas supply, so the baryon reservoirs are all zero by design; the module writes only
    Regime and the case asserts on that field alone.

On writes its domain (the observable list):
  - Regime: in {0, 1} on every output row of both snapshots. Domain: the module classifies every
    live row 0 or 1 and leaves only Type 3 rows, which are never output (README.md, "Rows" and
    "Properties": "Writes: Regime"); the property's range is [-1, 1] with -1 the creation value
    and "not yet assigned" sentinel, so -1 on an output row would be a row the classifier missed.
  - Witness: at least one row with Regime 1 and at least one with Regime 0 at snapshot 63.
  - Exceptions: none. Regime is the only property the module writes.

The CGMrecipeOn 0 writes-nothing contract, the required-parameter refusals and the placement
refusals are asserted by the module's C unit tests, which reach each branch directly.

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

RUNS = PackageRuns(MODULE, PARAMETER_VALUES, OUTPUT_FILES, SUBSTEPS)
parameters_for = RUNS.parameters_for
mimic_run = RUNS.mimic_run
load_snapshot = RUNS.load_snapshot

# ============================================================================
# TEST CASES
# ============================================================================


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


def main():
    return run_test_suite([test_on_writes_domain], f"{MODULE} Integration Tests")


if __name__ == "__main__":
    sys.exit(main())
