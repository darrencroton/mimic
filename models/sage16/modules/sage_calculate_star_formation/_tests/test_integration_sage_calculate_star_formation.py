#!/usr/bin/env python3
"""
SAGE Calculate Star Formation Module - Integration Test

Validates: the module end to end on a pipeline that supplies cold gas

The pipeline supplies the cold gas the module consumes: the disk scale radius (which sets the
star-forming disk, reff = 3 r_s), reionization, infall and cooling (with radio-mode heating) feed
the disk, then the star-formation chain runs, with no supernova calculator and so no feedback. One
Mimic run hosts the case, which requires star formation to have happened (a positive witness), so
it cannot pass on an empty disk.

Test cases:
  - test_star_formation_physics: the run succeeds without memory leaks and writes finite,
    non-negative star formation rates that are positive on some rows; the stars formed over the
    snapshot, net of recycling, are in the galaxy (a bound the largest ratio reaches, so it is
    tight); rows without cold gas form no stars

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

# The gas supply before the module: without the disk scale radius no disk forms stars, and
# without infall and cooling the disk holds no cold gas
PHASES = {
    "pre_timestep": [
        ("sage_set_disk_scale_radius", "process_full_halo"),
        ("sage_reionization", "process_full_halo"),
        ("sage_prepare_infall_budget", "process_full_halo"),
    ],
    "galaxy_physics": [
        ("sage_apply_infall", "process_full_halo"),
        ("sage_calculate_cooling_budget", "process_by_galaxy"),
        ("sage_radio_mode_heating", "process_by_galaxy"),
        ("sage_apply_cooling", "process_by_galaxy"),
        ("sage_calculate_star_formation", "process_by_galaxy"),
        ("sage_apply_star_formation_supernova", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}

RECYCLE_FRACTION = 0.43

# Every key the pipeline reads
PARAMS = {
    "GlobalBaryonFraction": 0.17,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "SfrEfficiency": 0.02,
    "StarFormingDiskFactor": 3.0,
    "RecycleFraction": RECYCLE_FRACTION,
}


def test_star_formation_physics():
    """Star formation on a cold-gas pipeline is finite, non-negative and bounded by StellarMass

    Validates:
    - The run succeeds without memory leaks and writes halos; StarFormationRate is finite,
      non-negative and positive on some rows
    - The stars formed over the snapshot, net of recycling, are held by the galaxy: with no
      merger, disruption or feedback module nothing removes stars, so (1 - RecycleFraction) times
      the stars formed is at most StellarMass. StarFormationRate (Msun/yr) times dT (Myr) is the
      stars formed in 10^6 Msun, so a factor 10^-4 gives the code's 10^10 Msun (the writer
      conversions of both fields, model_properties.yaml and src/core/core_properties.yaml, at
      UnitMass_in_g = 10^10 solar masses). A galaxy whose stars all formed this snapshot sits at
      equality, so the largest ratio reaching 1 shows the conversion is not a factor off low
    - Rows with no cold gas form no stars, and such rows exist
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run("sf_physics", phase_config=PHASES, model_params=PARAMS)
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"
    output_file = run.output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"
    halos, _metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have output halos"

    sfr = halos["StarFormationRate"].astype(np.float64)
    assert np.all(np.isfinite(sfr)), "StarFormationRate should be finite"
    assert np.all(sfr >= 0.0), "StarFormationRate should be non-negative"
    forming = sfr > 0
    assert np.sum(forming) > 0, "some rows should form stars (the pipeline supplies cold gas)"
    print(f"  {int(np.sum(forming))} of {len(halos)} rows form stars")

    formed = sfr * halos["dT"].astype(np.float64) * 1e-4
    retained = (1.0 - RECYCLE_FRACTION) * formed[forming]
    stellar_mass = halos["StellarMass"].astype(np.float64)[forming]
    ratio = retained / stellar_mass
    # 1e-5 relative: float stores; a galaxy whose stars all formed this snapshot is at equality
    excess = retained > stellar_mass * (1.0 + 1e-5)
    assert not np.any(excess), (
        f"{int(np.sum(excess))} rows formed more stars this snapshot than they hold "
        f"(largest ratio {np.max(ratio):.6f})"
    )
    assert np.max(ratio) > 1.0 - 1e-5, (
        f"no row holds only the stars formed this snapshot (largest ratio {np.max(ratio):.6f}): "
        "the star-formation conversion to stellar mass is off"
    )
    print(f"  retained/StellarMass at most {np.max(ratio):.7f}")

    no_gas = halos["ColdGas"] < 1e-6
    assert np.sum(no_gas) > 0, "some rows should hold no cold gas"
    assert np.all(sfr[no_gas] == 0.0), "Galaxies with no cold gas should have zero SFR"
    print(f"  Verified {np.sum(no_gas)} galaxies with no gas have zero SFR")


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite([test_star_formation_physics], "sage_calculate_star_formation")
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
