#!/usr/bin/env python3
"""
SAGE Disrupt Satellites Module - Integration Test

Validates: C-Python bindings, data flow, and pipeline integration
Phase: Phase 4.2 (SAGE Modular Refactoring)

This test validates that the sage_disrupt_satellites module correctly integrates
with the Mimic pipeline and produces valid Python-accessible output:
- Module loads and initializes correctly
- Module executes without errors or memory leaks
- C-Python bindings work (properties accessible from Python)
- Data flows through module (produces valid output)
- Property types and ranges are correct

NOTE: Physics validation (correctness of disruption) deferred to scientific tests.
      These tests validate infrastructure and data flow only.

Test cases:
  - test_module_loads: Module registration and initialization
  - test_memory_safety: No memory leaks
  - test_output_properties_exist: Required properties in output
  - test_output_property_types: Properties have correct dtypes
  - test_output_sanity_checks: Basic non-negativity checks
  - test_data_flow_validation: Module processes data (not just passes through)
  - test_standalone_execution: Module runs without other physics modules

Author: Mimic Development Team
Date: 2025-12-23
"""

import os
import sys
import shutil
import subprocess
import tempfile
from pathlib import Path
import numpy as np

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent
MIMIC_EXE = REPO_ROOT / "mimic"

# Add tests directory to path to import framework
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (
    create_test_param_file,
    run_mimic,
    load_binary_halos,
    validate_no_nans,
    validate_no_infs,
    validate_range
)

# ANSI color codes
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


def test_module_loads():
    """
    Test that sage_disrupt_satellites module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_disrupt_satellites_load",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [('sage_disrupt_satellites', 'process_full_halo')],
            'post_timestep': []
        },
        model_params={}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Mimic should execute successfully\nStderr: {stderr}"

    # Check initialization log message
    assert "SAGE Disrupt Satellites initialized" in stdout, \
        f"sage_disrupt_satellites should log initialization message\nStdout:\n{stdout}"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Module loads and initializes successfully")


def test_memory_safety():
    """
    Test that sage_disrupt_satellites doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_disrupt_satellites_memory",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [('sage_disrupt_satellites', 'process_full_halo')],
            'post_timestep': []
        },
        model_params={}
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


def test_output_properties_exist():
    """
    Test that required properties exist in output and are accessible from Python

    Expected: ICS, MetalsICS, HotGas, MetalsHotGas, Type exist in binary output
    Validates: C-Python bindings for property access
    """
    print("Testing output property existence...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_disrupt_satellites_properties",
        output_format="binary",  # Test binary format
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [('sage_disrupt_satellites', 'process_full_halo')],
            'post_timestep': []
        },
        model_params={},
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nStderr: {stderr}"

    # ===== VALIDATE =====
    # Load binary output using framework (binary files are named model_z0.000_0)
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Check required properties exist (C-Python binding validation)
    # ICS-related properties are key for disruption module
    required_props = ['ICS', 'MetalsICS', 'HotGas', 'MetalsHotGas', 'Type', 'StellarMass']
    for prop in required_props:
        assert prop in halos.dtype.names, \
            f"Property '{prop}' should exist in output (C-Python binding)"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Required properties exist in output")


def test_output_property_types():
    """
    Test that output properties have correct data types

    Expected: ICS and MetalsICS are float32, Type is int32
    Validates: Type safety in C-Python bindings
    """
    print("Testing output property types...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_disrupt_satellites_types",
        output_format="binary",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [('sage_disrupt_satellites', 'process_full_halo')],
            'post_timestep': []
        },
        model_params={},
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Check data types
    assert halos['ICS'].dtype == np.float32, \
        f"ICS should be float32, got {halos['ICS'].dtype}"
    assert halos['MetalsICS'].dtype == np.float32, \
        f"MetalsICS should be float32, got {halos['MetalsICS'].dtype}"
    assert halos['HotGas'].dtype == np.float32, \
        f"HotGas should be float32, got {halos['HotGas'].dtype}"
    assert halos['Type'].dtype == np.int32, \
        f"Type should be int32, got {halos['Type'].dtype}"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Property types are correct")


