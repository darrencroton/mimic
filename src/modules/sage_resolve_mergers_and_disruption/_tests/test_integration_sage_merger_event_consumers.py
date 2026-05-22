#!/usr/bin/env python3
"""
Immediate merger event consumer integration tests.

Validates the production runtime chain:
sage_resolve_mergers_and_disruption -> module_emit_event() -> process_per_event consumers.
"""

import shutil
import sys
from pathlib import Path

import numpy as np

# Add tests directory to path to import framework
# This test is one level deeper than src/modules/_tests/ so needs 5 parent hops
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import create_test_param_file, load_binary_halos, run_mimic


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

PHASE_1_MODULES = [
    ("sage_apply_infall", "process_full_halo"),
    ("sage_reincorporation", "process_full_halo"),
    ("sage_satellite_stripping", "process_full_halo"),
    ("sage_calculate_cooling_budget", "process_by_galaxy"),
    ("sage_radio_mode_heating", "process_by_galaxy"),
    ("sage_apply_cooling", "process_by_galaxy"),
    ("sage_star_formation", "process_by_galaxy"),
    ("sage_supernova_feedback", "process_by_galaxy"),
    ("sage_apply_star_formation_supernova", "process_by_galaxy"),
    ("sage_disk_instability", "process_by_galaxy"),
    ("sage_quasar_mode", "process_by_galaxy"),
    ("sage_starburst_feedback", "process_by_galaxy"),
]


def build_phase_config(merger_mode="process_full_halo",
                       enable_phase2_quasar=True,
                       enable_phase2_starburst=True):
    """Build the smallest deterministic pipeline that exercises merger events."""
    phase_2 = [("sage_resolve_mergers_and_disruption", merger_mode)]
    if enable_phase2_quasar:
        phase_2.append(("sage_quasar_mode", "process_per_event"))
    if enable_phase2_starburst:
        phase_2.append(("sage_starburst_feedback", "process_per_event"))

    return {
        "pre_timestep": PRE_TIMESTEP_PHASE,
        "phase_1": PHASE_1_MODULES,
        "phase_2": phase_2,
        "post_timestep": [],
    }


def run_merger_pipeline(output_name, merger_mode="process_full_halo",
                        enable_phase2_quasar=True,
                        enable_phase2_starburst=True):
    """Execute the immediate-merger pipeline on the standard test dataset."""
    return create_test_param_file(
        output_name=output_name,
        phase_config=build_phase_config(
            merger_mode=merger_mode,
            enable_phase2_quasar=enable_phase2_quasar,
            enable_phase2_starburst=enable_phase2_starburst,
        ),
        model_params=MERGER_MODEL_PARAMS,
    )


def load_output_halos(output_dir):
    """Load the single-snapshot binary output for a test run."""
    output_file = output_dir / "model_z0.000_0"
    halos, _ = load_binary_halos(output_file)
    return halos


def merger_remnant_mask(halos):
    """Select galaxies that recorded a merger in the current run."""
    return (halos["TimeOfLastMinorMerger"] > 0.0) | (
        halos["TimeOfLastMajorMerger"] > 0.0
    )


def common_merger_field_values(halos_a, halos_b, field_name):
    """Return aligned field values for merger remnants present in both outputs."""
    subset_a = halos_a[merger_remnant_mask(halos_a)]
    subset_b = halos_b[merger_remnant_mask(halos_b)]

    order_a = np.argsort(subset_a["UniqueGalaxyID"])
    order_b = np.argsort(subset_b["UniqueGalaxyID"])

    ids_a = subset_a["UniqueGalaxyID"][order_a]
    ids_b = subset_b["UniqueGalaxyID"][order_b]

    common_ids, idx_a, idx_b = np.intersect1d(
        ids_a, ids_b, return_indices=True
    )

    values_a = subset_a[field_name][order_a][idx_a]
    values_b = subset_b[field_name][order_b][idx_b]
    return common_ids, values_a, values_b


