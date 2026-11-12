#!/usr/bin/env python3
"""
SAGE26 Apply Star Formation Supernova Module - Integration Test

Validates: the apply step's commit on real galaxies, through the whole chain that feeds and
follows it

Two runs are launched. The first is a pipeline that supplies cold gas (as
sage26_calculate_star_formation's integration test does): the disk scale radius, reionization,
infall and cooling (with radio-mode heating) feed the disk, then the star-formation chain and the
metal yield (sage26_apply_metal_enrichment, which reads Yield and FracZleaveDisk). Its assertions
require the subject (star formation, ejection, metals) to be present, so none can pass on an empty
disk. The second is the run files' pipeline without sage26_calculate_supernova_feedback, where the
apply step's own cold-gas cap is the only thing between star formation and a negative reservoir.

Test cases:
  - test_full_pipeline_conservation: on the supplied pipeline, stars and metals are bounded by what
    was formed (retained stars at most StellarMass, metals at most the yield budget), every
    reservoir is populated, finite and non-negative, and a galaxy without cold gas forms no stars
  - test_cold_gas_cap_without_supernova_calculator: no negative reservoir or outflow rate without
    the SN calculator

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
        ("sage26_apply_metal_enrichment", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}

# Every key the configured modules read (SFprescription 0, the CGM recipe off, the fixed-epsilon
# feedback)
PIPELINE_PARAMS = {
    # The gas supply
    "GlobalBaryonFraction": 0.17,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "BulgeSizeOn": 0,
    "CGMrecipeOn": 0,
    # Star formation
    "SFprescription": 0,
    "H2RadialIntegrationOn": 1,
    "H2RadialNBins": 25,
    "H2RadialRMaxFactor": 5.0,
    "SfrEfficiency": 0.02,
    "StarFormingDiskFactor": 3.0,
    # Feedback, recycling and yield
    "FeedbackReheatingEpsilon": 3.0,
    "FeedbackEjectionEfficiency": 0.3,
    "FIREmodeOn": 0,
    "RecycleFraction": 0.43,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
}

# The run files' pipeline (models/sage26/input/sage26_mini-millennium.yaml) without
# sage26_calculate_supernova_feedback. No other module needs dropping: nothing else requires the
# calculator, and the apply step initialises without it (it warns only when neither prescription
# module is configured).
NO_CALCULATOR_PHASES = {
    "pre_timestep": [
        ("sage26_initialise_merger_clock", "process_full_halo"),
        ("sage26_set_halo_concentration", "process_full_halo"),
        ("sage26_set_disk_scale_radius", "process_full_halo"),
        ("sage26_reionization", "process_full_halo"),
        ("sage26_determine_regime", "process_full_halo"),
        ("sage26_prepare_infall_budget", "process_full_halo"),
        ("sage26_determine_ffb_regime", "process_full_halo"),
    ],
    "galaxy_physics": [
        ("sage26_apply_infall", "process_full_halo"),
        ("sage26_reincorporation", "process_full_halo"),
        ("sage26_satellite_stripping", "process_by_galaxy"),
        ("sage26_calculate_cooling_budget", "process_by_galaxy"),
        ("sage26_radio_mode_heating", "process_by_galaxy"),
        ("sage26_apply_cooling", "process_by_galaxy"),
        ("sage26_regime_aware_cooling", "process_by_galaxy"),
        ("sage26_ffb_star_formation", "process_by_galaxy"),
        ("sage26_calculate_star_formation", "process_by_galaxy"),
        ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
        ("sage26_disk_instability", "process_by_galaxy"),
        ("sage26_quasar_mode", "process_by_galaxy"),
        ("sage26_starburst_feedback", "process_by_galaxy"),
        ("sage26_apply_metal_enrichment", "process_by_galaxy"),
    ],
    "satellite_mergers": [
        ("sage26_resolve_mergers_and_disruption", "process_full_halo"),
        ("sage26_quasar_mode", "process_per_event"),
        ("sage26_starburst_feedback", "process_per_event"),
    ],
    "post_timestep": [
        ("sage26_finalize_cooling_heating", "process_full_halo"),
        ("sage26_finalize_sfr_histories", "process_full_halo"),
        ("sage26_record_transition_state", "process_full_halo"),
    ],
}

# The run files' parameters
RUN_FILE_PARAMS = {
    "GlobalBaryonFraction": 0.17,
    "CGMrecipeOn": 1,
    "MShockMsun": 6.0e11,
    "EnhancedStarFormationOn": 1,
    "EnhancedSFEfficiency": 0.2,
    "ConcentrationOn": 3,
    "SfrEfficiency": 0.05,
    "StarFormingDiskFactor": 3.0,
    "SFprescription": 1,
    "H2RadialIntegrationOn": 1,
    "H2RadialNBins": 25,
    "H2RadialRMaxFactor": 5.0,
    "StarburstColdGasOn": 1,
    "FeedbackReheatingEpsilon": 2.9,
    "FeedbackEjectionEfficiency": 0.3,
    "FIREmodeOn": 1,
    "ReIncorporationFactor": 0.19,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "BlackHoleGrowthRate": 0.015,
    "QuasarModeEfficiency": 0.005,
    "RecycleFraction": 0.43,
    "Yield": 0.025,
    "FracZleaveDisk": 0.0,
    "ThresholdMajorMerger": 0.3,
    "ThresholdSatDisruption": 1.0,
    "MergerTimeFactor": 2.0,
    "BulgeSizeOn": 3,
}

# The run files' output snapshots, all written so that the cap is checked at every epoch
SNAPSHOT_LIST = [63, 37, 32, 27, 23, 20, 18, 16]

# The reservoirs and rate an uncapped star formation drives negative, through the feedback step
NON_NEGATIVE_FIELDS = (
    "ColdGas",
    "HotGas",
    "MetalsHotGas",
    "CGMgas",
    "MetalsCGMgas",
    "EjectedGas",
    "MetalsEjectedGas",
    "SupernovaOutflowRate",
)

# The metal fields are allowed a rounding floor: MetalsEjectedGas carries float cancellation
# residues of at most about 6e-18 on rows whose EjectedGas is 0 (the metals left when all the gas
# moves), at SubSteps 1 with or without the calculator (-5.5e-20 with it, where nothing goes
# negative otherwise), and the other metal fields can carry the same residue on another libm. The
# defect the cap prevents drives the gas reservoirs to order -1 and MetalsHotGas to order -5e-4,
# so -1e-12 separates the two; the gas and rate fields are held strictly to 0
METALS_ROUNDING_FLOOR = -1.0e-12


def require_mimic():
    """Skip a case when Mimic is not built."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")