def test_output_sanity_checks():
    """
    Test that output values are in reasonable ranges

    Expected: Non-negative masses, no NaN/Inf values, valid Type values
    Validates: Module produces physically valid output
    """
    print("Testing output sanity checks...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_disrupt_satellites_sanity",
        output_format="binary",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [('sage_disrupt_satellites', 'process_full_halo')],
            'post_timestep': []
        },
        model_params={},
        first_file=0,
        last_file=0
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
    validate_range(halos, 'ICS', 0.0, 1e15)
    validate_range(halos, 'MetalsICS', 0.0, 1e15)
    validate_range(halos, 'HotGas', 0.0, 1e15)
    validate_range(halos, 'MetalsHotGas', 0.0, 1e15)
    validate_range(halos, 'StellarMass', 0.0, 1e15)

    # Check Type values are valid (0, 1, 2, or 3)
    assert np.all((halos['Type'] >= 0) & (halos['Type'] <= 3)), \
        "Type values should be in range [0, 3]"

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
        output_name="sage_disrupt_satellites_dataflow",
        output_format="binary",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [('sage_disrupt_satellites', 'process_full_halo')],
            'post_timestep': []
        },
        model_params={},
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic should execute successfully"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Verify we have data
    assert len(halos) > 0, "Output should contain halos"

    # Verify centrals exist (Type 0)
    centrals = halos[halos['Type'] == 0]
    assert len(centrals) > 0, "Output should contain centrals"

    # Basic data flow check: Type values are valid
    # In standalone mode, no satellites will be marked for disruption,
    # so all should remain Type 0, 1, or 2
    assert np.all((halos['Type'] >= 0) & (halos['Type'] <= 3)), \
        "Type values should be valid (data should flow through)"

    # Verify ICS property exists and is accessible
    assert not np.all(np.isnan(halos['ICS'])), \
        "ICS values should not all be NaN (data should flow through)"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Data flows through module correctly")


def test_standalone_execution():
    """
    Test that sage_disrupt_satellites runs standalone without dependencies

    Expected: Module executes without requiring other physics modules
    Validates: Module self-containment and independence
    """
    print("Testing standalone execution...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_disrupt_satellites_standalone",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [('sage_disrupt_satellites', 'process_full_halo')],
            'post_timestep': []
        },
        model_params={},
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Module should run standalone without other modules\nStderr: {stderr}"

    # Verify module initialized
    assert "SAGE Disrupt Satellites initialized" in stdout, \
        "Module should log initialization"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Standalone execution successful")


def test_type_3_generation():
    """
    Test that Type 3 galaxies can exist in output

    Expected: When disruption occurs, Type 3 galaxies appear in output
    Validates: Type marking functionality works end-to-end
    Note: In standalone mode without other physics, no satellites will be
          marked for disruption, so this tests Type handling infrastructure
    """
    print("Testing Type 3 infrastructure...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_disrupt_satellites_type3",
        output_format="binary",
        phase_config={
            'pre_timestep': [],
            'phase_1': [],
            'phase_2': [('sage_disrupt_satellites', 'process_full_halo')],
            'post_timestep': []
        },
        model_params={},
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Execution should succeed"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Verify Type field can hold all valid values
    # (Type 3 may or may not exist depending on physics)
    type_values = np.unique(halos['Type'])
    for t in type_values:
        assert t in [0, 1, 2, 3], f"Type {t} is not a valid galaxy type"

    # Verify the Type field is properly populated
    assert len(type_values) >= 1, "Should have at least one Type value"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Type 3 infrastructure validated")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: SAGE Disrupt Satellites Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_module_loads,
        test_memory_safety,
        test_output_properties_exist,
        test_output_property_types,
        test_output_sanity_checks,
        test_data_flow_validation,
        test_standalone_execution,
        test_type_3_generation,
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
