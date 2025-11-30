#!/usr/bin/env python3
"""
SAGE Satellite Stripping Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration
Phase: Phase 4.2 (SAGE Modular Refactoring)

This test validates software quality aspects of the sage_satellite_stripping module:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- Module works correctly with sage_infall (shared reionization utility)

NOTE: Physics validation (stripping correctness) deferred to Phase 4.3+
      when downstream modules are implemented for end-to-end testing.

Test cases:
  - test_module_loads: Module registration and initialization
  - test_parameter_configuration: BaryonFrac parameter configuration
  - test_with_sage_infall: Integration with sage_infall module
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion

Author: Mimic Development Team
Date: 2025-11-26
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
from framework import load_binary_halos, create_test_param_file

# Test state
temp_dir = None

# ANSI color codes (module-level constants)
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


def run_mimic(param_file):
    """
    Execute Mimic with specified parameter file

    Args:
        --verbose (required to capture stdout validation)
        param_file (Path): Path to parameter file

    Returns:
        tuple: (returncode, stdout, stderr)
    """
    result = subprocess.run(
        [str(MIMIC_EXE), "--verbose", str(param_file)],
        capture_output=True,
        text=True,
        timeout=60  # 60 second timeout for integration tests
    )
    return result.returncode, result.stdout, result.stderr


def test_module_loads():
    """
    Test that sage_satellite_stripping module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    Note: Reionization parameters hardcoded in shared/reionization.h
    Note: Phase 4.4 - BaryonFrac from model_parameters (centralized)
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_load",
        enabled_modules=["sage_satellite_stripping"],
        model_params={"BaryonFrac": 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Mimic should execute successfully\nStderr: {stderr}"

    # Check initialization log message
    assert "SAGE satellite stripping module initialized" in stdout, \
        "sage_satellite_stripping should log initialization message"

    # Check that reionization model is logged (hardcoded in header)
    assert "Gnedin (2000)" in stdout, \
        "Should log reionization model from shared header"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Module loads and initializes successfully")


def test_parameter_configuration():
    """
    Test that sage_satellite_stripping uses BaryonFrac model parameter

    Expected: Custom BaryonFrac value is read from model_parameters and logged
    Validates: Model parameter reading and usage
    Note: Phase 4.4 - BaryonFrac is now a global model parameter, not module-specific
    """
    print("Testing parameter configuration...")

    # ===== SETUP =====
    import yaml

    # Create parameter file with custom BaryonFrac
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_params",
        enabled_modules=["sage_satellite_stripping"],
        model_params={"BaryonFrac": 0.20}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with custom parameters should succeed"

    # Verify parameter was read (from model_parameters, not module-specific)
    # Note: BaryonFrac is logged during infall/stripping module initialization
    assert "BaryonFrac = 0.2000" in stdout or "BaryonFrac: 0.2" in stdout, \
        f"Custom BaryonFrac should be logged\nStdout:\n{stdout}"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Parameters are configurable")


def test_with_sage_infall():
    """
    Test that sage_satellite_stripping works with sage_infall module

    Expected: Both modules execute successfully together
    Validates: Modules work together sharing reionization utility
    """
    print("Testing with sage_infall...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_infall",
        enabled_modules=["sage_infall", "sage_satellite_stripping"],
        model_params={
            "BaryonFrac": 0.17
        }
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Should run with both modules\nStderr: {stderr}"

    # Verify both modules initialized
    assert "SAGE infall module initialized" in stdout, \
        "sage_infall should initialize"
    assert "SAGE satellite stripping module initialized" in stdout, \
        "sage_satellite_stripping should initialize"

    # Both modules use shared reionization.h
    assert stdout.count("Gnedin (2000)") >= 2, \
        "Both modules should log reionization model from shared header"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Works with sage_infall")


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
        enabled_modules=["sage_satellite_stripping"],
        model_params={"BaryonFrac": 0.17}
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
        enabled_modules=["sage_satellite_stripping"],
        model_params={"BaryonFrac": 0.17},
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

    try:
        tests = [
            test_module_loads,
            test_parameter_configuration,
            test_with_sage_infall,
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

    except Exception as e:
        print(f"{RED}ERROR: Test suite failed: {e}{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
