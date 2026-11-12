#!/usr/bin/env python3
"""
sage_starburst_feedback - Integration Test

Validates: the disk-instability burst through a full gas-supply pipeline.

The merger-event channel of the module is integration-tested with the other merger event
consumers in sage_resolve_mergers_and_disruption/_tests/
test_integration_sage_merger_event_consumers.py; its parameters, guards and exact reservoir
arithmetic are unit-tested in test_unit_sage_starburst_feedback.c.

Test cases:
  - test_full_pipeline_burst: reionization, infall, reincorporation, cooling, disk instability
    and the burst run in one pipeline that supplies cold gas. Some galaxies must burst (a
    positive StarFormationRate, which no other module in this pipeline writes), a bursting row
    must carry stars, bulge and outflow, BulgeMass must not exceed StellarMass, and every
    written reservoir must be finite and non-negative. The run also reports no memory leak.

Author: Mimic Development Team
Date: 2025-12-23
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

# The gas supply (reionization, infall, reincorporation, cooling), the instability trigger and
# the burst. There is no disk star formation module and no merger module, so every star in the
# output comes from the burst module's disk-instability channel.
FULL_PIPELINE_PHASES = {
    "pre_timestep": [
        ("sage_reionization", "process_full_halo"),
        ("sage_prepare_infall_budget", "process_full_halo"),
        ("sage_set_disk_scale_radius", "process_full_halo"),
    ],
    "galaxy_physics": [
        # Gas accretion chain
        ("sage_apply_infall", "process_full_halo"),
        ("sage_reincorporation", "process_full_halo"),
        # Cooling chain
        ("sage_calculate_cooling_budget", "process_by_galaxy"),
        ("sage_apply_cooling", "process_by_galaxy"),
        # Disk instability sets UnstableDiskGasFraction; the burst consumes it
        ("sage_disk_instability", "process_by_galaxy"),
        ("sage_starburst_feedback", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}

FULL_PIPELINE_PARAMS = {
    # Infall and reionization
    "GlobalBaryonFraction": 0.17,
    # Reincorporation
    "ReIncorporationFactor": 0.15,
    # Disk instability
    "StarFormingDiskFactor": 3.0,
    # The burst
    "FeedbackReheatingEpsilon": 3.0,
    "FeedbackEjectionEfficiency": 0.5,
    "RecycleFraction": 0.43,
    "Yield": 0.03,
    "FracZleaveDisk": 0.5,
    "ThresholdMajorMerger": 0.3,
}


def test_full_pipeline_burst():
    """Disk-instability bursts happen in a pipeline that supplies gas

    The only writer of StarFormationRate in this pipeline is the burst, so a positive rate
    marks a galaxy that burst in its last snapshot. Such a galaxy formed stars, put them in
    its bulge and reheated gas into its outflow.
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")

    run = RUNS.run(
        "starburst_full_pipeline",
        phase_config=FULL_PIPELINE_PHASES,
        model_params=FULL_PIPELINE_PARAMS,
    )
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"

    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")
    assert len(halos) > 0, "Should have output halos"

    # The supply pipeline gives the galaxies cold gas to burst
    assert np.sum(halos["ColdGas"]) > 0.0, "Infall and cooling should supply cold gas"

    # Every reservoir the burst writes is finite and non-negative
    for name in (
        "StellarMass",
        "BulgeMass",
        "ColdGas",
        "HotGas",
        "EjectedGas",
        "StarFormationRate",
        "SupernovaOutflowRate",
        "MetalsStellarMass",
        "MetalsBulgeMass",
        "MetalsColdGas",
        "MetalsHotGas",
        "MetalsEjectedGas",
    ):
        values = halos[name]
        assert np.all(np.isfinite(values)), f"{name} should be finite"
        assert np.all(values >= 0.0), f"{name} should be non-negative"

    # Some galaxies burst, and a bursting galaxy carries the burst's products
    bursting = halos["StarFormationRate"] > 0.0
    assert np.any(bursting), "Some galaxy should burst in the full pipeline"
    assert np.all(halos["StellarMass"][bursting] > 0.0), "A bursting galaxy has formed stars"
    assert np.all(halos["BulgeMass"][bursting] > 0.0), "A burst's stars go to the bulge"
    assert np.all(
        halos["SupernovaOutflowRate"][bursting] > 0.0
    ), "A burst reheats gas into the outflow"

    # The burst adds the same mass to both and the instability's transfer to the bulge is clamped
    # at StellarMass, so the bound is exact
    assert np.all(
        halos["BulgeMass"] <= halos["StellarMass"]
    ), "BulgeMass should not exceed StellarMass"


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_full_pipeline_burst], "sage_starburst_feedback Integration Tests"
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
