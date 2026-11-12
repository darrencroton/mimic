#!/usr/bin/env python3
"""
sage26_disk_instability - Integration Test

Validates the disk instability module through one real Mimic run on the committed test trees, in a
pipeline that supplies the disks an instability needs.

The supply pipeline (CGMrecipeOn 0, SFprescription 0): infall supplies the hot gas, cooling the
cold gas, and the star-formation chain the stellar disk; sage26_disk_instability then moves the
stars of unstable disks to the bulge. Disk instability alone would act on empty galaxies and write
zeros, so no case runs it that way. The run is configured once and shared by the cases.

Test cases:
  - test_property_values_physical: the run reports no memory leak; unstable disks occur (some rows
    hold a bulge), and every row's BulgeMass and MetalsBulgeMass are non-negative and at most one
    float above the stellar totals

UnstableDiskGasFraction is an internal hand-off to the downstream modules (not an output
property), so the instability's gas branch is asserted in the C unit tests.

Run (from the repository root, after `make MODEL=sage26 SIMULATION=mini-millennium`):
  MODEL=sage26 SIMULATION=mini-millennium python3 \\
    models/sage26/modules/sage26_disk_instability/_tests/test_integration_sage26_disk_instability.py
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
        ("sage26_set_disk_scale_radius", "process_full_halo"),
        ("sage26_reionization", "process_full_halo"),
        ("sage26_prepare_infall_budget", "process_full_halo"),
    ],
    "galaxy_physics": [
        ("sage26_apply_infall", "process_full_halo"),
        ("sage26_calculate_cooling_budget", "process_by_galaxy"),
        ("sage26_apply_cooling", "process_by_galaxy"),
        ("sage26_calculate_star_formation", "process_by_galaxy"),
        ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
        ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
        ("sage26_disk_instability", "process_by_galaxy"),
        ("sage26_apply_metal_enrichment", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}
SUPPLY_PARAMS = {
    "BulgeSizeOn": 0,
    "StarFormingDiskFactor": 3.0,
    "SFprescription": 0,
    "CGMrecipeOn": 0,
    "GlobalBaryonFraction": 0.17,
    "H2RadialIntegrationOn": 1,
    "H2RadialNBins": 25,
    "H2RadialRMaxFactor": 5.0,
    "SfrEfficiency": 0.02,
    "FeedbackReheatingEpsilon": 3.0,
    "FeedbackEjectionEfficiency": 0.3,
    "FIREmodeOn": 0,
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
    """The run leaks nothing, unstable disks occur, and the bulge stays within SAGE26's rounding.

    SAGE26 adds the double unstable_stars straight into the float BulgeMass, with no clamp to
    StellarMass, so a full-disk transfer can leave the bulge one float above the stellar total.
    A bulge already above it has a negative disk and transfers nothing more, so one float is the
    bound (likewise for the metals).
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

    def one_float_above(values):
        return np.nextafter(np.asarray(values, dtype=np.float32), np.float32(np.inf))

    assert (bulge >= 0).all(), "BulgeMass should be >= 0"
    assert (
        bulge <= one_float_above(halos["StellarMass"])
    ).all(), "BulgeMass should be at most one float above StellarMass"
    assert (metals_bulge >= 0).all(), "MetalsBulgeMass should be >= 0"
    assert (
        metals_bulge <= one_float_above(halos["MetalsStellarMass"])
    ).all(), "MetalsBulgeMass should be at most one float above MetalsStellarMass"


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_property_values_physical],
            "sage26_disk_instability Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
