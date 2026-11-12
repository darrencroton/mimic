#!/usr/bin/env python3
"""
SAGE26 Calculate Supernova Feedback Module - Integration Test

Validates: the module end to end, on a pipeline that supplies cold gas

The pipeline is the one sage26_calculate_star_formation's integration test uses: the disk scale
radius, reionization, infall and cooling (with radio-mode heating) feed the disk before the
star-formation chain, and sage26_finalize_cooling_heating turns the reheated mass accumulated in
SupernovaOutflowRate into the rate it is written as (Msun/yr). Each case requires star formation
to have happened, so none can pass on an empty disk. FIREmodeOn is 0 (the fixed-epsilon feedback)
and SFprescription 0. Three Mimic runs, one per configuration:

  - the standard run (FeedbackReheatingEpsilon 3, FeedbackEjectionEfficiency 0.3)
  - the same with FeedbackEjectionEfficiency 0, the control that shows ejection
  - a high-efficiency run (epsilon 50, eta 10), where the renormalisation binds

Test cases:
  - test_feedback_reasonable_magnitude: on every star-forming row the outflow rate is exactly
    epsilon times the star formation rate (scaled by the rows' time intervals); no outflow without
    star formation; the run is leak-free
  - test_edge_case_very_high_efficiency: the same exact relation at epsilon 50, which holds only
    if the renormalisation kept the reheated mass within the cold gas
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

# One substep per snapshot interval (the harness default, set explicitly because drained_rows()
# below depends on it): StarFormationRate * dT is the stars formed over the whole interval, which
# bounds the cold gas left only when the interval is a single substep. With more substeps a row
# could be left with less cold gas than RecycleFraction times the interval's stars.
SUBSTEPS = 1

# The pipeline. Without the disk scale radius no disk forms stars, and without infall and cooling
# the disk holds no cold gas, so neither star formation nor its feedback happens.
# SupernovaOutflowRate accumulates the reheated mass undivided; only
# sage26_finalize_cooling_heating divides it by the snapshot interval, making it the rate in
# Msun/yr it is written as (model_properties.yaml).
PHYSICS_PHASES = {
    "pre_timestep": [
        ("sage26_set_disk_scale_radius", "process_full_halo"),
        ("sage26_reionization", "process_full_halo"),
        ("sage26_prepare_infall_budget", "process_full_halo"),
    ],
    "galaxy_physics": [
        ("sage26_apply_infall", "process_full_halo"),
        ("sage26_calculate_cooling_budget", "process_by_galaxy"),
        ("sage26_radio_mode_heating", "process_by_galaxy"),
        ("sage26_apply_cooling", "process_by_galaxy"),
        ("sage26_calculate_star_formation", "process_by_galaxy"),
        ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
        ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [("sage26_finalize_cooling_heating", "process_full_halo")],
}


def feedback_params(epsilon, eta):
    """Every key the pipeline reads, with the feedback's FeedbackReheatingEpsilon and eta."""
    return {
        # The gas supply (SFprescription 0)
        "GlobalBaryonFraction": 0.17,
        "AGNrecipe": 2,
        "RadioModeEfficiency": 0.08,
        "BulgeSizeOn": 0,
        # Required by the SF module (SFprescription also by the apply step)
        "SFprescription": 0,
        "CGMrecipeOn": 0,  # required (the CGM recipe off)
        "H2RadialIntegrationOn": 1,
        "H2RadialNBins": 25,
        "H2RadialRMaxFactor": 5.0,
        "SfrEfficiency": 0.02,
        "StarFormingDiskFactor": 3.0,
        "FeedbackReheatingEpsilon": epsilon,
        "FeedbackEjectionEfficiency": eta,
        "FIREmodeOn": 0,  # required by the SN module (the fixed-epsilon feedback)
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

    With FIREmodeOn 0 the reheated mass is FeedbackReheatingEpsilon times the stars formed, both
    scaled together when they exceed the cold gas (sage26_calculate_supernova_feedback), and the
    apply step's clamp to the remaining cold gas never binds (stars + reheated <= ColdGas leaves
    at least the reheated mass after star formation). StarFormationRate is the stars formed over
    the row's own dT; sage26_finalize_cooling_heating divides SupernovaOutflowRate by the dT of
    the FoF's first galaxy, its central. So on every star-forming row

        SupernovaOutflowRate * dT(central) = epsilon * StarFormationRate * dT(row)

    (both fields share the writer conversion to Msun/yr), and the ratio of the two rates is
    epsilon on a central and epsilon * dT(row) / dT(central) on a satellite carried from an older
    snapshot. Were the renormalisation missing, a row whose stars and reheated mass exceed its
    cold gas would have its reheated mass clamped by the apply step, and the relation would fail
    there. Rows without star formation have no outflow.
    """
    sfr = halos["StarFormationRate"].astype(np.float64)
    snfb = halos["SupernovaOutflowRate"].astype(np.float64)
    assert np.all(np.isfinite(snfb)), "SupernovaOutflowRate should be finite"
    assert np.all(snfb >= 0.0), "SupernovaOutflowRate should be non-negative"

    has_sf = sfr > 0.0
    assert np.any(has_sf), "some galaxies should form stars (the pipeline supplies cold gas)"
    dt = halos["dT"].astype(np.float64)
    centrals = halos["Type"] == 0
    central_ids = halos["UniqueGalaxyID"][centrals]
    order = np.argsort(central_ids)
    position = np.searchsorted(central_ids[order], halos["UniqueCentralGalaxyID"])
    assert np.all(
        central_ids[order][position] == halos["UniqueCentralGalaxyID"]
    ), "every row's FoF central should be written"
    central_dt = dt[centrals][order][position]
    outflow = snfb[has_sf] * central_dt[has_sf]
    expected = epsilon * sfr[has_sf] * dt[has_sf]
    # 1e-5 relative: both are float stores
    off = np.abs(outflow - expected) > 1e-5 * expected
    assert not np.any(off), (
        f"{int(np.sum(off))} star-forming rows break SupernovaOutflowRate * dT(central) = "
        f"epsilon * StarFormationRate * dT (largest relative error "
        f"{np.max(np.abs(outflow / expected - 1.0)):.3e})"
    )
    print(f"  {int(np.sum(has_sf))} star-forming rows satisfy the relation at epsilon {epsilon}")

    no_sf = ~has_sf
    assert np.any(no_sf), "some galaxies should form no stars"
    assert np.all(snfb[no_sf] == 0.0), "Galaxies without star formation should have no outflow"


def drained_rows(halos):
    """Return the ratio of ColdGas to RecycleFraction times the stars formed, on star-forming rows.

    Star formation takes (1 - RecycleFraction) of the stars formed out of the cold gas and
    feedback takes the reheated mass, so a row whose stars and reheated mass use up the cold gas
    (the renormalisation binds, stars + reheated = cold gas) is left with exactly
    RecycleFraction times the stars formed (the stars are in 10^6 Msun per Msun/yr and Myr, a
    factor 10^-4 to the code's 10^10 Msun, as in sage26_calculate_star_formation's integration
    test). At SUBSTEPS 1 a row cannot be left with less: without the renormalisation the apply
    step would clamp the reheated mass at the cold gas left after star formation and the cold gas
    would be 0.
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
    assert np.min(drained_rows(halos)) > 1.0 - 1e-4, "stars and reheating exceed the cold gas"


