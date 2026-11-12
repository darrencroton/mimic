#!/usr/bin/env python3
"""
sage26_starburst_feedback - Integration Test

Validates: the disk-instability burst through a full gas-supply pipeline.

The merger-event channel of the module is integration-tested with the other merger event
consumers in sage26_resolve_mergers_and_disruption/_tests/
test_integration_sage26_merger_event_consumers.py; its parameters, guards and exact
reservoir arithmetic are unit-tested in test_unit_sage26_starburst_feedback.c.

Test cases:
  - test_full_pipeline_burst: reionization, infall, reincorporation, cooling, disk instability
    and the burst run in one pipeline that supplies cold gas. Some galaxies must burst (a
    positive StarFormationRate, which no other module in this pipeline writes), a bursting row
    must carry stars, bulge, instability-channel bulge and outflow, no row may carry
    merger-channel bulge (the pipeline has no mergers), BulgeMass must stay within SAGE26's
    rounding bound of StellarMass, and every written reservoir must be finite and non-negative.
    The run also reports no memory leak.

Author: Mimic Development Team
Date: 2025-12-23
"""

import sys
from pathlib import Path

import numpy as np
import yaml

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
        ("sage26_reionization", "process_full_halo"),
        ("sage26_prepare_infall_budget", "process_full_halo"),
        ("sage26_set_disk_scale_radius", "process_full_halo"),
    ],
    "galaxy_physics": [
        # Gas accretion chain
        ("sage26_apply_infall", "process_full_halo"),
        ("sage26_reincorporation", "process_full_halo"),
        # Cooling chain
        ("sage26_calculate_cooling_budget", "process_by_galaxy"),
        ("sage26_apply_cooling", "process_by_galaxy"),
        # Disk instability sets UnstableDiskGasFraction; the burst consumes it
        ("sage26_disk_instability", "process_by_galaxy"),
        ("sage26_starburst_feedback", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}

FULL_PIPELINE_PARAMS = {
    # Required by the burst module: SFprescription 0, cold-gas bursts, fixed-epsilon feedback,
    # no CGM routing, no bulge sizes
    "SFprescription": 0,
    "StarburstColdGasOn": 1,
    "FIREmodeOn": 0,
    "CGMrecipeOn": 0,
    "BulgeSizeOn": 0,
    # Infall and reionization
    "GlobalBaryonFraction": 0.17,
    # Cooling with the AGN off
    "AGNrecipe": 0,
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


def burst_write_limit(param_file):
    """An upper limit on the bursts one galaxy can have in a run of this parameter file

    sage26_starburst_feedback's process_by_galaxy channel bursts a galaxy at most once per
    substep, and a galaxy lives at most one snapshot interval per entry of the simulation's
    snapshot list, so the limit is the number of a_list entries times SubSteps. The
    fixed timestep scheme is required: a dynamic scheme's substep count is not SubSteps.
    """
    with open(param_file) as handle:
        params = yaml.safe_load(handle)
    scheme = str(params.get("TimestepScheme", "fixed")).lower()
    assert scheme == "fixed", f"bulge bound assumes the fixed timestep scheme, got {scheme}"
    substeps = int(params.get("SubSteps", 1))
    with open(params["simulation"]["config"]) as handle:
        simulation = yaml.safe_load(handle)
    a_list = Path(simulation["input"]["snapshot_list_file"])
    if not a_list.is_absolute():
        a_list = REPO_ROOT / a_list
    snapshots = sum(1 for line in a_list.read_text().splitlines() if line.strip())
    return snapshots * substeps


def bulge_upper_bound(stellar_mass, bulge_mass, param_file):
    """The largest BulgeMass SAGE26's arithmetic allows a row of these pipelines

    BulgeMass <= StellarMass + (3 * B + 1) * D, where B = burst_write_limit(param_file)
    and D = 2**-22 * max(StellarMass, BulgeMass) + 2**-148 (one float rounding of the
    larger reservoir, with room for the double roundings, and the subnormal spacing).

    Why it holds. These pipelines run sage26_disk_instability and this module's
    process_by_galaxy burst, with no disk star formation, no mergers and no stellar
    stripping, so a galaxy's StellarMass and BulgeMass start at 0 and change only through
    its own bursts and instability transfers, and never decrease (stars, recycled_stars
    and unstable_stars are non-negative). Let e = BulgeMass - StellarMass after b bursts.
    By induction, -b * D <= e <= (3 * b + 1) * D:

    - a burst adds the rounded double recycled_stars = (1 - RecycleFraction) * stars to
      BulgeMass and the same product to StellarMass; each sum is rounded to double and then to
      float, so e moves by at most one float rounding of each reservoir plus the product's
      double rounding, at most D in either direction;
    - the instability transfer adds unstable_stars to BulgeMass only when the star fraction is
      positive, i.e. when the float disk StellarMass - BulgeMass is positive, so only from
      e < 0, and it never lowers e. That disk is exactly -e (the two floats are within a factor
      of two). The float disk mass ColdGas + (-e) exceeds ColdGas only if -e is at least half a
      float spacing of ColdGas, so unstable_stars is at most -e plus one float spacing of the
      disk mass plus double roundings, at most 3 * (-e), and the transfer leaves
      e <= 2 * (-e) + D <= 3 * b * D + D.

    The constants are generous (2**-22 for 2**-23 plus double terms, 3 for 2 plus 2**-25),
    which also covers the double rounding of this bound's own evaluation. The bound is not
    looser than SAGE26's arithmetic: the excess does grow with the number of bursts, as
    test_unit_sage26_starburst_feedback.c::test_consecutive_bursts_bulge_excess_grows pins
    with SAGE26's own bits (two floats above after three bursts), so no fixed number of
    floats bounds it. These pipelines show no row above StellarMass at all.
    """
    stellar = np.asarray(stellar_mass, dtype=np.float32).astype(np.float64)
    bulge = np.asarray(bulge_mass, dtype=np.float32).astype(np.float64)
    bursts = burst_write_limit(param_file)
    spacing = 2.0**-22 * np.maximum(stellar, bulge) + 2.0**-148
    return stellar + (3 * bursts + 1) * spacing


def test_full_pipeline_burst():
    """Disk-instability bursts happen in a pipeline that supplies gas, and credit one channel

    The only writer of StarFormationRate in this pipeline is the burst, so a positive rate
    marks a galaxy that burst in its last snapshot. Such a galaxy formed stars, put them in
    its bulge through the instability channel (the pipeline has no mergers, so the merger
    channel's MergerBulgeMass stays 0 in every row) and reheated gas into its outflow.
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
        "InstabilityBulgeMass",
        "MergerBulgeMass",
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
        halos["InstabilityBulgeMass"][bursting] > 0.0
    ), "A disk-instability burst credits the instability channel"
    assert np.all(
        halos["SupernovaOutflowRate"][bursting] > 0.0
    ), "A burst reheats gas into the outflow"
    assert np.all(
        halos["MergerBulgeMass"] == 0.0
    ), "Without mergers no burst credits the merger channel"

    # Bulge mass does not exceed stellar mass beyond SAGE26's float rounding
    param_file = run.output_dir.parent / f"{run.output_dir.name}.yaml"
    assert np.all(
        halos["BulgeMass"]
        <= bulge_upper_bound(halos["StellarMass"], halos["BulgeMass"], param_file)
    ), "BulgeMass should not exceed StellarMass beyond SAGE26's rounding bound"


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [test_full_pipeline_burst], "sage26_starburst_feedback Integration Tests"
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
