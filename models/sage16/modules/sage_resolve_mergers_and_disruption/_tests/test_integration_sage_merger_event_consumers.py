#!/usr/bin/env python3
"""
Immediate merger event consumer integration tests.

Validates the production runtime chain:
sage_resolve_mergers_and_disruption -> module_emit_event() -> process_per_event consumers.

Test cases:
  - test_quasar_consumer_receives_merger_events: removing the quasar consumer removes
    merger-driven black-hole growth (base run against the no-quasar run)
  - test_starburst_consumer_receives_merger_events: removing the burst consumer removes
    merger-driven bulge growth and burst star formation (base run against the no-burst run)

Each of the three configurations (base, no quasar, no burst) runs once and is shared between
the cases that read it.
"""

import sys
from pathlib import Path

import numpy as np

# Add tests directory to path to import framework
# This test is one level deeper than models/sage16/modules/_tests/ so needs 5 parent hops
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    MIMIC_EXE,
    SharedRuns,
    TestSkipped,
    load_binary_halos,
    run_test_suite,
)

MERGER_MODEL_PARAMS = {
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

PRE_TIMESTEP_PHASE = [
    ("sage_reionization", "process_full_halo"),
    ("sage_prepare_infall_budget", "process_full_halo"),
    ("sage_set_disk_scale_radius", "process_full_halo"),
    ("sage_initialise_merger_clock", "process_full_halo"),
]

GALAXY_PHYSICS_MODULES = [
    ("sage_apply_infall", "process_full_halo"),
    ("sage_reincorporation", "process_full_halo"),
    ("sage_satellite_stripping", "process_by_galaxy"),
    ("sage_calculate_cooling_budget", "process_by_galaxy"),
    ("sage_radio_mode_heating", "process_by_galaxy"),
    ("sage_apply_cooling", "process_by_galaxy"),
    ("sage_calculate_star_formation", "process_by_galaxy"),
    ("sage_calculate_supernova_feedback", "process_by_galaxy"),
    ("sage_apply_star_formation_supernova", "process_by_galaxy"),
    ("sage_disk_instability", "process_by_galaxy"),
    ("sage_quasar_mode", "process_by_galaxy"),
    ("sage_starburst_feedback", "process_by_galaxy"),
]


def build_phase_config(quasar_consumer=True, starburst_consumer=True):
    """Build the smallest deterministic pipeline that exercises merger events."""
    satellite_mergers = [("sage_resolve_mergers_and_disruption", "process_full_halo")]
    if quasar_consumer:
        satellite_mergers.append(("sage_quasar_mode", "process_per_event"))
    if starburst_consumer:
        satellite_mergers.append(("sage_starburst_feedback", "process_per_event"))

    return {
        "pre_timestep": PRE_TIMESTEP_PHASE,
        "galaxy_physics": GALAXY_PHYSICS_MODULES,
        "satellite_mergers": satellite_mergers,
        "post_timestep": [],
    }


# The three run configurations, one launch each
BASE_PHASES = build_phase_config()
NO_QUASAR_PHASES = build_phase_config(quasar_consumer=False)
NO_STARBURST_PHASES = build_phase_config(starburst_consumer=False)

RUNS = SharedRuns()


def merger_run(name, phase_config):
    """Return the shared run `name` of MERGER_MODEL_PARAMS, asserting it succeeded

    Skips the calling case when Mimic is not built.
    """
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run(name, phase_config=phase_config, model_params=MERGER_MODEL_PARAMS)
    assert run.returncode == 0, f"the {name} pipeline should succeed\nSTDERR: {run.stderr}"
    return run


def load_output_halos(output_dir):
    """Load the single-snapshot binary output for a test run."""
    output_file = output_dir / "model_z0.000_0"
    halos, _ = load_binary_halos(output_file)
    return halos


def merger_remnant_mask(halos):
    """Select galaxies that recorded a merger in the current run."""
    return (halos["TimeOfLastMinorMerger"] > 0.0) | (halos["TimeOfLastMajorMerger"] > 0.0)


def common_merger_field_values(halos_a, halos_b, field_name):
    """Return aligned field values for merger remnants present in both outputs."""
    subset_a = halos_a[merger_remnant_mask(halos_a)]
    subset_b = halos_b[merger_remnant_mask(halos_b)]

    order_a = np.argsort(subset_a["UniqueGalaxyID"])
    order_b = np.argsort(subset_b["UniqueGalaxyID"])

    ids_a = subset_a["UniqueGalaxyID"][order_a]
    ids_b = subset_b["UniqueGalaxyID"][order_b]

    common_ids, idx_a, idx_b = np.intersect1d(ids_a, ids_b, return_indices=True)

    values_a = subset_a[field_name][order_a][idx_a]
    values_b = subset_b[field_name][order_b][idx_b]
    return common_ids, values_a, values_b


def test_quasar_consumer_receives_merger_events():
    """Disabling the satellite_mergers quasar consumer must remove merger-driven BH growth."""
    base = merger_run("merger_event_base", BASE_PHASES)
    no_quasar = merger_run("merger_event_no_quasar", NO_QUASAR_PHASES)
    base_halos = load_output_halos(base.output_dir)
    no_quasar_halos = load_output_halos(no_quasar.output_dir)

    common_ids, base_bh, no_quasar_bh = common_merger_field_values(
        base_halos, no_quasar_halos, "BlackHoleMass"
    )
    assert len(common_ids) >= 100, "Test dataset should produce merger remnants"

    _, base_accretion, no_quasar_accretion = common_merger_field_values(
        base_halos, no_quasar_halos, "QuasarModeBHaccretionMass"
    )

    bh_delta = float(np.sum(base_bh - no_quasar_bh))
    accretion_delta = float(np.sum(base_accretion - no_quasar_accretion))

    # ~0.05 M_sun/h total BH mass added across merger remnants in the test snapshot
    assert (
        bh_delta > 0.05
    ), "satellite_mergers quasar consumer should increase remnant BH mass via merger events"
    # ~0.002 M_sun/h total accretion recorded across merger remnants
    assert (
        accretion_delta > 0.002
    ), "satellite_mergers quasar consumer should record merger-driven BH accretion"


def test_starburst_consumer_receives_merger_events():
    """Disabling the satellite_mergers starburst consumer must remove merger-driven bulge growth."""
    base = merger_run("merger_event_base", BASE_PHASES)
    no_starburst = merger_run("merger_event_no_starburst", NO_STARBURST_PHASES)
    base_halos = load_output_halos(base.output_dir)
    no_starburst_halos = load_output_halos(no_starburst.output_dir)

    common_ids, base_bulge, no_starburst_bulge = common_merger_field_values(
        base_halos, no_starburst_halos, "BulgeMass"
    )
    assert len(common_ids) >= 100, "Test dataset should produce merger remnants"

    _, base_stellar, no_starburst_stellar = common_merger_field_values(
        base_halos, no_starburst_halos, "StellarMass"
    )
    _, base_cold_gas, no_starburst_cold_gas = common_merger_field_values(
        base_halos, no_starburst_halos, "ColdGas"
    )

    bulge_delta = float(np.sum(base_bulge - no_starburst_bulge))
    stellar_delta = float(np.sum(base_stellar - no_starburst_stellar))
    cold_gas_delta = float(np.sum(base_cold_gas - no_starburst_cold_gas))

    # ~9 M_sun/h total bulge mass added across merger remnants in the test snapshot
    assert (
        bulge_delta > 5.0
    ), "satellite_mergers starburst consumer should increase merger-remnant bulge mass"
    # ~6.5 M_sun/h net stellar mass increase from burst star formation
    assert (
        stellar_delta > 5.0
    ), "satellite_mergers starburst consumer should form additional stars in merger remnants"
    # The burst net-consumes a few M_sun/h of cold gas. SAGE parity: the merger quasar wind
    # (grow_black_hole -> quasar_mode_wind) fires before the starburst in SAGE's
    # deal_with_galaxy_merger and ejects some cold gas first, so the net cold-gas reduction
    # attributable to the burst is a few M_sun/h (~-4.7 here).
    assert (
        cold_gas_delta < -3.0
    ), "satellite_mergers starburst consumer should net-consume cold gas in merger remnants"


def main():
    """Run the immediate merger event integration suite."""
    tests = [
        test_quasar_consumer_receives_merger_events,
        test_starburst_consumer_receives_merger_events,
    ]
    try:
        return run_test_suite(tests, Path(__file__).name)
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
