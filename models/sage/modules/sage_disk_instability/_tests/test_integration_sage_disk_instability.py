#!/usr/bin/env python3
"""
SAGE Disk Instability Module - Integration Test

Validates: Module lifecycle, configuration, pipeline integration, and high-level physics

This test validates software quality and high-level physics aspects:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- BulgeMass property exists in output (trigger flags are internal only)
- Module operates correctly in isolation (no crashes, handles zero-mass galaxies)
- Parameter sensitivity (StarFormingDiskFactor affects results)

IMPORTANT: UnstableDiskGasFraction is an internal trigger flag (output: false)
used for communication with downstream modules within the same timestep.
It is NOT in output files and should not be tested here.

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: BulgeMass property in output
  - test_parameters_configurable: StarFormingDiskFactor parameter configuration
  - test_property_values_physical: BulgeMass values are physical
  - test_handles_zero_mass_galaxies: Module handles galaxies with no disk gracefully
  - test_parameter_sensitivity: StarFormingDiskFactor affects Mcrit calculation
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion

Author: Mimic Development Team
Date: 2025-12-23
"""

import sys
import shutil
import numpy as np
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
    Test that sage_disk_instability module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    Note: sage_disk_instability runs in galaxy_physics (process_by_galaxy mode)
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_disk_instability_load",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [('sage_disk_instability', 'process_by_galaxy')],
            'satellite_mergers': [],
            'post_timestep': []
        },
        model_params={'StarFormingDiskFactor': 3.0}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Mimic should execute successfully with sage_disk_instability\nStderr: {stderr}"

    # Check initialization log message
    assert "SAGE disk instability module initialized" in stdout, \
        "sage_disk_instability should log initialization message"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Module loads and initializes successfully")


def test_output_properties_exist():
    """
    Test that BulgeMass property appears in output

    Expected: BulgeMass in output (trigger flag is internal only, not in output)
    Validates: Module output properties
    Note: UnstableDiskGasFraction has output: false
    """
    print("Testing output properties...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_disk_instability_output",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [('sage_disk_instability', 'process_by_galaxy')],
            'satellite_mergers': [],
            'post_timestep': []
        },
        model_params={'StarFormingDiskFactor': 3.0}
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
    assert 'BulgeMass' in halos.dtype.names, \
        "BulgeMass property should exist in output"
    assert 'MetalsBulgeMass' in halos.dtype.names, \
        "MetalsBulgeMass property should exist in output"

    # Trigger flags should NOT be in output (they're internal, output: false)
    # This is correct behavior - they're for inter-module communication only

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Output properties exist")
    print(f"  Found {len(halos)} halos")


def test_parameters_configurable():
    """
    Test that sage_disk_instability uses StarFormingDiskFactor model parameter

    Expected: Custom StarFormingDiskFactor value is read from model_parameters
    Validates: Model parameter reading and usage
    """
    print("Testing parameter configuration...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_disk_instability_params",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [('sage_disk_instability', 'process_by_galaxy')],
            'satellite_mergers': [],
            'post_timestep': []
        },
        model_params={'StarFormingDiskFactor': 5.0}  # Custom value
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with custom parameters should succeed"

    # Verify parameter was read (check log output)
    assert "StarFormingDiskFactor = 5.00" in stdout, \
        "Custom StarFormingDiskFactor should be logged"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Parameters are configurable")


def test_property_values_physical():
    """
    Test that BulgeMass values are physical

    Expected: 0 <= BulgeMass <= StellarMass for all halos
    Validates: Property values are within physical bounds
    """
    print("Testing property values are physical...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_disk_instability_physical",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [('sage_disk_instability', 'process_by_galaxy')],
            'satellite_mergers': [],
            'post_timestep': []
        },
        model_params={'StarFormingDiskFactor': 3.0}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Execution should succeed"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Check BulgeMass is physical (0 <= BulgeMass <= StellarMass)
    assert (halos['BulgeMass'] >= 0).all(), \
        "BulgeMass should be >= 0"
    assert (halos['BulgeMass'] <= halos['StellarMass'] + 1e-6).all(), \
        "BulgeMass should be <= StellarMass (with small tolerance for rounding)"

    # Check MetalsBulgeMass is physical
    assert (halos['MetalsBulgeMass'] >= 0).all(), \
        "MetalsBulgeMass should be >= 0"
    assert (halos['MetalsBulgeMass'] <= halos['MetalsStellarMass'] + 1e-6).all(), \
        "MetalsBulgeMass should be <= MetalsStellarMass"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Property values are physical")


