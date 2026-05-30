#!/usr/bin/env python3
"""
SAGE Add Infall Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration

This test validates software quality aspects of the sage_apply_infall module:
- Module loads and initializes correctly
- Module executes without errors or memory leaks
- Output properties appear in output files
- Module works with sage_prepare_infall_budget in multi-phase pipeline
- Proper substep distribution

NOTE: sage_apply_infall requires sage_prepare_infall_budget (pre_timestep) to calculate InfallingGas

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: HotGas properties in output
  - test_with_sage_prepare_infall_budget: Integration with sage_prepare_infall_budget (required)
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_substep_distribution: Infall distributed over substeps
  - test_negative_infall_physics: Negative infall handling (HotGas/EjectedGas reduction)

Author: Mimic Development Team
Date: 2025-12-11
"""

import sys
import shutil
from pathlib import Path

# Add tests directory to path to import framework
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    MIMIC_EXE,
    create_test_param_file,
    run_mimic,
    load_binary_halos,
)

# ANSI color codes
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


def test_module_loads():
    """
    Test that sage_apply_infall module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    Note: Requires sage_prepare_infall_budget in pre_timestep to set InfallingGas property
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_infall_load",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo'), ('sage_prepare_infall_budget', 'process_full_halo')],
            'phase_1': [('sage_apply_infall', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Mimic should execute successfully with sage_apply_infall\nStderr: {stderr}"

    # Check initialization log message
    assert "SAGE apply infall module initialized" in stdout, \
        "sage_apply_infall should log initialization message"

    # Check that sage_prepare_infall_budget ran first
    assert "SAGE prepare infall budget module initialized" in stdout, \
        "sage_prepare_infall_budget should run before sage_apply_infall"

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
        output_name="sage_apply_infall_output",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo'), ('sage_prepare_infall_budget', 'process_full_halo')],
            'phase_1': [('sage_apply_infall', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
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
    assert 'HotGas' in halos.dtype.names, \
        "HotGas property should exist in output"
    assert 'MetalsHotGas' in halos.dtype.names, \
        "MetalsHotGas property should exist in output"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Output properties exist")
    print(f"  Found {len(halos)} halos")


def test_with_sage_prepare_infall_budget():
    """
    Test that sage_apply_infall works with sage_prepare_infall_budget module

    Expected: Both modules execute successfully together
    Validates: sage_apply_infall requires sage_prepare_infall_budget to set InfallingGas
    """
    print("Testing with sage_prepare_infall_budget...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_infall_with_infall",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo'), ('sage_prepare_infall_budget', 'process_full_halo')],
            'phase_1': [('sage_apply_infall', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Should run with both modules\nStderr: {stderr}"

    # Verify all modules initialized
    assert "SAGE reionization module initialized" in stdout, \
        "sage_reionization should initialize"
    assert "SAGE prepare infall budget module initialized" in stdout, \
        "sage_prepare_infall_budget should initialize"
    assert "SAGE apply infall module initialized" in stdout, \
        "sage_apply_infall should initialize"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Works with sage_prepare_infall_budget")


def test_memory_safety():
    """
    Test that sage_apply_infall doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_infall_memory",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo'), ('sage_prepare_infall_budget', 'process_full_halo')],
            'phase_1': [('sage_apply_infall', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution should succeed"
    assert "Memory leak detected" not in stdout, \
        "Should not have memory leaks"
    assert "Memory leak detected" not in stderr, \
        "Should not have memory leaks in stderr"

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
        output_name="sage_apply_infall_complete",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo'), ('sage_prepare_infall_budget', 'process_full_halo')],
            'phase_1': [('sage_apply_infall', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17},
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Pipeline should complete successfully"
    assert "SAGE apply infall module initialized" in stdout, \
        "Module initialization message"
    assert "SAGE apply infall module cleaned up" in stdout, \
        "Module cleanup message"

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
        output_name="sage_apply_infall_substeps",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo'), ('sage_prepare_infall_budget', 'process_full_halo')],
            'phase_1': [('sage_apply_infall', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
    )

    # Update substeps in the parameter file
    with open(param_file, 'r') as f:
        config = yaml.safe_load(f)

    config['modules']['substeps'] = 4

    with open(param_file, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with substeps should succeed"
    assert "SAGE apply infall module initialized" in stdout, \
        "Module should initialize with substeps"

    # Load output and verify HotGas is reasonable
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Just check that HotGas exists and is non-negative
    assert all(halos['HotGas'] >= 0), "HotGas should be non-negative"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Substep distribution works (4 substeps)")


def test_negative_infall_physics():
    """
    Test that negative infall is handled correctly (mass loss)

    Expected: When InfallingGas is negative, HotGas and EjectedGas are reduced appropriately
    Validates: Negative infall handling doesn't produce NaN or negative masses
    """
    print("Testing negative infall physics...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_apply_infall_negative",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo'), ('sage_prepare_infall_budget', 'process_full_halo')],
            'phase_1': [('sage_apply_infall', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
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
    type0_mask = halos['Type'] == 0
    type0_halos = halos[type0_mask]

    # Check that all gas reservoirs are physically reasonable
    assert np.all(np.isfinite(type0_halos['HotGas'])), \
        "HotGas should have finite values"
    assert np.all(type0_halos['HotGas'] >= 0), \
        "HotGas should be non-negative (negative infall handled correctly)"

    assert np.all(np.isfinite(type0_halos['EjectedGas'])), \
        "EjectedGas should have finite values"
    assert np.all(type0_halos['EjectedGas'] >= 0), \
        "EjectedGas should be non-negative (negative infall handled correctly)"

    assert np.all(np.isfinite(type0_halos['MetalsHotGas'])), \
        "MetalsHotGas should have finite values"
    assert np.all(type0_halos['MetalsHotGas'] >= 0), \
        "MetalsHotGas should be non-negative"

    # Check if there are any halos with InfallingGas in the data
    # (InfallingGas is calculated in sage_prepare_infall_budget, may be positive or negative)
    if 'InfallingGas' in type0_halos.dtype.names:
        infalling = type0_halos['InfallingGas']
        negative_infall_mask = infalling < 0

        if np.any(negative_infall_mask):
            num_negative = np.sum(negative_infall_mask)
            print(f"  Found {num_negative} halos with negative infall (mass loss)")

            # Verify these halos have reasonable gas reservoirs
            negative_infall_halos = type0_halos[negative_infall_mask]
            assert np.all(negative_infall_halos['HotGas'] >= 0), \
                "Halos with negative infall should still have non-negative HotGas"
            assert np.all(negative_infall_halos['EjectedGas'] >= 0), \
                "Halos with negative infall should still have non-negative EjectedGas"

            print(f"  ✓ Negative infall handled correctly for {num_negative} halos")
        else:
            print("  ✓ No negative infall cases in test data (edge case not tested)")
    else:
        print("  ✓ Gas reservoirs are physically reasonable")

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

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_module_loads,
        test_output_properties_exist,
        test_with_sage_prepare_infall_budget,
        test_memory_safety,
        test_execution_completes,
        test_substep_distribution,
        test_negative_infall_physics,
    ]

    passed = 0
    failed = 0

    for test in tests:
        print()
        try:
            test()
            passed += 1
        except AssertionError as e:
            print(f"{RED}✗ FAIL: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1
        except Exception as e:
            print(f"{RED}✗ ERROR: {test.__name__}{NC}")
            print(f"  {e}")
            failed += 1

    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed: {passed}")
    print(f"Failed: {failed}")
    print(f"Total:  {passed + failed}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        print()
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        print()
        return 1


if __name__ == "__main__":
    sys.exit(main())
