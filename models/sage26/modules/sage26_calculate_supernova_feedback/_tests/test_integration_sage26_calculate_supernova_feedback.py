#!/usr/bin/env python3
"""
SAGE Calculate Supernova Feedback Module - Integration Test

Validates: Module lifecycle, configuration, pipeline integration, and physics correctness

This test validates both software quality and physics correctness:

**Software Quality Tests**:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- Module works in pipeline with sage26_calculate_star_formation

**Physics Validation Tests** run on a pipeline that supplies cold gas (as
sage26_calculate_star_formation's integration test does): the disk scale radius, reionization,
infall and cooling (with radio-mode heating) feed the disk before the star-formation chain, and
sage26_finalize_cooling_heating turns the reheated mass accumulated in SupernovaOutflowRate into the
rate it is written as (Msun/yr). Each requires star formation to have happened, so none can pass on
an empty disk:
- Feedback properties have physically reasonable values
- Feedback is proportional to star formation
- Mass conservation (implicit in renormalization)
- Non-negative constraints enforced
- Edge cases produce expected behavior

Reference: Croton et al. (2006, 2016) - SAGE model

Author: Mimic Development Team
Date: 2025-12-18 (Refactored for comprehensive physics validation)
"""

import shutil
import sys
from pathlib import Path

import numpy as np

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (
    BLUE,
    GREEN,
    MIMIC_EXE,
    NC,
    RED,
    TestSkipped,
    check_no_memory_leaks,
    create_test_param_file,
    load_binary_halos,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
)

# The physics tests' pipeline. Without the disk scale radius no disk forms stars, and without
# infall and cooling the disk holds no cold gas, so neither star formation nor its feedback
# happens. SupernovaOutflowRate accumulates the reheated mass undivided; only
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

# The gas supply's keys, at R1's values
SUPPLY_PARAMS = {
    "GlobalBaryonFraction": 0.17,
    "AGNrecipe": 2,
    "RadioModeEfficiency": 0.08,
    "BulgeSizeOn": 0,
}

# ============================================================================
# SOFTWARE QUALITY TESTS
# ============================================================================


def test_module_loads():
    """Test that sage26_calculate_supernova_feedback module loads and initializes"""
    print(f"\n{BLUE}TEST: Module loads and initializes{NC}")

    # Need star formation first to provide NewStellarMass
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_load",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("sage26_calculate_star_formation", "process_by_galaxy"),
                ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
                ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 3.0,
            "FeedbackEjectionEfficiency": 0.3,
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module loads and initializes successfully{NC}")


