#!/usr/bin/env python3
"""
sage_disk_instability - Integration Test

Validates the disk instability module through one real Mimic run on the committed test trees, in a
pipeline that supplies the disks an instability needs.

The supply pipeline: infall supplies the hot gas, cooling the
cold gas, and the star-formation chain the stellar disk; sage_disk_instability then moves the
stars of unstable disks to the bulge. Disk instability alone would act on empty galaxies and write
zeros, so no case runs it that way. The run is configured once and shared by the cases.

Test cases:
  - test_property_values_physical: unstable disks occur (some rows hold a bulge), every row's
    BulgeMass and MetalsBulgeMass are non-negative and at most the stellar totals, and the run
    reports no memory leak

UnstableDiskGasFraction is an internal hand-off to the downstream modules (not an output
property), so the instability's gas branch is asserted in the C unit tests.

Run (from the repository root, after `make MODEL=sage16 SIMULATION=mini-millennium`):
  MODEL=sage16 SIMULATION=mini-millennium python3 \\
    models/sage16/modules/sage_disk_instability/_tests/test_integration_sage_disk_instability.py
"""

import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[5]
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (  # noqa: E402
    MIMIC_EXE,
    SharedRuns,
    TestSkipped,
    check_no_memory_leaks,
    load_binary_halos,
    run_test_suite,
)

RUNS = SharedRuns()

# The gas supply and the star-formation chain, then the disk instability and the metal yield that
# gives the stars, and so the bulge, their metals
SUPPLY_PHASES = {
    "pre_timestep": [
        ("sage_set_disk_scale_radius", "process_full_halo"),
        ("sage_reionization", "process_full_halo"),
        ("sage_prepare_infall_budget", "process_full_halo"),
    ],
    "galaxy_physics": [
        ("sage_apply_infall", "process_full_halo"),
        ("sage_calculate_cooling_budget", "process_by_galaxy"),
        ("sage_apply_cooling", "process_by_galaxy"),
        ("sage_calculate_star_formation", "process_by_galaxy"),
        ("sage_calculate_supernova_feedback", "process_by_galaxy"),
        ("sage_apply_star_formation_supernova", "process_by_galaxy"),
        ("sage_disk_instability", "process_by_galaxy"),
        ("sage_apply_metal_enrichment", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}
SUPPLY_PARAMS = {
    "StarFormingDiskFactor": 3.0,
    "GlobalBaryonFraction": 0.17,
    "SfrEfficiency": 0.02,
    "FeedbackReheatingEpsilon": 3.0,
    "FeedbackEjectionEfficiency": 0.3,
    "RecycleFraction": 0.43,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
}


def supply_run():
    """Run (once) and return the supply pipeline, asserting a clean exit."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run("supply", phase_config=SUPPLY_PHASES, model_params=SUPPLY_PARAMS)
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    return run


def test_property_values_physical():
    """Unstable disks occur, the bulge never exceeds the stellar totals, and nothing leaks.

    The module clamps the bulge to the stellar total after the transfer (BulgeMass and
    MetalsBulgeMass each at most StellarMass and MetalsStellarMass), so the bounds are exact and
    carry no rounding allowance. The run's own leak report is clean.
    """
    run = supply_run()
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, "Should have output halos"

    bulge = np.asarray(halos["BulgeMass"], dtype=np.float32)
    metals_bulge = np.asarray(halos["MetalsBulgeMass"], dtype=np.float32)
    print(f"  {int(np.sum(bulge > 0))} of {len(halos)} rows hold a bulge")
    assert np.sum(bulge > 0) > 0, "unstable disks should move stars to the bulge on some rows"
    assert np.sum(metals_bulge > 0) > 0, "the moved stars should carry metals to the bulge"

    assert (bulge >= 0).all(), "BulgeMass should be >= 0"
    assert (bulge <= halos["StellarMass"]).all(), "BulgeMass should not exceed StellarMass"
    assert (metals_bulge >= 0).all(), "MetalsBulgeMass should be >= 0"
    assert (
        metals_bulge <= halos["MetalsStellarMass"]
    ).all(), "MetalsBulgeMass should not exceed MetalsStellarMass"


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_property_values_physical],
            "sage_disk_instability Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
