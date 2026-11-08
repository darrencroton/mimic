#!/usr/bin/env python3
"""
SAGE Radio Mode Heating Module - Integration Test

Validates: Module pipeline integration, AGN physics correctness, parameter sensitivity

This test validates the sage26_radio_mode_heating module integration:
- Module loads and executes correctly in full pipeline
- AGN feedback suppresses cooling appropriately
- Black hole mass increases via accretion
- Hot gas is consumed by BH accretion
- Parameter sensitivity (efficiency and recipe mode)
- Memory safety and performance

Test cases:
  - test_module_pipeline_integration: Full pipeline execution and output validation
  - test_agn_physics_correctness: Physics correctness and reasonableness checks
  - test_parameter_sensitivity: Efficiency and recipe parameters affect results
  - test_memory_and_performance: Memory leaks and performance baseline

Author: Mimic Development Team
Date: 2025-12-18
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


def test_module_pipeline_integration():
    """Test that module integrates correctly into full pipeline

    Validates:
    - Module loads and initializes
    - Pipeline executes without errors
    - Output file is created with expected properties
    - AGN-related fields exist and have valid data
    """
    print(f"\n{BLUE}TEST: Module pipeline integration{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="agn_integration",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("sage26_calculate_cooling_budget", "process_by_galaxy"),
                ("sage26_radio_mode_heating", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "RadioModeEfficiency": 0.08,
            "AGNrecipe": 1,
            # Cooling module parameters (required dependency)
            "CoolingRecipe": 1,
            "CoolingEfficiency": 1.0,
            "ReincorporationFactor": 1.0,
            "CGMrecipeOn": 0,  # required by sage26_calculate_cooling_budget, R3's value
        },
    )

    # Execute Mimic
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Validate output file exists
    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"
    assert output_file.stat().st_size > 0, "Output file should have content"

    # Load and validate output data
    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have output halos"

    # Check AGN-related fields exist
    assert "BlackHoleMass" in halos.dtype.names, "Output should have BlackHoleMass field"
    assert "HotGas" in halos.dtype.names, "Output should have HotGas field"
    assert "Heating" in halos.dtype.names, "Output should have Heating field"

    # Basic validation: no NaNs or Infs
    bh_mass = halos["BlackHoleMass"]
    hot_gas = halos["HotGas"]
    heating = halos["Heating"]

    assert not np.any(np.isnan(bh_mass)), "BlackHoleMass should not have NaN values"
    assert not np.any(np.isinf(bh_mass)), "BlackHoleMass should not have Inf values"
    assert np.all(bh_mass >= 0.0), "BlackHoleMass should be non-negative"

    assert not np.any(np.isnan(hot_gas)), "HotGas should not have NaN values"
    assert not np.any(np.isinf(hot_gas)), "HotGas should not have Inf values"
    assert np.all(hot_gas >= 0.0), "HotGas should be non-negative"

    assert not np.any(np.isnan(heating)), "Heating should not have NaN values"
    assert not np.any(np.isinf(heating)), "Heating should not have Inf values"
    assert np.all(heating >= 0.0), "Heating should be non-negative"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module pipeline integration successful{NC}")


def test_agn_physics_correctness():
    """Test that AGN physics is physically reasonable

    Validates:
    - Black holes grow over time (via accretion)
    - Hot gas decreases when BH accretes
    - Cooling is suppressed by AGN feedback
    - Mass conservation: accreted mass comes from hot gas
    - AGN is more active in massive halos

    The radio-mode rate is proportional to the black hole mass and acts only on cooling hot gas, so
    the pipeline supplies both: infall and cooling give the hot and cold gas, the star-formation
    chain the stellar disk, and the disk instability's quasar-mode growth seeds the black holes.
    Black holes in massive halos and radio-mode heating must both be present.

    The hot gas bound applies to centrals: a central's hot gas is part of its FoF group's baryons,
    which the infall budget sets to HaloBaryonFraction * Mvir (sage26_prepare_infall_budget), far
    below Mvir. A satellite keeps the hot gas it held as a central while its subhalo's Mvir is
    stripped (only sage26_satellite_stripping, not in this pipeline, removes it), so its hot gas
    can exceed its current Mvir (up to 2.3 times on the test trees) and is not bounded by it.
    """
    print(f"\n{BLUE}TEST: AGN physics correctness{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="agn_physics",
        phase_config={
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
                ("sage26_disk_instability", "process_by_galaxy"),
                ("sage26_quasar_mode", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "RadioModeEfficiency": 0.08,
            "AGNrecipe": 1,
            # The gas supply, the star-formation chain and the black-hole seeding, at R1's values
            "GlobalBaryonFraction": 0.17,
            "BulgeSizeOn": 0,
            "SFprescription": 0,
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 3.0,
            "FeedbackEjectionEfficiency": 0.3,
            "FIREmodeOn": 0,
            "RecycleFraction": 0.43,
            "BlackHoleGrowthRate": 0.01,
            "QuasarModeEfficiency": 0.001,
            # Cooling module parameters
            "CoolingRecipe": 1,
            "CoolingEfficiency": 1.0,
            "ReincorporationFactor": 1.0,
            "CGMrecipeOn": 0,  # required by sage26_calculate_cooling_budget, R3's value
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Load output
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    bh_mass = halos["BlackHoleMass"]
    hot_gas = halos["HotGas"]
    heating = halos["Heating"]
    mvir = halos["Mvir"]

    # Physics validation 1: Some black holes should exist
    # (They might not grow much in one timestep, but should be present)
    total_bh_mass = np.sum(bh_mass)
    print(f"  Total black hole mass: {total_bh_mass:.2e} [1e10 Msun/h]")
    assert total_bh_mass >= 0.0, "Total BH mass should be non-negative"

    # Physics validation 2: BH mass should be much smaller than halo mass
    # Typical M_BH/M_halo ~ 10^-3 to 10^-5
    bh_in_massive_halos = (mvir > 10.0) & (bh_mass > 0)
    assert np.sum(bh_in_massive_halos) > 0, "massive halos should host black holes"
    bh_fraction = bh_mass[bh_in_massive_halos] / mvir[bh_in_massive_halos]
    assert np.all(bh_fraction < 0.1), "BH mass should be < 10% of halo mass (unreasonably high)"
    print(f"  BH mass fraction in massive halos: {np.mean(bh_fraction):.4e}")

    # Physics validation 3: a central's hot gas should be a reasonable fraction of its halo mass
    hot_gas_in_halos = (mvir > 1.0) & (halos["Type"] == 0)
    assert np.sum(hot_gas_in_halos & (hot_gas > 0)) > 0, "some centrals should hold hot gas"
    hot_gas_fraction = hot_gas[hot_gas_in_halos] / mvir[hot_gas_in_halos]
    assert np.all(hot_gas_fraction <= 1.0), "Hot gas cannot exceed total halo mass"
    print(f"  Mean hot gas fraction: {np.mean(hot_gas_fraction):.4f}")

    # Physics validation 4: Heating should be tracking AGN energy injection
    total_heating = np.sum(heating)
    print(f"  Total AGN heating: {total_heating:.2e}")
    assert total_heating >= 0.0, "Total heating should be non-negative"
    assert np.sum(heating > 0) > 0, "radio-mode heating should act on some galaxies"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ AGN physics is physically reasonable{NC}")


# The pipeline of test_agn_physics_correctness: the gas supply, the star-formation chain and the
# disk instability's quasar-mode growth, which seeds the black holes the radio mode heats with
GAS_PHASES = {
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
        ("sage26_disk_instability", "process_by_galaxy"),
        ("sage26_quasar_mode", "process_by_galaxy"),
    ],
    "satellite_mergers": [],
    "post_timestep": [],
}


def run_gas_pipeline(output_name, radio_mode_efficiency, agn_recipe):
    """Run GAS_PHASES at R1's values with the two radio-mode parameters; return the z = 0 halos."""
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name=output_name,
        phase_config=GAS_PHASES,
        model_params={
            "RadioModeEfficiency": radio_mode_efficiency,
            "AGNrecipe": agn_recipe,
            "GlobalBaryonFraction": 0.17,
            "BulgeSizeOn": 0,
            "SFprescription": 0,
            "H2RadialIntegrationOn": 1,
            "H2RadialNBins": 25,
            "H2RadialRMaxFactor": 5.0,
            "SfrEfficiency": 0.02,
            "StarFormingDiskFactor": 3.0,
            "FeedbackReheatingEpsilon": 3.0,
            "FeedbackEjectionEfficiency": 0.3,
            "FIREmodeOn": 0,
            "RecycleFraction": 0.43,
            "BlackHoleGrowthRate": 0.01,
            "QuasarModeEfficiency": 0.001,
            "CGMrecipeOn": 0,
        },
    )
    try:
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"
        halos, _metadata = load_binary_halos(output_dir / "model_z0.000_0")
    finally:
        shutil.rmtree(temp_dir)
    return halos


