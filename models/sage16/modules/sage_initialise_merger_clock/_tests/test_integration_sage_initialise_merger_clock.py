#!/usr/bin/env python3
"""
sage_initialise_merger_clock integration test.

MergTime is not an output property, so the clock is observed through the module that consumes
it. sage_resolve_mergers_and_disruption refuses a satellite whose clock is still unset
(999 or more) as an error, so a clock in pre_timestep followed by the merger pass over the real
mini-Millennium trees (file 0) runs to the end only if the clock gave every satellite a
calculated clock before the pass counted it down. The clock's values, thresholds and
transitions are asserted by the unit tests.

Test cases:
  - test_clock_feeds_merger_pass: the pipeline runs without a memory leak or an unset-clock
    error and writes finite values
"""

import sys
from pathlib import Path

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

CLOCK_AND_MERGER_PASS = {
    "phase_config": {
        "pre_timestep": [("sage_initialise_merger_clock", "process_full_halo")],
        "satellite_mergers": [("sage_resolve_mergers_and_disruption", "process_full_halo")],
    },
    "model_params": {"ThresholdMajorMerger": 0.3, "ThresholdSatDisruption": 1.0},
    "first_file": 0,
    "last_file": 0,
}

RUNS = SharedRuns()


def test_clock_feeds_merger_pass():
    """The merger pass finds a calculated clock on every satellite and the run is clean."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run("clock_and_merger_pass", **CLOCK_AND_MERGER_PASS)

    assert run.returncode == 0, (
        "the merger pass should find a calculated clock on every satellite\n"
        f"Stderr: {run.stderr}"
    )
    assert (
        "unset MergTime" not in run.stdout + run.stderr
    ), "no satellite should have an unset clock"
    assert check_no_memory_leaks(run.stdout, run.stderr), "the run should not leak memory"
    assert (
        "SAGE initialise merger clock initialized" in run.stdout
    ), f"the module should log its initialisation\nStdout:\n{run.stdout}"

    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, "the run should write galaxies"
    assert_no_nans(halos)
    assert_no_infs(halos)


def main():
    """Run the integration case, removing the run's temporary directory afterwards."""
    try:
        return run_test_suite(
            [test_clock_feeds_merger_pass], "sage_initialise_merger_clock Integration Tests"
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
