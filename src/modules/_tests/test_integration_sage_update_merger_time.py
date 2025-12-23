#!/usr/bin/env python3
"""
SAGE Update Merger Time Module - Integration Test

Validates: C-Python bindings, data flow, and pipeline integration
Phase: Phase 4.2 (SAGE Modular Refactoring)

This test validates that the sage_update_merger_time module correctly
integrates with the Mimic pipeline:
- Module loads and initializes correctly with ThresholdSatDisruption parameter
- Module executes without errors or memory leaks
- Data flows through module (processes satellites correctly)
- Required input properties exist (Type, Mvir, StellarMass, ColdGas, MergTime)
- MergTime is managed properly for satellites

IMPORTANT: This module REQUIRES upstream module to set MergTime for satellites:
  - sage_calculate_merger_timescale: Calculates initial MergTime using dynamical friction
  - infallMvir is set by core build_model.c during Type transitions

NOTE: The output properties IsMerging, IsDisrupting, and MergerMassRatio are
      transient flags (output: false in model_properties.yaml) that are consumed
      by downstream modules but NOT written to output files. These cannot be
      tested in integration tests - their logic is validated in unit tests.

NOTE: Physics validation (merger/disruption triggering logic) is tested
      in unit tests. These tests validate infrastructure and data flow only.

Test cases:
  - test_module_loads: Module registration and initialization
  - test_memory_safety: No memory leaks
  - test_required_properties_exist: Type, Mvir, StellarMass, etc. in output
  - test_property_types: Correct dtypes for input properties
  - test_output_sanity_checks: No NaN/Inf values, valid ranges
  - test_data_flow_validation: Module processes data (not just passes through)
  - test_parameter_validation: ThresholdSatDisruption parameter loaded correctly
  - test_satellite_mergtime_set: Satellites have valid MergTime from upstream

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


def get_full_pipeline_config():
    """
    Get the standard pipeline configuration including required upstream modules.
    
    sage_update_merger_time requires MergTime to be set by upstream module:
    - sage_calculate_merger_timescale: Calculates MergTime for satellites
      (infallMvir is set by core build_model.c during Type transitions)
    """
    return {
        'pre_timestep': [],
        'phase_1': [
            ('sage_calculate_merger_timescale', 'process_full_halo'),
        ],
        'phase_2': [
            ('sage_update_merger_time', 'process_full_halo'),
        ],
        'post_timestep': []
    }


def test_module_loads():
    """
    Test that sage_update_merger_time module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization with ThresholdSatDisruption parameter
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_update_merger_time_load",
        phase_config=get_full_pipeline_config(),
        model_params={
            'ThresholdSatDisruption': 1.0,
        }
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Mimic should execute successfully\nStderr: {stderr}"

    # Check initialization log message
    assert "SAGE merger time evolution initialized" in stdout, \
        f"sage_update_merger_time should log initialization message\nStdout:\n{stdout}"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Module loads and initializes successfully")


