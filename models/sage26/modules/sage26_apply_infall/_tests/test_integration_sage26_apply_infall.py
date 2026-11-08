#!/usr/bin/env python3
"""
SAGE Add Infall Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration

This test validates software quality aspects of the sage26_apply_infall module:
- Module loads and initializes correctly
- Module executes without errors or memory leaks
- Output properties appear in output files
- Module works with sage26_prepare_infall_budget in multi-phase pipeline
- Proper substep distribution

NOTE: sage26_apply_infall requires sage26_prepare_infall_budget (pre_timestep) to calculate InfallingGas

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: HotGas properties in output
  - test_with_sage26_prepare_infall_budget: Integration with sage26_prepare_infall_budget (required)
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_substep_distribution: Infall distributed over substeps
  - test_negative_infall_physics: Negative infall handling (HotGas/EjectedGas reduction)

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
    Test that sage26_apply_infall module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    Note: Requires sage26_prepare_infall_budget in pre_timestep to set InfallingGas property
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_apply_infall_load",
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
    assert (
        returncode == 0
    ), f"Mimic should execute successfully with sage26_apply_infall\nStderr: {stderr}"

    # Check initialization log message
    assert (
        "SAGE apply infall module initialized" in stdout
    ), "sage26_apply_infall should log initialization message"

    # Check that sage26_prepare_infall_budget ran first
    assert (
        "SAGE prepare infall budget module initialized" in stdout
    ), "sage26_prepare_infall_budget should run before sage26_apply_infall"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Module loads and initializes successfully")


def test_output_properties_exist():
    """
    Test that HotGas properties appear in output

    Expected: HotGas and MetalsHotGas in output file
    Validates: Module creates expected output properties
    """
    print("Testing output properties...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_apply_infall_output",
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

    # Load and check halos
    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have halos in output"

    # Check output properties exist
    assert "HotGas" in halos.dtype.names, "HotGas property should exist in output"
    assert "MetalsHotGas" in halos.dtype.names, "MetalsHotGas property should exist in output"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Output properties exist")
    print(f"  Found {len(halos)} halos")


def test_with_sage26_prepare_infall_budget():
    """
    Test that sage26_apply_infall works with sage26_prepare_infall_budget module

    Expected: Both modules execute successfully together
    Validates: sage26_apply_infall requires sage26_prepare_infall_budget to set InfallingGas
    """
    print("Testing with sage26_prepare_infall_budget...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_apply_infall_with_infall",
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
    assert returncode == 0, f"Should run with both modules\nStderr: {stderr}"

    # Verify all modules initialized
    assert "SAGE reionization module initialized" in stdout, "sage26_reionization should initialize"
    assert (
        "SAGE prepare infall budget module initialized" in stdout
    ), "sage26_prepare_infall_budget should initialize"
    assert "SAGE apply infall module initialized" in stdout, "sage26_apply_infall should initialize"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Works with sage26_prepare_infall_budget")


def test_memory_safety():
    """
    Test that sage26_apply_infall doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_apply_infall_memory",
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
        output_name="sage26_apply_infall_complete",
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
        first_file=0,
        last_file=0,
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Pipeline should complete successfully"
    assert "SAGE apply infall module initialized" in stdout, "Module initialization message"
    assert "SAGE apply infall module cleaned up" in stdout, "Module cleanup message"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Full pipeline completes")


def test_substep_distribution():
    """
    Test that infall is properly distributed over substeps

    Expected: Module executes with multiple substeps without errors
    Validates: Substep distribution logic
    """
    print("Testing substep distribution...")

    # ===== SETUP =====
    import yaml

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_apply_infall_substeps",
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

    # Update substeps in the parameter file (SubSteps is a top-level key)
    with open(param_file, "r") as f:
        config = yaml.safe_load(f)

    config["SubSteps"] = 4

    with open(param_file, "w") as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with substeps should succeed"
    assert (
        "SAGE apply infall module initialized" in stdout
    ), "Module should initialize with substeps"

    # Load output and verify HotGas is reasonable
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Just check that HotGas exists and is non-negative
    assert all(halos["HotGas"] >= 0), "HotGas should be non-negative"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Substep distribution works (4 substeps)")


def test_negative_infall_physics():
    """
    Test that negative infall is handled correctly (mass loss)

    Expected: When InfallingGas is negative, HotGas and EjectedGas are reduced appropriately
    Validates: Negative infall handling doesn't produce NaN or negative masses

    InfallingGas is a transport field that is not written (model_properties.yaml, output: false),
    so the negative budget is witnessed by its signature in the written reservoirs. In this
    infall-only pipeline the budget sets each FoF group's baryons to HaloBaryonFraction * Mvir
    (sage26_prepare_infall_budget, with Mvir read as a float), so no group ends below that
    allowance. A group can end above it only where a negative budget exceeded the central's ejected
    and hot gas: sage26_apply_infall then clears both with their metals, and the satellites' hot
    gas, which the budget does not consolidate, keeps the group above its allowance. Such groups
    must exist on the test trees (the positive witness of negative infall).
    """
    print("Testing negative infall physics...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage26_apply_infall_negative",
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

    # Filter to Type 0 centrals
    import numpy as np

    type0_mask = halos["Type"] == 0
    type0_halos = halos[type0_mask]

    # Check that all gas reservoirs are physically reasonable
    assert np.all(np.isfinite(type0_halos["HotGas"])), "HotGas should have finite values"
    assert np.all(
        type0_halos["HotGas"] >= 0
    ), "HotGas should be non-negative (negative infall handled correctly)"

    assert np.all(np.isfinite(type0_halos["EjectedGas"])), "EjectedGas should have finite values"
    assert np.all(
        type0_halos["EjectedGas"] >= 0
    ), "EjectedGas should be non-negative (negative infall handled correctly)"

    assert np.all(
        np.isfinite(type0_halos["MetalsHotGas"])
    ), "MetalsHotGas should have finite values"
    assert np.all(type0_halos["MetalsHotGas"] >= 0), "MetalsHotGas should be non-negative"

    # Each FoF group's baryons (every row carries its FoF central's UniqueGalaxyID) against its
    # allowance HaloBaryonFraction * Mvir
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
    allowance = type0_halos["HaloBaryonFraction"].astype(np.float64) * type0_halos["Mvir"].astype(
        np.float32
    ).astype(np.float64)

    # 1e-5 relative: float reservoirs
    below = group_baryons < allowance * (1.0 - 1e-5)
    assert not np.any(below), f"{int(np.sum(below))} FoF groups end below their baryon allowance"

    exhausted = group_baryons > allowance * (1.0 + 1e-5)
    num_negative = int(np.sum(exhausted))
    assert num_negative > 0, "some negative budgets should exceed the central's reservoirs"
    negative_infall_halos = type0_halos[exhausted]
    for field in ["HotGas", "MetalsHotGas", "EjectedGas", "MetalsEjectedGas"]:
        assert np.all(
            negative_infall_halos[field] == 0.0
        ), f"a central whose negative infall exceeded its reservoirs should hold no {field}"
    print(
        f"  ✓ Negative infall exhausted the central's hot and ejected gas in {num_negative} groups"
    )

    # Cleanup
    shutil.rmtree(temp_dir)


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: SAGE Add Infall Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    tests = [
        test_module_loads,
        test_output_properties_exist,
        test_with_sage26_prepare_infall_budget,
        test_memory_safety,
        test_execution_completes,
        test_substep_distribution,
        test_negative_infall_physics,
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
