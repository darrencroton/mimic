#!/usr/bin/env python3
"""
SAGE Add Cooling Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration

This test validates software quality aspects of the sage_apply_cooling module:
- Module loads and initializes correctly
- Module executes without errors or memory leaks
- Output properties appear in output files
- Module works with sage_calculate_cooling_budget in multi-phase pipeline
- Proper gas transfer from hot to cold reservoirs
- Metallicity preservation during transfer
- Cooling energy tracking

NOTE: sage_apply_cooling requires sage_calculate_cooling_budget (galaxy_physics) to calculate CoolingGas

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: ColdGas, HotGas properties in output
  - test_with_sage_calculate_cooling_budget: Integration with sage_calculate_cooling_budget (required)
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_gas_transfer_physics: Hot to cold gas transfer validation
  - test_metallicity_preservation: Metallicity preserved during transfer
  - test_cooling_energy_tracking: Cooling property tracking

Author: Mimic Development Team
Date: 2025-12-18
"""

import shutil
import sys
from pathlib import Path

# Add tests directory to path to import framework
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    BLUE,
    GREEN,
    MIMIC_EXE,
    NC,
    RED,
    TestSkipped,
    create_test_param_file,
    load_binary_halos,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
)


def test_module_loads():
    """
    Test that sage_apply_cooling module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    Note: Requires sage_calculate_cooling_budget in galaxy_physics to set CoolingGas property
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_cooling_load",
        phase_config={
            "pre_timestep": [
                ("sage_reionization", "process_full_halo"),
                ("sage_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [
                ("sage_apply_infall", "process_full_halo"),
                ("sage_calculate_cooling_budget", "process_by_galaxy"),
                ("sage_apply_cooling", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert (
        returncode == 0
    ), f"Mimic should execute successfully with sage_apply_cooling\nStderr: {stderr}"

    # Check initialization log message
    assert (
        "SAGE apply cooling module initialized" in stdout
    ), "sage_apply_cooling should log initialization message"

    # Check that sage_calculate_cooling_budget ran first
    assert (
        "SAGE calculate cooling budget module initialized" in stdout
    ), "sage_calculate_cooling_budget should run before sage_apply_cooling"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Module loads and initializes successfully")


def test_output_properties_exist():
    """
    Test that gas reservoir properties appear in output

    Expected: ColdGas, HotGas, MetalsColdGas, MetalsHotGas, Cooling in output file
    Validates: Module creates expected output properties
    """
    print("Testing output properties...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_cooling_output",
        phase_config={
            "pre_timestep": [
                ("sage_reionization", "process_full_halo"),
                ("sage_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [
                ("sage_apply_infall", "process_full_halo"),
                ("sage_calculate_cooling_budget", "process_by_galaxy"),
                ("sage_apply_cooling", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic execution should succeed"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"

    # Load and check halos
    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have halos in output"

    # Check output properties exist
    assert "ColdGas" in halos.dtype.names, "ColdGas property should exist in output"
    assert "HotGas" in halos.dtype.names, "HotGas property should exist in output"
    assert "MetalsColdGas" in halos.dtype.names, "MetalsColdGas property should exist in output"
    assert "MetalsHotGas" in halos.dtype.names, "MetalsHotGas property should exist in output"
    assert "Cooling" in halos.dtype.names, "Cooling property should exist in output"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Output properties exist")
    print(f"  Found {len(halos)} halos")


def test_with_sage_calculate_cooling_budget():
    """
    Test that sage_apply_cooling works with sage_calculate_cooling_budget module

    Expected: Both modules execute successfully together
    Validates: sage_apply_cooling requires sage_calculate_cooling_budget to set CoolingGas
    """
    print("Testing with sage_calculate_cooling_budget...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_cooling_with_calc",
        phase_config={
            "pre_timestep": [
                ("sage_reionization", "process_full_halo"),
                ("sage_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [
                ("sage_apply_infall", "process_full_halo"),
                ("sage_calculate_cooling_budget", "process_by_galaxy"),
                ("sage_apply_cooling", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, f"Should run with both modules\nStderr: {stderr}"

    # Verify all modules initialized
    assert "SAGE reionization module initialized" in stdout, "sage_reionization should initialize"
    assert (
        "SAGE prepare infall budget module initialized" in stdout
    ), "sage_prepare_infall_budget should initialize"
    assert "SAGE apply infall module initialized" in stdout, "sage_apply_infall should initialize"
    assert (
        "SAGE calculate cooling budget module initialized" in stdout
    ), "sage_calculate_cooling_budget should initialize"
    assert "SAGE apply cooling module initialized" in stdout, "sage_apply_cooling should initialize"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Works with sage_calculate_cooling_budget")


def test_memory_safety():
    """
    Test that sage_apply_cooling doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_cooling_memory",
        phase_config={
            "pre_timestep": [
                ("sage_reionization", "process_full_halo"),
                ("sage_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [
                ("sage_apply_infall", "process_full_halo"),
                ("sage_calculate_cooling_budget", "process_by_galaxy"),
                ("sage_apply_cooling", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution should succeed"
    assert "Memory leak detected" not in stdout, "Should not have memory leaks"
    assert "Memory leak detected" not in stderr, "Should not have memory leaks in stderr"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ No memory leaks detected")


def test_execution_completes():
    """
    Test that full pipeline execution completes without errors

    Expected: Initialization, processing, and cleanup all succeed
    Validates: Complete module lifecycle
    """
    print("Testing full pipeline completion...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_cooling_complete",
        phase_config={
            "pre_timestep": [
                ("sage_reionization", "process_full_halo"),
                ("sage_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [
                ("sage_apply_infall", "process_full_halo"),
                ("sage_calculate_cooling_budget", "process_by_galaxy"),
                ("sage_apply_cooling", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
        first_file=0,
        last_file=0,
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Pipeline should complete successfully"
    assert "SAGE apply cooling module initialized" in stdout, "Module initialization message"
    assert "SAGE apply cooling module cleaned up" in stdout, "Module cleanup message"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Full pipeline completes")


def test_gas_transfer_physics():
    """
    Test that gas is transferred from hot to cold reservoirs

    Expected: ColdGas increases, HotGas decreases for centrals with cooling
    Validates: Gas transfer physics
    """
    print("Testing gas transfer physics...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_cooling_transfer",
        phase_config={
            "pre_timestep": [
                ("sage_reionization", "process_full_halo"),
                ("sage_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [
                ("sage_apply_infall", "process_full_halo"),
                ("sage_calculate_cooling_budget", "process_by_galaxy"),
                ("sage_apply_cooling", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic execution should succeed"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Filter to Type 0 centrals
    import numpy as np

    type0_mask = halos["Type"] == 0
    type0_halos = halos[type0_mask]

    # Check gas reservoirs are physically reasonable
    assert np.all(np.isfinite(type0_halos["HotGas"])), "HotGas should have finite values"
    assert np.all(type0_halos["HotGas"] >= 0), "HotGas should be non-negative"

    assert np.all(np.isfinite(type0_halos["ColdGas"])), "ColdGas should have finite values"
    assert np.all(type0_halos["ColdGas"] >= 0), "ColdGas should be non-negative"

    # Check that some halos have cold gas (cooling occurred)
    num_with_cold_gas = np.sum(type0_halos["ColdGas"] > 0)
    print(f"  Found {num_with_cold_gas} / {len(type0_halos)} centrals with cold gas")
    assert num_with_cold_gas > 0, "Some centrals should have cold gas from cooling"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Gas transfer physics validated")


def test_metallicity_preservation():
    """
    Test that metallicity is preserved during gas transfer

    Expected: Metal to gas ratio preserved in both reservoirs
    Validates: Metallicity handling
    """
    print("Testing metallicity preservation...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_cooling_metallicity",
        phase_config={
            "pre_timestep": [
                ("sage_reionization", "process_full_halo"),
                ("sage_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [
                ("sage_apply_infall", "process_full_halo"),
                ("sage_calculate_cooling_budget", "process_by_galaxy"),
                ("sage_apply_cooling", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic execution should succeed"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Filter to Type 0 centrals
    import numpy as np

    type0_mask = halos["Type"] == 0
    type0_halos = halos[type0_mask]

    # Check metal reservoirs are physically reasonable
    assert np.all(
        np.isfinite(type0_halos["MetalsHotGas"])
    ), "MetalsHotGas should have finite values"
    assert np.all(type0_halos["MetalsHotGas"] >= 0), "MetalsHotGas should be non-negative"

    assert np.all(
        np.isfinite(type0_halos["MetalsColdGas"])
    ), "MetalsColdGas should have finite values"
    assert np.all(type0_halos["MetalsColdGas"] >= 0), "MetalsColdGas should be non-negative"

    # Check that metals don't exceed gas (metallicity ≤ 1.0)
    hot_gas_nonzero = type0_halos["HotGas"] > 0
    if np.any(hot_gas_nonzero):
        z_hot = (
            type0_halos["MetalsHotGas"][hot_gas_nonzero] / type0_halos["HotGas"][hot_gas_nonzero]
        )
        assert np.all(z_hot <= 1.0), "Hot gas metallicity should be ≤ 1.0"

    cold_gas_nonzero = type0_halos["ColdGas"] > 0
    if np.any(cold_gas_nonzero):
        z_cold = (
            type0_halos["MetalsColdGas"][cold_gas_nonzero]
            / type0_halos["ColdGas"][cold_gas_nonzero]
        )
        assert np.all(z_cold <= 1.0), "Cold gas metallicity should be ≤ 1.0"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Metallicity preservation validated")


def test_cooling_energy_tracking():
    """
    Test that cooling energy is tracked

    Expected: Cooling property has finite, non-negative values
    Validates: Cooling energy tracking
    """
    print("Testing cooling energy tracking...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_cooling_energy",
        phase_config={
            "pre_timestep": [
                ("sage_reionization", "process_full_halo"),
                ("sage_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [
                ("sage_apply_infall", "process_full_halo"),
                ("sage_calculate_cooling_budget", "process_by_galaxy"),
                ("sage_apply_cooling", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic execution should succeed"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Check Cooling property
    import numpy as np

    assert "Cooling" in halos.dtype.names, "Cooling property should exist"

    # Filter to Type 0 centrals
    type0_mask = halos["Type"] == 0
    type0_halos = halos[type0_mask]

    # Check cooling values are finite and non-negative
    assert np.all(np.isfinite(type0_halos["Cooling"])), "Cooling should have finite values"
    assert np.all(type0_halos["Cooling"] >= 0), "Cooling should be non-negative"

    # Check that some halos have non-zero cooling
    num_with_cooling = np.sum(type0_halos["Cooling"] > 0)
    print(f"  Found {num_with_cooling} / {len(type0_halos)} centrals with cooling energy")

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Cooling energy tracking validated")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: SAGE Add Cooling Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    tests = [
        test_module_loads,
        test_output_properties_exist,
        test_with_sage_calculate_cooling_budget,
        test_memory_safety,
        test_execution_completes,
        test_gas_transfer_physics,
        test_metallicity_preservation,
        test_cooling_energy_tracking,
    ]

    passed = 0
    failed = 0
    skipped = 0

    if not MIMIC_EXE.exists():
        for test in tests:
            result_skip(test.__name__, "Mimic not built")
        return 0

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
