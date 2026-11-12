#!/usr/bin/env python3
"""
SAGE Calculate Supernova Feedback Module - Integration Test

Validates: the module end to end, on a pipeline that supplies cold gas

The pipeline: the disk scale radius, reionization, infall and cooling (with radio-mode heating)
feed the disk before the star-formation chain. Each case requires star formation to have happened,
so none can pass on an empty disk. Three Mimic runs, one per configuration:

  - the standard run (FeedbackReheatingEpsilon 3, FeedbackEjectionEfficiency 0.3)
  - the same with FeedbackEjectionEfficiency 0, the control that shows ejection
  - a high-efficiency run (epsilon 50, eta 10), where the renormalisation binds

Test cases:
  - test_feedback_reasonable_magnitude: on every star-forming row the outflow rate is exactly
    epsilon times the star formation rate; no outflow without star formation; the run is leak-free
  - test_edge_case_very_high_efficiency: the same exact relation at epsilon 50, and rows whose
    stars and reheated mass use up the cold gas without overdrawing it, which only the
    renormalisation allows
  - test_ejection_efficiency_sensitivity: FeedbackEjectionEfficiency 0.3 ejects gas into EjectedGas
    on some rows and 0 ejects none

Reference: Croton et al. (2006, 2016) - SAGE model

Author: Mimic Development Team
Date: 2025-12-18 (Refactored for comprehensive physics validation)
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

RECYCLE_FRACTION = 0.43

# One substep per snapshot interval: the cold-gas bound of drained_rows holds per substep, and the
# output's star formation accumulates over all of a row's substeps
SUBSTEPS = 1

# The pipeline. Without the disk scale radius no disk forms stars, and without infall and cooling
# the disk holds no cold gas, so neither star formation nor its feedback happens.
PHYSICS_PHASES = {
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
        ("sage_calculate_supernova_feedback", "process_by_galaxy"),
        ("sage_apply_star_formation_supernova", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}


def feedback_params(epsilon, eta):
    """Every key the pipeline reads, with the feedback's FeedbackReheatingEpsilon and eta."""
    return {
        # The gas supply
        "GlobalBaryonFraction": 0.17,
        "AGNrecipe": 2,
        "RadioModeEfficiency": 0.08,
        # Star formation and feedback
        "SfrEfficiency": 0.02,
        "StarFormingDiskFactor": 3.0,
        "FeedbackReheatingEpsilon": epsilon,
        "FeedbackEjectionEfficiency": eta,
        "RecycleFraction": RECYCLE_FRACTION,
    }


STANDARD = feedback_params(3.0, 0.3)
NO_EJECTION = feedback_params(3.0, 0.0)
HIGH_EFFICIENCY = feedback_params(50.0, 10.0)


