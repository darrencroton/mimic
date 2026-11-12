#!/usr/bin/env python3
"""
SAGE26 Set Disk Scale Radius Module - Integration Test

Validates: DiskScaleRadius on real halos against the Mo, Mao & White (1998) formula

This test validates the sage26_set_disk_scale_radius module in a one-module pipeline
(BulgeSizeOn 0), the only configuration it needs:
- The run completes without memory leaks and DiskScaleRadius is finite and non-negative
- For FoF centrals DiskScaleRadius equals |J| / (1.414^2 * Vvir), the closed form of
  (lambda / 1.414) * Rvir with lambda = |J| / (1.414 * Vvir * Rvir), computed from the
  output's own Spin and Vvir

Test cases:
  - test_disk_radius_matches_formula: DiskScaleRadius of real centrals against the formula

Author: Mimic Development Team
Date: 2025-12-18
"""

import sys
from pathlib import Path

import numpy as np

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (
    MIMIC_EXE,
    SharedRuns,
    TestSkipped,
    check_no_memory_leaks,
    load_binary_halos,
    run_test_suite,
)

RUNS = SharedRuns()

# The module alone in pre_timestep; BulgeSizeOn 0 is its required parameter
PIPELINE = {
    "pre_timestep": [("sage26_set_disk_scale_radius", "process_full_halo")],
    "galaxy_physics": [],
    "satellite_mergers": [],
    "post_timestep": [],
}
PARAMS = {"BulgeSizeOn": 0}

# The truncated literal SAGE26 uses in place of sqrt(2); sqrt(2) itself shifts every radius by 3e-4
SQRT_REPLACEMENT = 1.414

# DiskScaleRadius is stored as a float (6e-8 precision); the 1e-6 margin covers the output's float
# Spin and the module's float spin products
RELATIVE_TOLERANCE = 1e-6


def test_disk_radius_matches_formula():
    """DiskScaleRadius of real centrals equals |J| / (1.414^2 * Vvir).

    The output's Vvir is recomputed from the final Mvir when the file is written, while the
    module reads the halo's own Vvir, which is refreshed only when the halo's mass grows. The two
    agree for centrals whose mass grew at the last step (deltaMvir > 0), so the exact comparison
    uses those; every Type 0 radius is finite and non-negative.
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run("disk_radius", phase_config=PIPELINE, model_params=PARAMS)
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"

    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    disk_radius = halos["DiskScaleRadius"].astype(np.float64)
    assert np.all(np.isfinite(disk_radius)), "DiskScaleRadius should be finite"
    assert np.all(disk_radius >= 0.0), "DiskScaleRadius should be non-negative"

    centrals = halos[(halos["Type"] == 0) & (halos["Mvir"] > 0) & (halos["deltaMvir"] > 0)]
    assert len(centrals) > 100, f"Need centrals that grew at the last step (got {len(centrals)})"

    spin = centrals["Spin"].astype(np.float64)
    spin_magnitude = np.sqrt((spin * spin).sum(axis=1))
    expected = spin_magnitude / (SQRT_REPLACEMENT * SQRT_REPLACEMENT * centrals["Vvir"])
    actual = centrals["DiskScaleRadius"].astype(np.float64)

    relative_error = np.abs(actual - expected) / expected
    worst = int(np.argmax(relative_error))
    assert relative_error[worst] < RELATIVE_TOLERANCE, (
        f"DiskScaleRadius must equal |J| / (1.414^2 * Vvir): worst of {len(centrals)} centrals "
        f"has {actual[worst]:.8e} vs expected {expected[worst]:.8e}"
    )


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_disk_radius_matches_formula],
            "sage26_set_disk_scale_radius Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