def test_invalid_processing_mode_rejected():
    """Immediate merger handler must reject non-full-halo configuration at startup."""
    param_file, output_dir, temp_dir = run_merger_pipeline(
        output_name="merger_event_invalid_mode",
        merger_mode="process_by_galaxy",
    )

    try:
        returncode, stdout, stderr = run_mimic(param_file)
        combined_output = stdout + stderr

        assert returncode != 0, "Invalid processing mode should fail startup"
        assert "does not support processing mode 'process_by_galaxy'" in combined_output
        assert "Supported modes: process_full_halo" in combined_output
    finally:
        shutil.rmtree(temp_dir)


def test_quasar_consumer_receives_merger_events():
    """Disabling the phase_2 quasar consumer must remove merger-driven BH growth."""
    base_param_file, base_output_dir, base_temp_dir = run_merger_pipeline(
        output_name="merger_event_quasar_base"
    )
    no_quasar_param_file, no_quasar_output_dir, no_quasar_temp_dir = (
        run_merger_pipeline(
            output_name="merger_event_quasar_disabled",
            enable_phase2_quasar=False,
        )
    )

    try:
        returncode, stdout, stderr = run_mimic(base_param_file)
        assert returncode == 0, f"Baseline merger pipeline should succeed\nSTDERR: {stderr}"

        returncode, stdout, stderr = run_mimic(no_quasar_param_file)
        assert returncode == 0, (
            "Comparison pipeline without phase_2 quasar consumer should succeed\n"
            f"STDERR: {stderr}"
        )

        base_halos = load_output_halos(base_output_dir)
        no_quasar_halos = load_output_halos(no_quasar_output_dir)

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
        assert bh_delta > 0.05, (
            "Phase_2 quasar consumer should increase remnant BH mass via merger events"
        )
        # ~0.002 M_sun/h total accretion recorded across merger remnants
        assert accretion_delta > 0.002, (
            "Phase_2 quasar consumer should record merger-driven BH accretion"
        )
    finally:
        shutil.rmtree(base_temp_dir)
        shutil.rmtree(no_quasar_temp_dir)


def test_starburst_consumer_receives_merger_events():
    """Disabling the phase_2 starburst consumer must remove merger-driven bulge growth."""
    base_param_file, base_output_dir, base_temp_dir = run_merger_pipeline(
        output_name="merger_event_starburst_base"
    )
    no_starburst_param_file, no_starburst_output_dir, no_starburst_temp_dir = (
        run_merger_pipeline(
            output_name="merger_event_starburst_disabled",
            enable_phase2_starburst=False,
        )
    )

    try:
        returncode, stdout, stderr = run_mimic(base_param_file)
        assert returncode == 0, f"Baseline merger pipeline should succeed\nSTDERR: {stderr}"

        returncode, stdout, stderr = run_mimic(no_starburst_param_file)
        assert returncode == 0, (
            "Comparison pipeline without phase_2 starburst consumer should succeed\n"
            f"STDERR: {stderr}"
        )

        base_halos = load_output_halos(base_output_dir)
        no_starburst_halos = load_output_halos(no_starburst_output_dir)

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

        # ~5 M_sun/h total bulge mass added across merger remnants in the test snapshot
        assert bulge_delta > 5.0, (
            "Phase_2 starburst consumer should increase merger-remnant bulge mass"
        )
        # ~5 M_sun/h net stellar mass increase from burst star formation
        assert stellar_delta > 5.0, (
            "Phase_2 starburst consumer should form additional stars in merger remnants"
        )
        # ~5 M_sun/h cold gas consumed by burst star formation
        assert cold_gas_delta < -5.0, (
            "Phase_2 starburst consumer should consume cold gas in merger remnants"
        )
    finally:
        shutil.rmtree(base_temp_dir)
        shutil.rmtree(no_starburst_temp_dir)


def main():
    """Run the immediate merger event integration suite."""
    try:
        test_invalid_processing_mode_rejected()
        test_quasar_consumer_receives_merger_events()
        test_starburst_consumer_receives_merger_events()
        return 0
    except AssertionError as exc:
        print(f"Test failed: {exc}")
        return 1
    except Exception as exc:
        print(f"Unexpected error: {exc}")
        import traceback

        traceback.print_exc()
        return 1


if __name__ == "__main__":
    sys.exit(main())