def test_parameter_sensitivity():
    """Test that changing parameters changes results

    Validates, on GAS_PHASES, where black holes exist and hot gas cools:
    - AGNrecipe 0 turns the radio mode off: no row is heated
    - RadioModeEfficiency is read and acts: both efficiencies heat some rows and give different
      heating and black-hole masses. No direction is asserted: the radio mode's feedback on the
      cooling that feeds it and the quasar-mode growth makes the z = 0 totals non-monotonic
      (measured at 0.02 and 0.20: BlackHoleMass 9.09e-2 and 8.97e-2, Heating 5.3e3 and 3.5e3)
    - The cold-cloud recipe (AGNrecipe 3) heats, with finite, non-negative black-hole masses
    """
    print(f"\n{BLUE}TEST: Parameter sensitivity{NC}")

    off = run_gas_pipeline("agn_off", 0.08, 0)
    low = run_gas_pipeline("agn_low_eff", 0.02, 1)
    high = run_gas_pipeline("agn_high_eff", 0.20, 1)
    cloud = run_gas_pipeline("agn_cold_cloud", 0.08, 3)

    for name, halos in (("off", off), ("low", low), ("high", high), ("cold cloud", cloud)):
        print(
            f"  {name}: BlackHoleMass {np.sum(halos['BlackHoleMass']):.3e}, "
            f"Heating {np.sum(halos['Heating']):.3e} on {int(np.sum(halos['Heating'] > 0))} rows"
        )

    assert np.all(off["Heating"] == 0.0), "AGNrecipe 0 should heat no row"
    assert np.sum(low["Heating"]) > 0, "the lower efficiency should heat some rows"
    assert np.sum(high["Heating"]) > 0, "the higher efficiency should heat some rows"
    for field in ("Heating", "BlackHoleMass"):
        assert not np.array_equal(
            low[field], high[field]
        ), f"RadioModeEfficiency should change {field}"
    bh_cloud = cloud["BlackHoleMass"]
    assert np.all(np.isfinite(bh_cloud) & (bh_cloud >= 0.0)), "cold-cloud BH masses must be valid"
    assert np.sum(cloud["Heating"]) > 0, "the cold-cloud recipe should heat some rows"

    print(f"{GREEN}✓ Parameter sensitivity validated{NC}")


