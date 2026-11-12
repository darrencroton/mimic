#!/usr/bin/env python3
"""
SAGE Apply Star Formation Supernova Module - Integration Test

Validates: the apply step's commit on real galaxies, through the whole chain that feeds and
follows it

Two runs are launched. The first is a pipeline that supplies cold gas: the disk scale radius,
reionization, infall and cooling (with radio-mode heating) feed the disk, then the star-formation
chain and the metal yield (sage_apply_metal_enrichment, which reads Yield and FracZleaveDisk). Its
assertions require the subject (star formation, ejection, metals) to be present, so none can pass
on an empty disk. The second is the shipped pipeline without sage_calculate_supernova_feedback,
where the apply step's own cold-gas cap is the only thing between star formation and a negative
ColdGas.

Test cases:
  - test_full_pipeline_conservation: on the supplied pipeline, metals are bounded by what was
    formed (at most the yield budget), every reservoir is populated, finite and non-negative, and
    star-forming galaxies hold stars
  - test_cold_gas_cap_without_supernova_calculator: ColdGas never negative without the SN
    calculator

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

# The gas supply before the SF chain: without the disk scale radius no disk forms stars, and
# without infall and cooling the disk holds no cold gas. The metal yield follows the apply step.
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
        ("sage_calculate_supernova_feedback", "process_by_galaxy"),
        ("sage_apply_star_formation_supernova", "process_by_galaxy"),
        ("sage_apply_metal_enrichment", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}

# Every key the configured modules read
PIPELINE_PARAMS = {
    # The gas supply
    "GlobalBaryonFraction": 0.17,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    # Star formation
    "SfrEfficiency": 0.02,
    "StarFormingDiskFactor": 3.0,
    # Feedback, recycling and yield
    "FeedbackReheatingEpsilon": 3.0,
    "FeedbackEjectionEfficiency": 0.3,
    "RecycleFraction": 0.43,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
}

# The shipped phases (sage16_mini-millennium.yaml) minus sage_calculate_supernova_feedback. No
# other module is dropped: nothing else names the calculator, and the apply step initialises
# without it (it only warns when neither calculator is configured).
NO_CALCULATOR_PHASES = {
    "pre_timestep": [
        ("sage_reionization", "process_full_halo"),
        ("sage_prepare_infall_budget", "process_full_halo"),
        ("sage_set_disk_scale_radius", "process_full_halo"),
        ("sage_initialise_merger_clock", "process_full_halo"),
    ],
    "galaxy_physics": [
        ("sage_apply_infall", "process_full_halo"),
        ("sage_reincorporation", "process_full_halo"),
        ("sage_satellite_stripping", "process_by_galaxy"),
        ("sage_calculate_cooling_budget", "process_by_galaxy"),
        ("sage_radio_mode_heating", "process_by_galaxy"),
        ("sage_apply_cooling", "process_by_galaxy"),
        ("sage_calculate_star_formation", "process_by_galaxy"),
        ("sage_apply_star_formation_supernova", "process_by_galaxy"),
        ("sage_disk_instability", "process_by_galaxy"),
        ("sage_quasar_mode", "process_by_galaxy"),
        ("sage_starburst_feedback", "process_by_galaxy"),
        ("sage_apply_metal_enrichment", "process_by_galaxy"),
    ],
    "satellite_mergers": [
        ("sage_resolve_mergers_and_disruption", "process_full_halo"),
        ("sage_quasar_mode", "process_per_event"),
        ("sage_starburst_feedback", "process_per_event"),
    ],
    "post_timestep": [],
}

# The shipped run file's parameters (the starburst module reads the feedback pair)
RUN_FILE_PARAMS = {
    "GlobalBaryonFraction": 0.17,
    "SfrEfficiency": 0.05,
    "StarFormingDiskFactor": 3.0,
    "FeedbackReheatingEpsilon": 3.0,
    "FeedbackEjectionEfficiency": 0.3,
    "ReIncorporationFactor": 0.15,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "BlackHoleGrowthRate": 0.015,
    "QuasarModeEfficiency": 0.005,
    "RecycleFraction": 0.43,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
    "ThresholdMajorMerger": 0.3,
    "ThresholdSatDisruption": 1.0,
}

# The output snapshots of the cold-gas cap run: the shipped run file's eight (the generated run
# file otherwise writes only the last)
CAP_SNAPSHOTS = [63, 37, 32, 27, 23, 20, 18, 16]

# The metal reservoirs are allowed a rounding floor: the ejection debits metals at the central's
# hot-gas metallicity with no cap, so a central whose hot gas all leaves keeps a float cancellation
# residue of order -1e-19 in MetalsHotGas (about 2000 rows on this pipeline). The residue is
# seven orders below -1e-12, which a metal transfer in the wrong direction would exceed by many
# orders; the gas and rate fields are held strictly to 0.
METALS_ROUNDING_FLOOR = -1.0e-12


def require_mimic():
    """Skip a case when Mimic is not built."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")