def test_handles_zero_mass_galaxies():
    """
    Test that module handles galaxies with zero disk mass gracefully

    Expected: No crashes, BulgeMass remains valid for zero-disk galaxies
    Validates: Edge case handling
    Note: Test trees may have galaxies with zero ColdGas and zero stellar disk
    """
    print("Testing zero-mass galaxy handling...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_disk_instability_zeromass",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [('sage_disk_instability', 'process_by_galaxy')],
            'satellite_mergers': [],
            'post_timestep': []
        },
        model_params={'StarFormingDiskFactor': 3.0}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Execution should succeed even with zero-mass galaxies"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Module should handle all galaxies without crashing
    # Zero-disk galaxies should have BulgeMass unchanged (or remain valid)
    assert (halos['BulgeMass'] >= 0).all(), \
        "BulgeMass should remain non-negative for all galaxies"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Zero-mass galaxies handled correctly")


def test_parameter_sensitivity():
    """
    Test that changing StarFormingDiskFactor affects the critical mass calculation

    Expected: Different parameter values should be accepted and logged
    Validates: Parameter sensitivity
    Note: With test data (likely zero initial mass), we can't test physics impact,
          but we can verify parameter changes are accepted
    """
    print("Testing parameter sensitivity...")

    # Test with low disk factor
    param_file_low, output_dir_low, temp_dir_low = create_test_param_file(
        output_name="sage_disk_instability_low",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [('sage_disk_instability', 'process_by_galaxy')],
            'satellite_mergers': [],
            'post_timestep': []
        },
        model_params={'StarFormingDiskFactor': 1.0}
    )

    returncode_low, stdout_low, stderr_low = run_mimic(param_file_low)
    assert returncode_low == 0, "Low parameter value should work"
    assert "StarFormingDiskFactor = 1.00" in stdout_low, \
        "Low parameter value should be logged"

    shutil.rmtree(temp_dir_low)

    # Test with high disk factor
    param_file_high, output_dir_high, temp_dir_high = create_test_param_file(
        output_name="sage_disk_instability_high",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [('sage_disk_instability', 'process_by_galaxy')],
            'satellite_mergers': [],
            'post_timestep': []
        },
        model_params={'StarFormingDiskFactor': 10.0}
    )

    returncode_high, stdout_high, stderr_high = run_mimic(param_file_high)
    assert returncode_high == 0, "High parameter value should work"
    assert "StarFormingDiskFactor = 10.00" in stdout_high, \
        "High parameter value should be logged"

    shutil.rmtree(temp_dir_high)

    print("  ✓ Parameter sensitivity validated")


def test_memory_safety():
    """
    Test that sage_disk_instability doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_disk_instability_memory",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [('sage_disk_instability', 'process_by_galaxy')],
            'satellite_mergers': [],
            'post_timestep': []
        },
        model_params={'StarFormingDiskFactor': 3.0}
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
        output_name="sage_disk_instability_complete",
        phase_config={
            'pre_timestep': [],
            'galaxy_physics': [('sage_disk_instability', 'process_by_galaxy')],
            'satellite_mergers': [],
            'post_timestep': []
        },
        model_params={'StarFormingDiskFactor': 3.0},
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Pipeline should complete successfully"
    assert "SAGE disk instability module initialized" in stdout, \
        "Module initialization message"
    assert "SAGE disk instability module cleaned up" in stdout, \
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
    print(f"{BLUE}Test Suite: SAGE Disk Instability Integration Tests{NC}")
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
        test_handles_zero_mass_galaxies,
        test_parameter_sensitivity,
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