def run_z0(name, model_params):
    """Run the pipeline once under `name`; return (halos at z = 0, stdout, stderr)."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run(name, phase_config=PHYSICS_PHASES, model_params=model_params, substeps=SUBSTEPS)
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    output_file = run.output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"
    halos, _metadata = load_binary_halos(output_file)
    return halos, run.stdout, run.stderr


def assert_outflow_relation(halos, epsilon):
    """Assert the exact outflow relation on star-forming rows, and no outflow elsewhere.

    The reheated mass is FeedbackReheatingEpsilon times the stars formed, both scaled together
    when they exceed the cold gas (sage_calculate_supernova_feedback). StarFormationRate and
    SupernovaOutflowRate both accumulate their mass over the row's own dT and share the writer
    conversion to Msun/yr, so on every star-forming row

        SupernovaOutflowRate = epsilon * StarFormationRate

    Rows without star formation have no outflow.
    """
    sfr = halos["StarFormationRate"].astype(np.float64)
    snfb = halos["SupernovaOutflowRate"].astype(np.float64)
    assert np.all(np.isfinite(snfb)), "SupernovaOutflowRate should be finite"
    assert np.all(snfb >= 0.0), "SupernovaOutflowRate should be non-negative"

    has_sf = sfr > 0.0
    assert np.any(has_sf), "some galaxies should form stars (the pipeline supplies cold gas)"
    expected = epsilon * sfr[has_sf]
    # 1e-5 relative: both are float stores
    off = np.abs(snfb[has_sf] - expected) > 1e-5 * expected
    assert not np.any(off), (
        f"{int(np.sum(off))} star-forming rows break SupernovaOutflowRate = epsilon * "
        f"StarFormationRate (largest relative error "
        f"{np.max(np.abs(snfb[has_sf] / expected - 1.0)):.3e})"
    )
    print(f"  {int(np.sum(has_sf))} star-forming rows satisfy the outflow relation at {epsilon}")

    no_sf = ~has_sf
    assert np.any(no_sf), "some galaxies should form no stars"
    assert np.all(snfb[no_sf] == 0.0), "Galaxies without star formation should have no outflow"


def drained_rows(halos):
    """Return the ratio of ColdGas to RecycleFraction times the stars formed, on star-forming rows.

    Star formation takes (1 - RecycleFraction) of the stars formed out of the cold gas and
    feedback takes the reheated mass, so a row whose stars and reheated mass use up the cold gas
    (the renormalisation binds, stars + reheated = cold gas) is left with exactly
    RecycleFraction times the stars formed (the stars are in 10^6 Msun per Msun/yr and Myr, a
    factor 10^-4 to the code's 10^10 Msun). A row cannot be left with less: without the
    renormalisation the reheated mass would overdraw the cold gas, which this model does not clamp.
    This holds for one substep's draw on the cold gas against that substep's stars. With several
    substeps the stars formed (the accumulated StarFormationRate) sum over substeps while the cold
    gas left reflects each substep's draw on what cooling supplied since, so the bound is asserted
    only on runs at SUBSTEPS = 1.
    """
    sfr = halos["StarFormationRate"].astype(np.float64)
    has_sf = sfr > 0.0
    formed = sfr[has_sf] * halos["dT"].astype(np.float64)[has_sf] * 1e-4
    return halos["ColdGas"].astype(np.float64)[has_sf] / (RECYCLE_FRACTION * formed)


def test_feedback_reasonable_magnitude():
    """Feedback has the magnitude epsilon times the star formation rate, and the run is leak-free

    At FeedbackReheatingEpsilon 3 the outflow rate satisfies the exact relation of
    assert_outflow_relation on every star-forming row, and is zero on rows without star formation.
    """
    halos, stdout, stderr = run_z0("snfb_standard", STANDARD)
    assert check_no_memory_leaks(stdout, stderr), "Should not have memory leaks"
    assert_outflow_relation(halos, 3.0)
    assert np.min(drained_rows(halos)) > 1.0 - 1e-4, "stars and reheating overdraw the cold gas"


def test_edge_case_very_high_efficiency():
    """The exact outflow relation holds at epsilon 50, where the renormalisation binds

    With epsilon 50 a galaxy's stars plus 50 times its stars exceed its cold gas almost
    everywhere, so sage_calculate_supernova_feedback rescales both. The cold gas is never
    overdrawn (drained_rows), and some rows are left with exactly the recycled stars as cold gas:
    their stars and reheated mass used up the cold gas.
    """
    halos, _stdout, _stderr = run_z0("snfb_high_efficiency", HIGH_EFFICIENCY)
    assert_outflow_relation(halos, 50.0)
    left = drained_rows(halos)
    assert np.min(left) > 1.0 - 1e-4, "stars and reheating overdraw the cold gas"
    drained = int(np.sum(np.abs(left - 1.0) < 1e-3))
    assert drained > 0, "no row has its cold gas used up: the renormalisation never binds"
    print(f"  {drained} rows have their cold gas used up by stars and reheating")


def test_ejection_efficiency_sensitivity():
    """FeedbackEjectionEfficiency drives the gas that reaches EjectedGas

    Only sage_calculate_supernova_feedback sends gas to EjectedGas in this pipeline, so with
    FeedbackEjectionEfficiency 0 no row holds any, while at 0.3 ejection happens: the standard
    run's EjectedGas is positive on some rows and none is negative.
    """
    standard, _stdout, _stderr = run_z0("snfb_standard", STANDARD)
    control, _stdout, _stderr = run_z0("snfb_no_ejection", NO_EJECTION)
    assert np.any(control["StarFormationRate"] > 0.0), "the control should form stars"
    assert np.all(control["EjectedGas"] == 0.0), "eta 0 should eject no gas"
    ejected = standard["EjectedGas"]
    assert np.all(ejected >= 0.0), "EjectedGas should be non-negative"
    assert np.any(ejected > 0.0), "eta 0.3 should eject gas on some rows"
    print(f"  {int(np.sum(ejected > 0.0))} rows hold ejected gas at eta 0.3, none at eta 0")


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [
                test_feedback_reasonable_magnitude,
                test_edge_case_very_high_efficiency,
                test_ejection_efficiency_sensitivity,
            ],
            "sage_calculate_supernova_feedback",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
