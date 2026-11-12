#!/usr/bin/env python3
"""
sage26_initialise_merger_clock integration tests.

The clock runs in pre_timestep beside sage26_record_transition_state, the last post_timestep
module, which supplies the previous Type the clock reads and writes the infall outputs the
clock feeds. MergTime itself is not an output property, so the clock's effect on real trees is
observed through TimeOfInfall; the clock's values and transitions are asserted by the unit
tests, and sage26_record_transition_state's integration test follows the transitions over
consecutive snapshots.

One run of the clock and recorder pipeline (file 0) serves the case.

Test cases:
  - test_infall_recorded_at_joining: the pipeline runs without a memory leak and every output
    value is finite; every satellite on the real trees holds the snapshot number it joined at as
    TimeOfInfall, which only the clock writes. The MergerTimeFactor refusals are in
    test_unit_sage26_parameter_refusals.c
"""

import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (  # noqa: E402
    MIMIC_EXE,
    SharedRuns,
    TestSkipped,
    assert_no_infs,
    assert_no_nans,
    check_no_memory_leaks,
    load_binary_halos,
    run_test_suite,
)

PIPELINE = {
    "pre_timestep": [("sage26_initialise_merger_clock", "process_full_halo")],
    "post_timestep": [("sage26_record_transition_state", "process_full_halo")],
}
DEFAULT_RUN = {"phase_config": PIPELINE, "model_params": {}, "first_file": 0, "last_file": 0}
#: The last snapshot of the mini-Millennium trees, the one the default run writes
LAST_SNAPSHOT = 63

RUNS = SharedRuns()


def _run(name, config):
    """The named run of the module's pipeline; SKIP when Mimic is not built."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    return RUNS.run(name, **config)


def _final_snapshot_galaxies(run):
    """The galaxies of the run's z = 0 output file."""
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, "the run should write galaxies"
    return halos


def test_infall_recorded_at_joining():
    """
    The run is clean, and every satellite has the snapshot number it joined at as TimeOfInfall.

    A galaxy is a Type 1 only after progenitor joining made it one from a Type 0, where the
    clock records the descendant's snapshot number; without the clock the creation value -1
    would reach the output instead. So each satellite on the real trees holds an integer
    snapshot number from 1 to the last snapshot, where the recorder writes 0 for a Type 0.
    """
    run = _run("default", DEFAULT_RUN)
    assert run.returncode == 0, f"Mimic should execute successfully\nStderr: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), "the run should not leak memory"
    halos = _final_snapshot_galaxies(run)
    assert_no_nans(halos)
    assert_no_infs(halos)

    satellites = halos[halos["Type"] == 1]
    assert len(satellites) > 0, "the trees should hold satellites"
    infall_time = satellites["TimeOfInfall"]
    assert np.all(infall_time == np.round(infall_time)), "TimeOfInfall is a whole snapshot number"
    assert np.all(
        (infall_time >= 1) & (infall_time <= LAST_SNAPSHOT)
    ), f"TimeOfInfall should lie in [1, {LAST_SNAPSHOT}], got [{infall_time.min()}, {infall_time.max()}]"

    centrals = halos[halos["Type"] == 0]
    assert np.all(centrals["TimeOfInfall"] == 0), "a Type 0 row writes TimeOfInfall 0"


def main():
    """Run the integration cases, removing the runs' temporary directories afterwards."""
    try:
        return run_test_suite(
            [test_infall_recorded_at_joining],
            "sage26_initialise_merger_clock Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