def test_full_pipeline_conservation():
    """The supplied pipeline forms stars and metals within what the yield and recycling allow

    Validates, on one run of PHASES:
    - Mimic runs to completion and reports no memory leak
    - Every mass and metal reservoir, and StarFormationRate, is finite and non-negative
    - The box's metals are positive and at most Yield / (1 - RecycleFraction) times its
      StellarMass: the yield is the pipeline's only metal source (infall adds no metals), Yield
      times the stars formed, while StellarMass grows by (1 - RecycleFraction) times the stars
      formed and nothing removes stars (no merger or disruption module, so no galaxy is discarded,
      src/core/inheritance.c). Metals only move between reservoirs or leave with negative infall.
      This is an upper bound, not an exact budget: the pipeline's initial conditions are not
      observable, so exact mass and metal conservation is asserted by the unit tier
    - Every reservoir holds gas or stars on some rows, with non-negative metallicity. No upper
      bound on a reservoir's metallicity is asserted: a disk whose star formation and reheating
      use up its cold gas keeps R * stars of gas but gains Yield * stars of metals, so its
      metallicity rises by Yield / R per such event (0.058 here) and can exceed 0.1
      (ColdGas reaches 0.13 on the test trees)
    - Star-forming galaxies exist and hold stars. The stars formed over the snapshot, net of
      recycling, are held by the galaxy: feedback moves gas only and no merger or disruption
      module removes stars, so (1 - RecycleFraction) times the stars formed is at most
      StellarMass. StarFormationRate (Msun/yr) times dT (Myr) is the stars formed in 10^6 Msun, so
      a factor 10^-4 gives the code's 10^10 Msun (the writer conversions of both fields,
      model_properties.yaml and src/core/core_properties.yaml)
    - Galaxies without cold gas exist, and have a zero StarFormationRate
    """
    require_mimic()
    run = RUNS.run("supplied_pipeline", phase_config=PHASES, model_params=PIPELINE_PARAMS)
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"
    assert check_no_memory_leaks(run.stdout, run.stderr), "Should not have memory leaks"
    halos, _metadata = load_binary_halos(run.output_dir / "model_z0.000_0")

    # Finite and non-negative reservoirs and rate
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
        assert np.all(np.isfinite(halos[field])), f"{field} should be finite"
        assert np.all(halos[field] >= 0.0), f"{field} should be non-negative"

    # The yield budget: the box's metals are at most Yield / (1 - RecycleFraction) * StellarMass
    total_metals = (
        halos["MetalsColdGas"]
        + halos["MetalsHotGas"]
        + halos["MetalsEjectedGas"]
        + halos["MetalsStellarMass"]
    )
    total_stellar = float(np.sum(halos["StellarMass"], dtype=np.float64))
    total_metal_mass = float(np.sum(total_metals, dtype=np.float64))
    yield_budget = 0.025 / (1.0 - 0.43) * total_stellar  # this run's Yield and RecycleFraction
    print(f"  Total metals {total_metal_mass:.4e} of the yield budget {yield_budget:.4e}")
    assert total_metal_mass > 0.0, "the yield should produce metals"
    assert total_metal_mass <= yield_budget * (
        1.0 + 1e-5
    ), "the metals should not exceed the yield budget"

    # Every reservoir is populated, with non-negative metallicity
    for component in ["ColdGas", "HotGas", "EjectedGas", "StellarMass"]:
        mass = halos[component]
        metals = halos[f"Metals{component}"]
        non_zero = mass > 1e-6
        assert np.sum(non_zero) > 0, f"some rows should hold {component}"
        metallicity = np.where(non_zero, metals / np.where(non_zero, mass, 1.0), 0.0)
        assert np.all(
            metallicity[non_zero] >= 0.0
        ), f"{component} metallicity should be non-negative"

    # Star-forming galaxies exist, hold stars, and hold at least the stars they formed net of
    # recycling
    star_forming = halos["StarFormationRate"] > 0
    print(f"  Found {np.sum(star_forming)} star-forming galaxies")
    assert (
        np.sum(star_forming) > 0
    ), "some galaxies should form stars (the pipeline supplies cold gas)"
    assert np.all(
        halos["StellarMass"][star_forming] > 0
    ), "Star-forming galaxies should have stellar mass"
    sfr_sf = halos["StarFormationRate"][star_forming].astype(np.float64)
    formed = sfr_sf * halos["dT"][star_forming].astype(np.float64) * 1e-4
    retained = (1.0 - 0.43) * formed  # this run's RecycleFraction
    stellar_mass = halos["StellarMass"][star_forming].astype(np.float64)
    # 1e-5 relative: float stores; a galaxy whose stars all formed this snapshot is at equality
    excess = retained > stellar_mass * (1.0 + 1e-5)
    assert not np.any(excess), (
        f"{int(np.sum(excess))} rows formed more stars this snapshot than they hold "
        f"(largest ratio {np.max(retained / stellar_mass):.6f})"
    )

    # A galaxy with no cold gas forms no stars
    zero_cold = halos["ColdGas"] < 1e-10
    print(f"  Galaxies with zero cold gas: {np.sum(zero_cold)}")
    assert np.sum(zero_cold) > 0, "some galaxies should hold no cold gas"
    assert np.all(
        halos["StarFormationRate"][zero_cold] == 0.0
    ), "Galaxies with no cold gas should have zero SFR"