def test_edge_case_very_high_efficiency():
    """The exact outflow relation holds at epsilon 50, where the renormalisation binds

    With epsilon 50 a galaxy's stars plus 50 times its stars exceed its cold gas almost
    everywhere, so sage26_calculate_supernova_feedback rescales both. The relation of
    assert_outflow_relation survives only because the rescaled budget fits in the cold gas, and
    some rows are left with exactly the recycled stars as cold gas (drained_rows): their stars
    and reheated mass used up the cold gas.
    """
    halos, _stdout, _stderr = run_z0("snfb_high_efficiency", HIGH_EFFICIENCY)
    assert_outflow_relation(halos, 50.0)
    left = drained_rows(halos)
    assert np.min(left) > 1.0 - 1e-4, "stars and reheating exceed the cold gas"
    drained = int(np.sum(np.abs(left - 1.0) < 1e-3))
    assert drained > 0, "no row has its cold gas used up: the renormalisation never binds"
    print(f"  {drained} rows have their cold gas used up by stars and reheating")


def test_ejection_efficiency_sensitivity():
    """FeedbackEjectionEfficiency drives the gas that reaches EjectedGas

    Only sage26_calculate_supernova_feedback sends gas to EjectedGas in this pipeline, so with
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
            "sage26_calculate_supernova_feedback",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