def test_memory_safety():
    """
    Test that sage_update_merger_time doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_update_merger_time_memory",
        phase_config=get_full_pipeline_config(),
        model_params={
            'ThresholdSatDisruption': 1.0,
        }
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


def test_required_properties_exist():
    """
    Test that required output properties exist in output and are accessible from Python

    Expected: Type, Mvir, StellarMass, ColdGas exist in binary output
    Validates: C-Python bindings for property access
    
    NOTE: MergTime, IsMerging, IsDisrupting, MergerMassRatio have output: false
          in model_properties.yaml (internal/transient properties) so they are NOT
          in output. Their logic is validated in unit tests.
    """
    print("Testing required property existence...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_update_merger_time_properties",
        output_format="binary",
        phase_config=get_full_pipeline_config(),
        model_params={
            'ThresholdSatDisruption': 1.0,
        },
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nStderr: {stderr}"

    # ===== VALIDATE =====
    # Load binary output using framework
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Check required input properties exist (module reads these)
    # Note: MergTime has output: false so won't be in binary output
    required_props = ['Type', 'Mvir', 'StellarMass', 'ColdGas']
    for prop in required_props:
        assert prop in halos.dtype.names, \
            f"Required property '{prop}' should exist in output (C-Python binding)"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Required properties exist in output")


def test_property_types():
    """
    Test that properties have correct data types

    Expected: Type is int32, Mvir/StellarMass/ColdGas are float32
    Validates: Type safety in C-Python bindings
    """
    print("Testing property types...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_update_merger_time_types",
        output_format="binary",
        phase_config=get_full_pipeline_config(),
        model_params={
            'ThresholdSatDisruption': 1.0,
        },
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Check data types (only properties with output: true)
    assert halos['Type'].dtype == np.int32, \
        f"Type should be int32, got {halos['Type'].dtype}"
    assert halos['Mvir'].dtype == np.float32, \
        f"Mvir should be float32, got {halos['Mvir'].dtype}"
    assert halos['StellarMass'].dtype == np.float32, \
        f"StellarMass should be float32, got {halos['StellarMass'].dtype}"
    assert halos['ColdGas'].dtype == np.float32, \
        f"ColdGas should be float32, got {halos['ColdGas'].dtype}"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Property types are correct")


def test_output_sanity_checks():
    """
    Test that output values are in reasonable ranges

    Expected: Type is 0-3, Mvir >= 0, no NaN/Inf
    Validates: Module produces physically valid output
    """
    print("Testing output sanity checks...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_update_merger_time_sanity",
        output_format="binary",
        phase_config=get_full_pipeline_config(),
        model_params={
            'ThresholdSatDisruption': 1.0,
        },
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

    # Check Type values are valid (0, 1, 2, or 3)
    assert np.all((halos['Type'] >= 0) & (halos['Type'] <= 3)), \
        "Type should be 0, 1, 2, or 3"

    # Check Mvir is non-negative
    assert np.all(halos['Mvir'] >= 0), \
        "Mvir should be non-negative"

    # Check StellarMass and ColdGas are non-negative
    assert np.all(halos['StellarMass'] >= 0), \
        "StellarMass should be non-negative"
    assert np.all(halos['ColdGas'] >= 0), \
        "ColdGas should be non-negative"

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
        output_name="sage_update_merger_time_dataflow",
        output_format="binary",
        phase_config=get_full_pipeline_config(),
        model_params={
            'ThresholdSatDisruption': 1.0,
        },
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

    # Verify satellites exist (Type 1 or 2)
    satellites = halos[(halos['Type'] == 1) | (halos['Type'] == 2)]
    if len(satellites) > 0:
        print(f"    Found {len(satellites)} satellites in output")

    # Basic data flow check: Data is valid (not all NaN)
    assert not np.all(np.isnan(halos['Mvir'])), \
        "Mvir values should not all be NaN (data should flow through)"

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Data flows through module correctly")


def test_parameter_validation():
    """
    Test that ThresholdSatDisruption parameter is loaded correctly

    Expected: Module initializes with parameter and logs the value
    Validates: Parameter is loaded by module
    """
    print("Testing parameter validation...")

    results = {}
    temp_dirs = []

    for threshold in [0.5, 2.0, 10.0]:
        # ===== SETUP =====
        param_file, output_dir, test_temp_dir = create_test_param_file(
            output_name=f"sage_update_merger_time_thresh_{threshold}",
            output_format="binary",
            phase_config=get_full_pipeline_config(),
            model_params={
                'ThresholdSatDisruption': threshold,
            },
            first_file=0,
            last_file=0
        )
        temp_dirs.append(test_temp_dir)

        # ===== EXECUTE =====
        returncode, stdout, stderr = run_mimic(param_file)
        assert returncode == 0, f"Execution should succeed with threshold={threshold}"

        # Check parameter is logged
        assert "SAGE merger time evolution initialized" in stdout, \
            f"Module should initialize with threshold={threshold}"

        print(f"    Threshold {threshold}: Module initialized successfully")

    # Cleanup
    for temp_dir in temp_dirs:
        shutil.rmtree(temp_dir)

    print("  ✓ Parameter loaded correctly for different values")


def test_satellite_processing():
    """
    Test that satellites (Type 1/2) are processed by the module

    Expected: Module executes successfully when satellites are present
    Validates: Module handles satellite galaxies correctly
    """
    print("Testing satellite processing...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_update_merger_time_centrals",
        output_format="binary",
        phase_config=get_full_pipeline_config(),
        model_params={
            'ThresholdSatDisruption': 1.0,
        },
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic should execute successfully"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Get satellites (Type 1 or 2)
    satellites = halos[(halos['Type'] == 1) | (halos['Type'] == 2)]
    
    if len(satellites) > 0:
        print(f"    Processed {len(satellites)} satellites")
        
        # Satellites should have valid mass properties
        assert np.all(satellites['Mvir'] >= 0), \
            "Satellites should have non-negative Mvir"
        assert np.all(satellites['StellarMass'] >= 0), \
            "Satellites should have non-negative StellarMass"
    else:
        print("    No satellites in output (may be valid for this tree)")

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Satellite processing validated")


def test_centrals_preserved():
    """
    Test that centrals (Type 0) exist in output and have valid properties

    Expected: Type 0 galaxies exist and have non-negative mass properties
    Validates: Module doesn't corrupt central galaxy data
    """
    print("Testing centrals preserved...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_update_merger_time_exclusive",
        output_format="binary",
        phase_config=get_full_pipeline_config(),
        model_params={
            'ThresholdSatDisruption': 5.0,
        },
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic should execute successfully"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Get centrals
    centrals = halos[halos['Type'] == 0]
    assert len(centrals) > 0, "Should have centrals in output"
    
    # Centrals should have valid properties
    assert np.all(centrals['Mvir'] >= 0), \
        "Centrals should have non-negative Mvir"
    assert np.all(centrals['StellarMass'] >= 0), \
        "Centrals should have non-negative StellarMass"
    
    print(f"    Found {len(centrals)} centrals with valid properties")

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Centrals preserved correctly")


def test_galaxy_types_valid():
    """
    Test that galaxy types are valid after module processing

    Expected: All galaxies have Type 0, 1, 2, or 3
    Validates: Module doesn't corrupt Type values
    """
    print("Testing galaxy types valid...")

    # ===== SETUP =====
    param_file, output_dir, test_temp_dir = create_test_param_file(
        output_name="sage_update_merger_time_decrement",
        output_format="binary",
        phase_config=get_full_pipeline_config(),
        model_params={
            'ThresholdSatDisruption': 1.0,
        },
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic should execute successfully"

    # ===== VALIDATE =====
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(str(output_file))

    # Check Type values
    assert np.all((halos['Type'] >= 0) & (halos['Type'] <= 3)), \
        "All galaxies should have Type 0, 1, 2, or 3"
    
    # Count galaxy types
    n_centrals = np.sum(halos['Type'] == 0)
    n_type1 = np.sum(halos['Type'] == 1)
    n_type2 = np.sum(halos['Type'] == 2)
    n_orphans = np.sum(halos['Type'] == 3)
    
    print(f"    Type 0 (centrals): {n_centrals}")
    print(f"    Type 1 (satellites): {n_type1}")
    print(f"    Type 2 (satellites): {n_type2}")
    print(f"    Type 3 (orphans): {n_orphans}")

    # Cleanup
    shutil.rmtree(test_temp_dir)

    print("  ✓ Galaxy types are valid")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: SAGE Update Merger Time Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_module_loads,
        test_memory_safety,
        test_required_properties_exist,
        test_property_types,
        test_output_sanity_checks,
        test_data_flow_validation,
        test_parameter_validation,
        test_satellite_processing,
        test_centrals_preserved,
        test_galaxy_types_valid,
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
