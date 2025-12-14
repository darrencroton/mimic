#!/usr/bin/env python3
"""
SAGE Reionization Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration

This test validates software quality aspects of the sage_reionization module:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- Output properties appear in output files
- HaloBaryonFraction property is set correctly

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: HaloBaryonFraction property in output
  - test_parameters_configurable: GlobalBaryonFraction parameter configuration
  - test_property_values_physical: HaloBaryonFraction values are physical
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion

Author: Mimic Development Team
Date: 2025-12-11
"""

import sys
import shutil
from pathlib import Path

# Add tests directory to path to import framework
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
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
    Test that sage_reionization module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    Note: sage_reionization runs standalone (no dependencies)
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_reionization_load",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo')],
            'phase_1': [],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Mimic should execute successfully with sage_reionization\nStderr: {stderr}"

    # Check initialization log message
    assert "SAGE reionization module initialized" in stdout, \
        "sage_reionization should log initialization message"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Module loads and initializes successfully")


def test_output_properties_exist():
    """
    Test that HaloBaryonFraction property appears in output

    Expected: HaloBaryonFraction in output file
    Validates: Module creates expected output properties
    """
    print("Testing output properties...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_reionization_output",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo')],
            'phase_1': [],
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

    # Check output property exists
    assert 'HaloBaryonFraction' in halos.dtype.names, \
        "HaloBaryonFraction property should exist in output"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Output properties exist")
    print(f"  Found {len(halos)} halos")


def test_parameters_configurable():
    """
    Test that sage_reionization uses GlobalBaryonFraction model parameter

    Expected: Custom GlobalBaryonFraction value is read from model_parameters and logged
    Validates: Model parameter reading and usage
    """
    print("Testing parameter configuration...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_reionization_params",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo')],
            'phase_1': [],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.20}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with custom parameters should succeed"

    # Verify parameter was read and affects output
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # With GlobalBaryonFraction = 0.20, HaloBaryonFraction should be <= 0.20
    # (can be less due to reionization suppression)
    halos_with_mass = halos[halos['Mvir'] > 0]
    assert (halos_with_mass['HaloBaryonFraction'] <= 0.20).all(), \
        "HaloBaryonFraction should be <= custom GlobalBaryonFraction"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Parameters are configurable")


def test_property_values_physical():
    """
    Test that HaloBaryonFraction values are physical

    Expected: 0 <= HaloBaryonFraction <= GlobalBaryonFraction for all halos
    Validates: Property values are within physical bounds
    """
    print("Testing property values are physical...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_reionization_physical",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo')],
            'phase_1': [],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Execution should succeed"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Check property is physical (0 <= value <= 0.17 for all halos)
    assert (halos['HaloBaryonFraction'] >= 0).all(), \
        "HaloBaryonFraction should be >= 0"
    assert (halos['HaloBaryonFraction'] <= 0.17).all(), \
        "HaloBaryonFraction should be <= GlobalBaryonFraction"

    # Check property is set for halos with mass (Mvir > 0)
    # Orphans with Mvir=0 legitimately have HaloBaryonFraction=0
    halos_with_mass = halos[halos['Mvir'] > 0]
    assert (halos_with_mass['HaloBaryonFraction'] > 0).all(), \
        "HaloBaryonFraction should be > 0 for halos with mass"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Property values are physical")


def test_memory_safety():
    """
    Test that sage_reionization doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_reionization_memory",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo')],
            'phase_1': [],
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
        output_name="sage_reionization_complete",
        phase_config={
            'pre_timestep': [('sage_reionization', 'process_full_halo')],
            'phase_1': [],
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
    assert "SAGE reionization module initialized" in stdout, \
        "Module initialization message"
    assert "SAGE reionization module cleaned up" in stdout, \
        "Module cleanup message"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Full pipeline completes")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: SAGE Reionization Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_module_loads,
        test_output_properties_exist,
        test_parameters_configurable,
        test_property_values_physical,
        test_memory_safety,
        test_execution_completes,
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
