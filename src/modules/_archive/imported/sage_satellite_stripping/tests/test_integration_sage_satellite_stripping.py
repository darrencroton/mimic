#!/usr/bin/env python3
"""
SAGE Satellite Stripping Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration
Phase: Phase 4.2 (SAGE Modular Refactoring)

This test validates software quality aspects of the sage_satellite_stripping module
running in standalone mode (no dependencies on other physics modules):
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- Module integrates correctly with multi-phase pipeline

NOTE: Physics validation (actual stripping) deferred to Phase 4.3+ when full
      pipeline is available. These tests validate module infrastructure only.

Test cases:
  - test_module_loads: Module registration and initialization
  - test_parameter_configuration: GlobalBaryonFraction parameter configuration
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_standalone_execution: Module runs without other physics modules

Author: Mimic Development Team
Date: 2025-12-11
"""

import os
import sys
import shutil
import subprocess
import tempfile
from pathlib import Path

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
MIMIC_EXE = REPO_ROOT / "mimic"

# Add tests directory to path to import framework
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import create_test_param_file, run_mimic

# ANSI color codes
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


def test_module_loads():
    """
    Test that sage_satellite_stripping module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    Note: Runs standalone without other physics modules
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_load",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('sage_satellite_stripping', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={"GlobalBaryonFraction": 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Mimic should execute successfully\nStderr: {stderr}"

    # Check initialization log message (VERBOSE_LOG)
    assert "SAGE satellite stripping module initialized" in stdout, \
        "sage_satellite_stripping should log initialization message"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Module loads and initializes successfully")


def test_parameter_configuration():
    """
    Test that sage_satellite_stripping reads GlobalBaryonFraction parameter

    Expected: Custom GlobalBaryonFraction value is read from model_parameters and logged
    Validates: Model parameter reading and validation
    """
    print("Testing parameter configuration...")

    # ===== SETUP =====
    import yaml

    # Create parameter file with custom GlobalBaryonFraction
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_params",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('sage_satellite_stripping', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={"GlobalBaryonFraction": 0.20}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with custom parameters should succeed"

    # Verify parameter was read and logged (VERBOSE_LOG)
    assert "GlobalBaryonFraction = 0.2000" in stdout, \
        f"Custom GlobalBaryonFraction should be logged\nStdout:\n{stdout}"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Parameters are configurable")


def test_memory_safety():
    """
    Test that sage_satellite_stripping doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_memory",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('sage_satellite_stripping', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={"GlobalBaryonFraction": 0.17}
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
    shutil.rmtree(test_temp_dir)

    print("  ✓ No memory leaks detected")


def test_execution_completes():
    """
    Test that full pipeline execution completes without errors

    Expected: Initialization, processing, and cleanup all succeed
    Validates: Complete module lifecycle
    """
    print("Testing full pipeline completion...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_complete",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('sage_satellite_stripping', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={"GlobalBaryonFraction": 0.17},
        first_file=0,
        last_file=0  # Process single file
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Pipeline should complete successfully"
    assert "SAGE satellite stripping module initialized" in stdout, \
        "Module initialization message"
    assert "SAGE satellite stripping module cleaned up" in stdout, \
        "Module cleanup message"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Full pipeline completes")


def test_standalone_execution():
    """
    Test that sage_satellite_stripping runs standalone without dependencies

    Expected: Module executes without requiring other physics modules
    Validates: Module self-containment and independence
    Note: Module uses GlobalBaryonFraction fallback when HaloBaryonFraction not set
    """
    print("Testing standalone execution...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_standalone",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('sage_satellite_stripping', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={"GlobalBaryonFraction": 0.17},
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Module should run standalone without sage_reionization or sage_calculate_infall\nStderr: {stderr}"

    # Verify module initialized and cleaned up
    assert "SAGE satellite stripping module initialized" in stdout
    assert "SAGE satellite stripping module cleaned up" in stdout

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Standalone execution successful")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: SAGE Satellite Stripping Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_module_loads,
        test_parameter_configuration,
        test_memory_safety,
        test_execution_completes,
        test_standalone_execution,
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