def test_full_pipeline_conservation():
    """The supplied pipeline forms stars and metals within what the yield and recycling allow

    Validates, on one run of PHASES:
    - Mimic runs to completion and reports no memory leak
    - Every mass and metal reservoir, and StarFormationRate, is finite and non-negative (the
      metal reservoirs to METALS_ROUNDING_FLOOR)
    - The box's metals are positive and at most Yield / (1 - RecycleFraction) times its
      StellarMass: the yield is the pipeline's only metal source (infall adds no metals), Yield
      times the stars formed, while StellarMass grows by (1 - RecycleFraction) times the stars
      formed and nothing removes stars (no merger or disruption module, so no galaxy is discarded,
      src/core/inheritance.c). Metals only move between reservoirs or leave with negative infall.
      This is an upper bound, not an exact budget: the pipeline's initial conditions are not
      observable, so exact mass and metal conservation is asserted by the unit tier
    - Every reservoir holds gas or stars on some rows, with non-negative metals. No upper bound
      on a reservoir's metallicity is asserted: a disk whose star formation and reheating use up
      its cold gas keeps R * stars of gas but gains Yield * stars of metals, so its metallicity
      can exceed 0.1
    - Star-forming galaxies exist and hold stars (the stars-formed bound and the zero rate without
      cold gas are asserted in sage_calculate_star_formation's integration test)
    """
    require_mimic()
    run = RUNS.run("supplied_pipeline", phase_config=PHASES, model_params=PIPELINE_PARAMS)
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")

    # Finite and non-negative reservoirs and rate; the metal reservoirs to a rounding floor
    for field in [
        "ColdGas",
        "HotGas",
        "EjectedGas",
        "StellarMass",
        "MetalsColdGas",
        "MetalsHotGas",
        "MetalsEjectedGas",
        "MetalsStellarMass",
        "StarFormationRate",
    ]:
        floor = METALS_ROUNDING_FLOOR if field.startswith("Metals") else 0.0
        assert np.all(np.isfinite(halos[field])), f"{field} should be finite"
        assert np.all(halos[field] >= floor), f"{field} should be non-negative"

    # The yield budget: the box's metals are at most Yield / (1 - RecycleFraction) * StellarMass
    total_metals = (
        halos["MetalsColdGas"]
        + halos["MetalsHotGas"]
        + halos["MetalsEjectedGas"]
        + halos["MetalsStellarMass"]
    )
    total_stellar = float(np.sum(halos["StellarMass"], dtype=np.float64))
    total_metal_mass = float(np.sum(total_metals, dtype=np.float64))
    yield_fraction = PIPELINE_PARAMS["Yield"]
    recycle_fraction = PIPELINE_PARAMS["RecycleFraction"]
    yield_budget = yield_fraction / (1.0 - recycle_fraction) * total_stellar
    print(f"  Total metals {total_metal_mass:.4e} of the yield budget {yield_budget:.4e}")
    assert total_metal_mass > 0.0, "the yield should produce metals"
    assert total_metal_mass <= yield_budget * (
        1.0 + 1e-5
    ), "the metals should not exceed the yield budget"

    # Every reservoir is populated, and its metals are non-negative where it holds mass
    for component in ["ColdGas", "HotGas", "EjectedGas", "StellarMass"]:
        populated = halos[component] > 1e-6
        assert np.sum(populated) > 0, f"some rows should hold {component}"
        assert np.all(
            halos[f"Metals{component}"][populated] >= METALS_ROUNDING_FLOOR
        ), f"{component} metals should be non-negative"

    # Star-forming galaxies exist and hold stars
    star_forming = halos["StarFormationRate"] > 0
    print(f"  Found {np.sum(star_forming)} star-forming galaxies")
    assert (
        np.sum(star_forming) > 0
    ), "some galaxies should form stars (the pipeline supplies cold gas)"
    assert np.all(
        halos["StellarMass"][star_forming] > 0
    ), "Star-forming galaxies should have stellar mass"


def test_cold_gas_cap_without_supernova_calculator():
    """Test that ColdGas never goes negative when the SN calculator is left out

    Validates:
    - With sage_calculate_supernova_feedback absent nothing else caps star formation,
      so the apply step must cap NewStellarMass at ColdGas (SAGE's renormalisation with
      reheated_mass = 0, sage-model/src/model_starformation_and_feedback.c:50-54)
    - No galaxy has ColdGas < 0 at any written snapshot, with SubSteps = 1 so that one
      substep spans many dynamical times and star formation can exceed the cold gas. ColdGas is
      the only reservoir the defect drives negative here: with no calculator the reheated and
      ejected masses are zero, so no other reservoir is debited

    Pipeline: NO_CALCULATOR_PHASES at the shipped run file's parameters, writing the eight
    snapshots of CAP_SNAPSHOTS.

    Observed minimum ColdGas (output units, 1e10 Msun/h) over the eight written snapshots:
    -3.024462 before the cap (the case fails), 0.0 after it (the case passes).
    """
    require_mimic()
    run = RUNS.run(
        "no_calculator_cap",
        phase_config=NO_CALCULATOR_PHASES,
        model_params=RUN_FILE_PARAMS,
        substeps=1,
        snapshot_list=CAP_SNAPSHOTS,
    )
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"

    output_files = sorted(run.output_dir.glob("model_z*_0"))
    assert len(output_files) == len(
        CAP_SNAPSHOTS
    ), f"Expected {len(CAP_SNAPSHOTS)} snapshot outputs, found {len(output_files)}"

    minimum_cold_gas = np.inf
    for output_file in output_files:
        halos, _ = load_binary_halos(output_file)
        minimum_cold_gas = min(minimum_cold_gas, float(np.min(halos["ColdGas"])))
    print(f"  Snapshots written: {len(output_files)}")
    print(f"  Minimum ColdGas over all snapshots: {minimum_cold_gas:.6e}")

    assert minimum_cold_gas >= 0.0, (
        f"ColdGas went negative without the SN calculator (minimum {minimum_cold_gas:.6e}); "
        "star formation must be capped at the cold gas"
    )


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [
                test_full_pipeline_conservation,
                test_cold_gas_cap_without_supernova_calculator,
            ],
            "SAGE Apply Star Formation Supernova Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
