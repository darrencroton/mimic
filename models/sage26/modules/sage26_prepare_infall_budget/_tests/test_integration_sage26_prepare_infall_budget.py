#!/usr/bin/env python3
"""
SAGE Infall Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration

This test validates software quality aspects of the sage26_prepare_infall_budget module:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- Output properties appear in output files
- Module works in multi-module pipelines
- Integration with sage26_satellite_stripping (shared reionization utility)

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: HotGas properties in output
  - test_parameters_configurable: GlobalBaryonFraction parameter configuration
  - test_with_satellite_stripping: Integration with sage26_satellite_stripping
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_infalling_gas_calculation: Physics validation of InfallingGas calculation
  - test_multiple_module_pipeline: Multi-module integration

Author: Mimic Development Team
Date: 2025-12-11
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
    Test that sage26_prepare_infall_budget module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    Note: Requires sage26_reionization module to set HaloBaryonFraction property
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_prepare_infall_budget_load",
        phase_config={
            "pre_timestep": [
                ("sage26_reionization", "process_full_halo"),
                ("sage26_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17, "CGMrecipeOn": 0},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert (
        returncode == 0
    ), f"Mimic should execute successfully with sage26_prepare_infall_budget\nStderr: {stderr}"

    # Check initialization log message
    assert (
        "SAGE prepare infall budget module initialized" in stdout
    ), "sage26_prepare_infall_budget should log initialization message"

    # Check that reionization module ran first
    assert (
        "SAGE reionization module initialized" in stdout
    ), "sage26_reionization should run before sage26_prepare_infall_budget"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Module loads and initializes successfully")


def test_output_properties_exist():
    """
    Test that HotGas and related properties appear in output

    Expected: HotGas, MetalsHotGas, EjectedGas, and ICS in output file
    Validates: Module creates expected output properties
    """
    print("Testing output properties...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_prepare_infall_budget_output",
        phase_config={
            "pre_timestep": [
                ("sage26_reionization", "process_full_halo"),
                ("sage26_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17, "CGMrecipeOn": 0},
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
    assert "HotGas" in halos.dtype.names, "HotGas property should exist in output"
    assert "MetalsHotGas" in halos.dtype.names, "MetalsHotGas property should exist in output"
    assert "EjectedGas" in halos.dtype.names, "EjectedGas property should exist in output"
    assert "ICS" in halos.dtype.names, "ICS property should exist in output"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Output properties exist")
    print(f"  Found {len(halos)} halos")


def test_parameters_configurable():
    """
    Test that sage26_reionization uses GlobalBaryonFraction model parameter

    Expected: Custom GlobalBaryonFraction value is read from model_parameters and logged
    Validates: Model parameter reading and usage
    Note: GlobalBaryonFraction is used by sage26_reionization to set HaloBaryonFraction property
    """
    print("Testing parameter configuration...")

    # ===== SETUP =====

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_prepare_infall_budget_params",
        phase_config={
            "pre_timestep": [
                ("sage26_reionization", "process_full_halo"),
                ("sage26_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.20, "CGMrecipeOn": 0},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with custom parameters should succeed"

    # Verify parameter was read by sage26_reionization
    assert (
        "GlobalBaryonFraction = 0.2000" in stdout or "GlobalBaryonFraction: 0.2" in stdout
    ), f"Custom GlobalBaryonFraction should be logged\nStdout:\n{stdout}"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Parameters are configurable")


def test_with_satellite_stripping():
    """
    Test that sage26_prepare_infall_budget works with sage26_satellite_stripping module

    Expected: Both modules execute successfully together
    Validates: Modules work together using HaloBaryonFraction property from sage26_reionization
    Note: sage26_satellite_stripping is part of the sage26 package, so it is always registered;
    were it missing, the run would fail
    """
    print("Testing with sage26_satellite_stripping...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_prepare_infall_budget_stripping",
        phase_config={
            "pre_timestep": [
                ("sage26_reionization", "process_full_halo"),
                ("sage26_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [("sage26_satellite_stripping", "process_by_galaxy")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17, "CGMrecipeOn": 0},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, f"Should run with both modules\nStderr: {stderr}"

    # Verify all modules initialized
    assert (
        "SAGE reionization module initialized" in stdout
    ), "sage26_reionization should initialize first"
    assert (
        "SAGE prepare infall budget module initialized" in stdout
    ), "sage26_prepare_infall_budget should initialize"
    assert (
        "SAGE satellite stripping module initialized" in stdout
    ), "sage26_satellite_stripping should initialize"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Works with sage26_satellite_stripping")


def test_memory_safety():
    """
    Test that sage26_prepare_infall_budget doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_prepare_infall_budget_memory",
        phase_config={
            "pre_timestep": [
                ("sage26_reionization", "process_full_halo"),
                ("sage26_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17, "CGMrecipeOn": 0},
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
        output_name="sage26_prepare_infall_budget_complete",
        phase_config={
            "pre_timestep": [
                ("sage26_reionization", "process_full_halo"),
                ("sage26_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17, "CGMrecipeOn": 0},
        first_file=0,
        last_file=0,
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Pipeline should complete successfully"
    assert (
        "SAGE prepare infall budget module initialized" in stdout
    ), "Module initialization message"
    assert "SAGE prepare infall budget module cleaned up" in stdout, "Module cleanup message"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Full pipeline completes")


def test_infalling_gas_calculation():
    """
    Test that InfallingGas is calculated correctly based on HaloBaryonFraction

    Expected: InfallingGas values are physically reasonable
    Validates: Core physics calculation (InfallingGas = HaloBaryonFraction × Mvir - existing_baryons)

    InfallingGas is a transport field that is not written (model_properties.yaml, output: false),
    so the budget is observed through its consumer, sage26_apply_infall, which adds it to (or, when
    negative, removes it from the ejected and then the hot gas of) the FoF central. With no other
    module changing the baryons, every FoF group then holds exactly HaloBaryonFraction * Mvir (Mvir
    read as a float), the budget's target, and such groups must exist. The only exception is a
    negative budget larger than the central's ejected and hot gas, which sage26_apply_infall clears
    to zero: the satellites' hot gas, which the budget does not consolidate, then leaves the group
    above its target, never below.
    """
    print("Testing InfallingGas calculation physics...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_prepare_infall_budget_physics",
        phase_config={
            "pre_timestep": [
                ("sage26_reionization", "process_full_halo"),
                ("sage26_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [("sage26_apply_infall", "process_full_halo")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17, "CGMrecipeOn": 0},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic execution should succeed"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"

    # Load halos
    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have halos in output"

    # Filter to Type 0 centrals (where InfallingGas is calculated)
    import numpy as np

    type0_mask = halos["Type"] == 0
    type0_halos = halos[type0_mask]

    assert len(type0_halos) > 0, "Should have Type 0 centrals for testing"

    # Validate InfallingGas values are reasonable
    # InfallingGas = HaloBaryonFraction × Mvir - existing_baryons
    # Can be positive (net infall) or negative (mass loss)

    # Check that HaloBaryonFraction is used in calculation
    assert (
        "HaloBaryonFraction" in type0_halos.dtype.names
    ), "HaloBaryonFraction should be set by sage26_reionization"

    assert np.all(np.isfinite(type0_halos["HotGas"])), "HotGas should be finite"
    assert np.all(np.isfinite(type0_halos["EjectedGas"])), "EjectedGas should be finite"
    assert np.all(np.isfinite(type0_halos["ICS"])), "ICS should be finite"

    # Each FoF group's baryons (every row carries its FoF central's UniqueGalaxyID) against the
    # budget's target HaloBaryonFraction * Mvir
    reservoirs = [
        "StellarMass",
        "ColdGas",
        "HotGas",
        "EjectedGas",
        "ICS",
        "BlackHoleMass",
        "CGMgas",
    ]
    row_baryons = sum(halos[name].astype(np.float64) for name in reservoirs)
    group_ids, group_index = np.unique(halos["UniqueCentralGalaxyID"], return_inverse=True)
    group_sums = np.bincount(group_index, weights=row_baryons)
    position = np.searchsorted(group_ids, type0_halos["UniqueGalaxyID"])
    assert np.all(
        group_ids[position] == type0_halos["UniqueGalaxyID"]
    ), "every central should head its own FoF group"
    group_baryons = group_sums[position]
    target = type0_halos["HaloBaryonFraction"].astype(np.float64) * type0_halos["Mvir"].astype(
        np.float32
    ).astype(np.float64)

    # 1e-5 relative: float reservoirs
    at_target = np.abs(group_baryons - target) <= 1e-5 * target
    above = group_baryons > target * (1.0 + 1e-5)
    assert np.sum(at_target) > 0, "the budget should bring FoF groups to their target"
    assert np.all(at_target | above), (
        f"{int(np.sum(~(at_target | above)))} FoF groups end below their target "
        "HaloBaryonFraction * Mvir"
    )
    assert np.all(
        type0_halos["HotGas"][above] == 0.0
    ), "a group above its target should be one whose negative budget cleared the central's hot gas"
    print(
        f"  ✓ InfallingGas brought {int(np.sum(at_target))} of {len(type0_halos)} FoF groups to "
        f"HaloBaryonFraction × Mvir ({int(np.sum(above))} exhausted by a negative budget)"
    )

    # Cleanup
    shutil.rmtree(temp_dir)


def test_multiple_module_pipeline():
    """
    Test that sage26_prepare_infall_budget works with other modules in pipeline

    Expected: Multi-module pipeline executes successfully (if companion module available)
    Validates: Inter-module compatibility

    Note: Uses sage26_apply_infall, the budget's downstream consumer, as companion module. It is
          part of the sage26 package, so it is always registered; were it missing, the run would
          fail
    """
    print("Testing multi-module pipeline...")

    companion_module = "sage26_apply_infall"
    companion_init_msg = "SAGE apply infall module initialized"

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_prepare_infall_budget_multi",
        phase_config={
            "pre_timestep": [
                ("sage26_reionization", "process_full_halo"),
                ("sage26_prepare_infall_budget", "process_full_halo"),
            ],
            "galaxy_physics": [("sage26_apply_infall", "process_full_halo")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17, "CGMrecipeOn": 0},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, f"Multi-module pipeline should execute successfully\nStderr: {stderr}"

    # Verify all modules initialized
    assert (
        "SAGE prepare infall budget module initialized" in stdout
    ), "sage26_prepare_infall_budget should initialize"
    assert companion_init_msg in stdout, f"{companion_module} should initialize"

    # Cleanup
    shutil.rmtree(temp_dir)

    print(f"  ✓ Multi-module pipeline works (tested with {companion_module})")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: SAGE Infall Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    tests = [
        test_module_loads,
        test_output_properties_exist,
        test_parameters_configurable,
        test_with_satellite_stripping,
        test_memory_safety,
        test_execution_completes,
        test_infalling_gas_calculation,
        test_multiple_module_pipeline,
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