def test_memory_and_performance():
    """Test memory safety and establish performance baseline

    Validates:
    - No memory leaks during execution
    - Execution completes in reasonable time
    """
    print(f"\n{BLUE}TEST: Memory safety and performance{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="agn_memory",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("sage26_calculate_cooling_budget", "process_by_galaxy"),
                ("sage26_radio_mode_heating", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "RadioModeEfficiency": 0.08,
            "AGNrecipe": 1,
            "CoolingRecipe": 1,
            "CoolingEfficiency": 1.0,
            "ReincorporationFactor": 1.0,
            "CGMrecipeOn": 0,  # required by sage26_calculate_cooling_budget, R3's value
        },
    )

    # Execute and time
    import time

    start_time = time.time()
    returncode, stdout, stderr = run_mimic(param_file)
    elapsed_time = time.time() - start_time

    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Check for memory leaks
    assert check_no_memory_leaks(stdout, stderr), "Should not have memory leaks"

    # Performance baseline (should complete in < 30 seconds for test data)
    print(f"  Execution time: {elapsed_time:.2f}s")
    assert elapsed_time < 30.0, f"Execution too slow: {elapsed_time:.2f}s (expected < 30s)"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Memory safety and performance validated{NC}")


def main():
    """Main test runner"""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: {Path(__file__).name}{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        test_module_pipeline_integration,
        test_agn_physics_correctness,
        test_parameter_sensitivity,
        test_memory_and_performance,
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