def test_output_properties_exist():
    """Test that feedback properties appear in output"""
    print(f"\n{BLUE}TEST: Feedback properties exist in output{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_properties",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("sage26_calculate_star_formation", "process_by_galaxy"),
                ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
                ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 3.0,
            "FeedbackEjectionEfficiency": 0.3,
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Check that feedback field exists (SupernovaOutflowRate is the output property)
    assert (
        "SupernovaOutflowRate" in halos.dtype.names
    ), "Output should have SupernovaOutflowRate field"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Feedback properties exist in output{NC}")


def test_parameters_configurable():
    """Test that module parameters can be configured via YAML"""
    print(f"\n{BLUE}TEST: Module parameters are configurable{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_params",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("sage26_calculate_star_formation", "process_by_galaxy"),
                ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
                ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 5.0,  # Custom value
            "FeedbackEjectionEfficiency": 0.8,  # Custom value
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute with custom parameters\nSTDERR: {stderr}"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module parameters are configurable{NC}")


def test_memory_safety():
    """Test that module doesn't leak memory"""
    print(f"\n{BLUE}TEST: No memory leaks{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_memory",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("sage26_calculate_star_formation", "process_by_galaxy"),
                ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
                ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 3.0,
            "FeedbackEjectionEfficiency": 0.3,
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    assert check_no_memory_leaks(stdout, stderr), "Should not have memory leaks"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ No memory leaks detected{NC}")


def test_execution_completes():
    """Test that full pipeline execution completes successfully"""
    print(f"\n{BLUE}TEST: Full pipeline execution completes{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_complete",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("sage26_calculate_star_formation", "process_by_galaxy"),
                ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
                ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 3.0,
            "FeedbackEjectionEfficiency": 0.3,
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Pipeline should complete successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"
    assert output_file.stat().st_size > 0, "Output file should have content"

    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have output halos"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Full pipeline execution completes{NC}")


def test_pipeline_with_star_formation():
    """Test that module integrates correctly with star formation module"""
    print(f"\n{BLUE}TEST: Integration with star formation module{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_pipeline",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("sage26_calculate_star_formation", "process_by_galaxy"),
                ("sage26_calculate_supernova_feedback", "process_by_galaxy"),
                ("sage26_apply_star_formation_supernova", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 3.0,
            "FeedbackEjectionEfficiency": 0.3,
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Pipeline should complete successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Both SF and feedback properties should exist
    assert "StarFormationRate" in halos.dtype.names, "Should have StarFormationRate from SF module"
    assert (
        "SupernovaOutflowRate" in halos.dtype.names
    ), "Should have SupernovaOutflowRate from feedback module"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Integration with star formation module works{NC}")


# ============================================================================
# PHYSICS VALIDATION TESTS
# ============================================================================


def test_feedback_values_nonnegative():
    """Test that feedback values are non-negative"""
    print(f"\n{BLUE}TEST: Feedback values are non-negative{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_nonnegative",
        phase_config=PHYSICS_PHASES,
        model_params={
            **SUPPLY_PARAMS,
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 3.0,
            "FeedbackEjectionEfficiency": 0.3,
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # SupernovaOutflowRate should be non-negative
    snfb = halos["SupernovaOutflowRate"]
    assert np.all(snfb >= 0.0), "SupernovaOutflowRate should be non-negative"

    assert np.sum(snfb > 0.0) > 0, "star formation should drive some feedback"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Feedback values are non-negative{NC}")


def test_feedback_proportional_to_sf():
    """Test that feedback is proportional to star formation"""
    print(f"\n{BLUE}TEST: Feedback proportional to star formation{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_proportional",
        phase_config=PHYSICS_PHASES,
        model_params={
            **SUPPLY_PARAMS,
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 3.0,
            "FeedbackEjectionEfficiency": 0.3,
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    sfr = halos["StarFormationRate"]
    snfb = halos["SupernovaOutflowRate"]

    # Galaxies with star formation should have feedback
    has_sf = sfr > 0.0
    assert np.any(has_sf), "some galaxies should form stars (the pipeline supplies cold gas)"
    # All star-forming galaxies should have feedback
    assert np.all(snfb[has_sf] > 0.0), "Galaxies with star formation should have supernova feedback"
    print(f"  Found {np.sum(has_sf)} star-forming galaxies with feedback")

    # Galaxies without star formation should have zero feedback
    no_sf = sfr == 0.0
    assert np.any(no_sf), "some galaxies should form no stars"
    assert np.all(
        snfb[no_sf] == 0.0
    ), "Galaxies without star formation should have zero supernova feedback"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Feedback is proportional to star formation{NC}")


def test_feedback_reasonable_magnitude():
    """Test that feedback values are physically reasonable

    With FIREmodeOn 0 the reheated mass is FeedbackReheatingEpsilon times the stars formed, both
    scaled together when they exceed the cold gas (sage26_calculate_supernova_feedback), and the
    apply step's clamp to the remaining cold gas never binds (stars + reheated <= ColdGas leaves
    at least the reheated mass after star formation). StarFormationRate is the stars formed over
    the row's own dT; sage26_finalize_cooling_heating divides SupernovaOutflowRate by the dT of
    the FoF's first galaxy, its central. So on every star-forming row

        SupernovaOutflowRate * dT(central) = epsilon * StarFormationRate * dT(row)

    (both fields share the writer conversion to Msun/yr), and the ratio of the two rates is
    epsilon on a central and epsilon * dT(row) / dT(central) on a satellite carried from an older
    snapshot. This replaces a bound on SupernovaOutflowRate / StarFormationRate taken without
    sage26_finalize_cooling_heating, where SupernovaOutflowRate still held a mass, so the ratio
    was not dimensionless.
    """
    print(f"\n{BLUE}TEST: Feedback has reasonable magnitude{NC}")

    # Use high efficiency to ensure we get feedback
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_magnitude",
        phase_config=PHYSICS_PHASES,
        model_params={
            **SUPPLY_PARAMS,
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 3.0,  # epsilon = 3
            "FeedbackEjectionEfficiency": 0.3,
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    sfr = halos["StarFormationRate"]
    snfb = halos["SupernovaOutflowRate"]

    # For star-forming galaxies, SupernovaOutflowRate * dT(central) = epsilon * SFR * dT(row)
    epsilon = 3.0
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
    outflow = snfb[has_sf].astype(np.float64) * central_dt[has_sf]
    expected = epsilon * sfr[has_sf].astype(np.float64) * dt[has_sf]
    # 1e-5 relative: both are float stores
    off = np.abs(outflow - expected) > 1e-5 * expected
    assert not np.any(off), (
        f"{int(np.sum(off))} star-forming rows break SupernovaOutflowRate * dT(central) = "
        f"epsilon * StarFormationRate * dT (largest relative error "
        f"{np.max(np.abs(outflow / expected - 1.0)):.3e})"
    )
    feedback_ratio = snfb[has_sf] / sfr[has_sf]
    print(f"  Feedback ratio range: {np.min(feedback_ratio):.4f} - {np.max(feedback_ratio):.4f}")

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Feedback has reasonable magnitude{NC}")


def test_edge_case_zero_efficiency():
    """Test edge case: zero feedback efficiency"""
    print(f"\n{BLUE}TEST: Edge case - zero feedback efficiency{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_zero_efficiency",
        phase_config=PHYSICS_PHASES,
        model_params={
            **SUPPLY_PARAMS,
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 0.0,  # Zero reheating
            "FeedbackEjectionEfficiency": 0.0,  # Zero ejection
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should handle zero efficiency\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    assert np.any(halos["StarFormationRate"] > 0.0), "some galaxies should form stars"

    # With FIREmodeOn 0 the reheated mass is FeedbackReheatingEpsilon times the stars formed, so
    # at epsilon 0 it is exactly zero and the outflow rate it feeds is exactly zero
    snfb = halos["SupernovaOutflowRate"]
    assert np.all(
        snfb == 0.0
    ), f"zero efficiency must give zero outflow, got max={np.max(snfb):.3e}"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Edge case - zero feedback efficiency handled correctly{NC}")


def test_edge_case_very_high_efficiency():
    """Test edge case: very high feedback efficiency"""
    print(f"\n{BLUE}TEST: Edge case - very high feedback efficiency{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="snfb_high_efficiency",
        phase_config=PHYSICS_PHASES,
        model_params={
            **SUPPLY_PARAMS,
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 50.0,  # Very high reheating
            "FeedbackEjectionEfficiency": 10.0,  # Very high ejection
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should handle high efficiency\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Should still produce valid output
    assert np.any(halos["StarFormationRate"] > 0.0), "some galaxies should form stars"
    snfb = halos["SupernovaOutflowRate"]
    assert np.all(snfb >= 0.0), "Feedback should be non-negative even with high efficiency"
    assert np.all(np.isfinite(snfb)), "Feedback should be finite (no NaN/Inf)"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Edge case - very high feedback efficiency handled correctly{NC}")


def test_parameter_variation_effect():
    """Test that changing parameters produces expected changes in output"""
    print(f"\n{BLUE}TEST: Parameter variation effect{NC}")

    # Run 1: Low efficiency
    param_file_low, output_dir_low, temp_dir_low = create_test_param_file(
        output_name="snfb_low_eff",
        phase_config=PHYSICS_PHASES,
        model_params={
            **SUPPLY_PARAMS,
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 1.0,  # Low
            "FeedbackEjectionEfficiency": 0.1,
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file_low)
    assert returncode == 0, f"Low efficiency run should succeed\nSTDERR: {stderr}"

    output_file_low = output_dir_low / "model_z0.000_0"
    halos_low, _ = load_binary_halos(output_file_low)
    snfb_low = halos_low["SupernovaOutflowRate"]

    # Run 2: High efficiency
    param_file_high, output_dir_high, temp_dir_high = create_test_param_file(
        output_name="snfb_high_eff",
        phase_config=PHYSICS_PHASES,
        model_params={
            **SUPPLY_PARAMS,
            # Required by the SF module (and SFprescription by the apply step), at R1's values
            "SFprescription": 0,
            "CGMrecipeOn": 0,  # required, R3's value
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 5.0,  # High
            "FeedbackEjectionEfficiency": 0.5,
            "FIREmodeOn": 0,  # required by the SN module, R2's fixed-epsilon feedback
            "RecycleFraction": 0.43,
            "Yield": 0.025,
            "FracZleaveDisk": 0.0,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file_high)
    assert returncode == 0, f"High efficiency run should succeed\nSTDERR: {stderr}"

    output_file_high = output_dir_high / "model_z0.000_0"
    halos_high, _ = load_binary_halos(output_file_high)
    snfb_high = halos_high["SupernovaOutflowRate"]

    # Higher efficiency should produce higher feedback (on average)
    # Compare mean feedback for star-forming galaxies
    sfr_low = halos_low["StarFormationRate"]
    sfr_high = halos_high["StarFormationRate"]

    has_sf_low = sfr_low > 0.0
    has_sf_high = sfr_high > 0.0

    assert np.any(has_sf_low) and np.any(has_sf_high), "both runs should form stars"
    mean_snfb_low = np.mean(snfb_low[has_sf_low])
    mean_snfb_high = np.mean(snfb_high[has_sf_high])

    # High efficiency should produce more feedback
    assert mean_snfb_low > 0 and mean_snfb_high > 0, "both runs should drive feedback"
    assert mean_snfb_high > mean_snfb_low, (
        f"Higher efficiency should produce more feedback: "
        f"low={mean_snfb_low:.3e}, high={mean_snfb_high:.3e}"
    )
    print(f"  Low efficiency mean: {mean_snfb_low:.3e}, High efficiency mean: {mean_snfb_high:.3e}")

    shutil.rmtree(temp_dir_low)
    shutil.rmtree(temp_dir_high)
    print(f"{GREEN}✓ Parameter variation produces expected changes{NC}")


# ============================================================================
# MAIN TEST RUNNER
# ============================================================================


def main():
    """Main test runner"""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: {Path(__file__).name}{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        test_module_loads,
        test_output_properties_exist,
        test_parameters_configurable,
        test_memory_safety,
        test_execution_completes,
        test_pipeline_with_star_formation,
        test_feedback_values_nonnegative,
        test_feedback_proportional_to_sf,
        test_feedback_reasonable_magnitude,
        test_edge_case_zero_efficiency,
        test_edge_case_very_high_efficiency,
        test_parameter_variation_effect,
    ]

    if not MIMIC_EXE.exists():
        for test in tests:
            result_skip(test.__name__, "Mimic not built")
        return 0

    passed = 0
    failed = 0
    skipped = 0

    for test in tests:
        print()
        try:
            test()
            result_pass(test.__name__)
            passed += 1
        except TestSkipped as e:
            result_skip(test.__name__, str(e))
            skipped += 1
        except AssertionError as e:
            result_fail(test.__name__, str(e).splitlines()[0])
            failed += 1
        except Exception as e:
            result_error(test.__name__, str(e).splitlines()[0])
            failed += 1

    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed:  {passed}")
    if skipped:
        print(f"Skipped: {skipped}")
    print(f"Failed:  {failed}")
    print(f"Total:   {passed + failed + skipped}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