def test_cold_gas_cap_without_supernova_calculator():
    """Test that star formation is capped at the cold gas when the SN calculator is left out

    Validates:
    - No galaxy at any written snapshot has a negative ColdGas, HotGas, MetalsHotGas, CGMgas,
      MetalsCGMgas, EjectedGas, MetalsEjectedGas or SupernovaOutflowRate, with SubSteps = 1 so
      that one substep spans many dynamical times and star formation can exceed the cold gas

    Mechanism: without sage26_calculate_supernova_feedback nothing caps star formation (its cap is
    SAGE26's renormalisation), so the commit can leave the cold gas negative. The feedback step
    then clamps the reheated mass to that negative cold gas (SAGE26 itself stops there on an
    assertion, which this module does not carry), which resets ColdGas to 0 and ships the deficit
    to the FoF central as a negative reheat, and the ejection cap carries it into EjectedGas once
    it exceeds the central's reservoir, so the defect appears in the central's reservoirs and
    outflow rate rather than in ColdGas. The apply step therefore caps NewStellarMass at ColdGas.

    Pipeline: NO_CALCULATOR_PHASES, the run files' phases minus the calculator, at the run files'
    parameters, writing the run files' eight snapshots (SNAPSHOT_LIST).

    The three metal fields are held to METALS_ROUNDING_FLOOR rather than 0 (the floor's comment).
    """
    require_mimic()
    run = RUNS.run(
        "no_calculator_cap",
        phase_config=NO_CALCULATOR_PHASES,
        model_params=RUN_FILE_PARAMS,
        substeps=1,
        snapshot_list=SNAPSHOT_LIST,
    )
    assert run.returncode == 0, f"Mimic should execute successfully\nSTDERR: {run.stderr}"

    output_files = sorted(run.output_dir.glob("model_z*_0"))
    assert len(output_files) == len(
        SNAPSHOT_LIST
    ), f"Expected {len(SNAPSHOT_LIST)} snapshot outputs, found {len(output_files)}"

    minima = {field: np.inf for field in NON_NEGATIVE_FIELDS}
    negative_rows = {field: 0 for field in NON_NEGATIVE_FIELDS}
    for output_file in output_files:
        halos, _ = load_binary_halos(output_file)
        for field in NON_NEGATIVE_FIELDS:
            assert field in halos.dtype.names, f"the output should carry {field}"
            values = halos[field].astype(np.float64)
            minima[field] = min(minima[field], float(np.min(values)))
            negative_rows[field] += int(np.sum(values < 0.0))
    for field in NON_NEGATIVE_FIELDS:
        print(f"  {field}: minimum {minima[field]:.6g} ({negative_rows[field]} negative rows)")

    floors = {field: 0.0 for field in NON_NEGATIVE_FIELDS}
    for field in ("MetalsHotGas", "MetalsCGMgas", "MetalsEjectedGas"):
        floors[field] = METALS_ROUNDING_FLOOR
    negative = [
        f"{field} {minima[field]:.6g}"
        for field in NON_NEGATIVE_FIELDS
        if minima[field] < floors[field]
    ]
    assert not negative, (
        "without the SN calculator these went negative: " + ", ".join(negative) + "; star "
        "formation must be capped at the cold gas"
    )


def main():
    """Run this file's tests via the shared framework runner."""
    try:
        return run_test_suite(
            [
                test_full_pipeline_conservation,
                test_cold_gas_cap_without_supernova_calculator,
            ],
            "SAGE26 Apply Star Formation Supernova Integration Tests",
        )
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
