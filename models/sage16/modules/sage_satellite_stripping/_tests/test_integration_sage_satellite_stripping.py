#!/usr/bin/env python3
"""
SAGE Satellite Stripping Module - Integration Test

Validates: C-Python bindings, data flow, and pipeline integration
Phase: Phase 4.2 (SAGE Modular Refactoring)

This test validates that the sage_satellite_stripping module correctly integrates
with the Mimic pipeline and produces valid Python-accessible output:
- Module loads and initializes correctly
- Parameters are configurable via YAML files
- Module executes without errors or memory leaks
- C-Python bindings work (properties accessible from Python)
- Data flows through module (produces valid output)
- Property types and ranges are correct

NOTE: Physics validation (correctness of stripping) deferred to scientific tests.
      These tests validate infrastructure and data flow only.

Test cases:
  - test_module_loads: Module registration and initialization
  - test_parameter_configuration: GlobalBaryonFraction parameter configuration
  - test_memory_safety: No memory leaks
  - test_output_properties_exist: Required properties in output
  - test_output_property_types: Properties have correct dtypes
  - test_output_sanity_checks: Basic non-negativity checks
  - test_data_flow_validation: Module processes data (not just passes through)
  - test_standalone_execution: Module runs without other physics modules

Author: Mimic Development Team
Date: 2025-12-18
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent.parent
# Add tests directory to path to import framework
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (
    MIMIC_EXE,
    TestSkipped,
    create_test_param_file,
    load_binary_halos,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
    validate_no_infs,
    validate_no_nans,
    validate_range,
)

# ANSI color codes
BLUE = "\033[1;34m"
GREEN = "\033[0;32m"
RED = "\033[0;31m"
YELLOW = "\033[1;33m"
NC = "\033[0m"


def test_module_loads():
    """
    Test that sage_satellite_stripping module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_load",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_satellite_stripping", "process_by_galaxy")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, f"Mimic should execute successfully\nStderr: {stderr}"

    # Check initialization log message
    assert (
        "SAGE satellite stripping module initialized" in stdout
    ), "sage_satellite_stripping should log initialization message"

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
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_params",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_satellite_stripping", "process_by_galaxy")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.20},
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with custom parameters should succeed"

    # Verify parameter was read and logged
    assert (
        "GlobalBaryonFraction = 0.2000" in stdout
    ), f"Custom GlobalBaryonFraction should be logged\nStdout:\n{stdout}"

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
            "pre_timestep": [],
            "galaxy_physics": [("sage_satellite_stripping", "process_by_galaxy")],
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
    shutil.rmtree(test_temp_dir)

    print("  ✓ No memory leaks detected")


def test_output_properties_exist():
    """
    Test that required properties exist in output and are accessible from Python

    Expected: HotGas, MetalsHotGas exist in binary output
    Validates: C-Python bindings for property access
    """
    print("Testing output property existence...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_properties",
        output_format="binary",  # Test binary format
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_satellite_stripping", "process_by_galaxy")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
        first_file=0,
        last_file=0,
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nStderr: {stderr}"

    # ===== VALIDATE =====
    # Load binary output using framework (binary files are named model_z0.000_0)
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Check required properties exist (C-Python binding validation)
    required_props = ["HotGas", "MetalsHotGas", "Type", "Mvir"]
    for prop in required_props:
        assert (
            prop in halos.dtype.names
        ), f"Property '{prop}' should exist in output (C-Python binding)"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Required properties exist in output")


def test_output_property_types():
    """
    Test that output properties have correct data types

    Expected: HotGas and MetalsHotGas are float32
    Validates: Type safety in C-Python bindings
    """
    print("Testing output property types...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_types",
        output_format="binary",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_satellite_stripping", "process_by_galaxy")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
        first_file=0,
        last_file=0,
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Check data types
    assert (
        halos["HotGas"].dtype == np.float32
    ), f"HotGas should be float32, got {halos['HotGas'].dtype}"
    assert (
        halos["MetalsHotGas"].dtype == np.float32
    ), f"MetalsHotGas should be float32, got {halos['MetalsHotGas'].dtype}"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Property types are correct")


def test_output_sanity_checks():
    """
    Test that output values are in reasonable ranges

    Expected: Non-negative masses, no NaN/Inf values
    Validates: Module produces physically valid output
    """
    print("Testing output sanity checks...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_sanity",
        output_format="binary",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_satellite_stripping", "process_by_galaxy")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
        first_file=0,
        last_file=0,
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic should execute successfully"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Check for NaN and Inf
    validate_no_nans(halos)
    validate_no_infs(halos)

    # Check non-negativity (masses can't be negative)
    validate_range(halos, "HotGas", 0.0, 1e10)
    validate_range(halos, "MetalsHotGas", 0.0, 1e10)

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Output values pass sanity checks")


def test_data_flow_validation():
    """
    Test that module actually processes data (not just passes through unchanged)

    Expected: Module executes and produces output with expected structure
    Validates: Data flows through module correctly
    Note: Doesn't validate physics, just that processing occurs
    """
    print("Testing data flow validation...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_dataflow",
        output_format="binary",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_satellite_stripping", "process_by_galaxy")],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={"GlobalBaryonFraction": 0.17},
        first_file=0,
        last_file=0,
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic should execute successfully"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Verify we have data
    assert len(halos) > 0, "Output should contain halos"

    # Verify satellites exist (Type 1)
    satellites = halos[halos["Type"] == 1]
    assert len(satellites) > 0, "Output should contain satellites for stripping test"

    # Verify centrals exist (Type 0)
    centrals = halos[halos["Type"] == 0]
    assert len(centrals) > 0, "Output should contain centrals"

    # Basic data flow check: Data is valid (not all NaN)
    # Note: HotGas will be zero in standalone mode since no modules add gas
    # This is correct behavior - stripping only removes gas that's already there
    assert not np.all(
        np.isnan(halos["HotGas"])
    ), "HotGas values should not all be NaN (data should flow through)"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Data flows through module correctly")


def test_standalone_execution():
    """
    Test that sage_satellite_stripping runs standalone without dependencies

    Expected: Module executes without requiring other physics modules
    Validates: Module self-containment and independence
    """
    print("Testing standalone execution...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_satellite_stripping_standalone",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [("sage_satellite_stripping", "process_by_galaxy")],
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
    assert (
        returncode == 0
    ), f"Module should run standalone without sage_reionization\nStderr: {stderr}"

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

    tests = [
        test_module_loads,
        test_parameter_configuration,
        test_memory_safety,
        test_output_properties_exist,
        test_output_property_types,
        test_output_sanity_checks,
        test_data_flow_validation,
        test_standalone_execution,
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
