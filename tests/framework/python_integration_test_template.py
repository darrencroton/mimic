#!/usr/bin/env python3
"""
[Module Name] Module - Integration Test

Validates: End-to-end physics correctness, conservation laws, parameter sensitivity

This test validates the [module_name] module integration:
- [Primary integration test 1]
- [Primary integration test 2]
- Conservation laws (if applicable)
- Parameter sensitivity
- Edge cases
- Memory safety and performance

Test cases:
  - test_full_pipeline_execution: Module executes in pipeline
  - test_[physics_validation]: Physics correctness validation
  - test_conservation_laws: Conservation validation (if applicable)
  - test_parameter_sensitivity: Parameters affect results
  - test_edge_cases: Edge cases and boundary conditions
  - test_memory_and_performance: Memory leaks and performance

Author: Mimic Development Team
Date: [DATE]
"""

import os
import shutil
import sys
from pathlib import Path

import numpy as np

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import (
    TestSkipped,
    check_no_memory_leaks,
    create_test_param_file,
    load_binary_halos,
    result_error,
    result_fail,
    result_pass,
    result_skip,
    run_mimic,
)

# ANSI color codes
BLUE = "\033[1;34m"
GREEN = "\033[0;32m"
RED = "\033[0;31m"
NC = "\033[0m"


def test_full_pipeline_execution():
    """Test module executes successfully in full pipeline

    Validates:
    - Module loads and initializes
    - Pipeline executes without errors
    - Output files created
    - Basic data validity (non-negative, finite values)
    """
    print(f"\n{BLUE}TEST: Full pipeline execution{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="[module_name]_pipeline",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("[module_name]", "process_by_galaxy"),
                # Add dependencies if needed
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "Param1Name": 1.0,
            "Param2Name": 0.5,
            # Add all required module parameters
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Basic validity checks
    assert len(halos) > 0, "Should have halos in output"

    # Check key properties are valid
    # for prop in ['Mvir', 'Vvir', ...]:
    #     assert prop in halos.dtype.names, f"Property {prop} should exist"
    #     assert np.all(np.isfinite(halos[prop])), f"{prop} should be finite"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Full pipeline execution validated{NC}")


def test_physics_validation():
    """Test physics calculation correctness

    Validates:
    - [Specific physics calculation 1]
    - [Specific physics calculation 2]
    - Physical reasonableness of results
    - Expected property relationships
    """
    print(f"\n{BLUE}TEST: Physics validation{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="[module_name]_physics",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("[module_name]", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "Param1Name": 1.0,
            "Param2Name": 0.5,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Validate physics calculations
    # Example: Check that calculated property is reasonable
    # calculated_prop = halos['CalculatedProperty']
    # non_zero = calculated_prop > 1e-10
    # if np.sum(non_zero) > 0:
    #     assert np.all(calculated_prop[non_zero] > 0), "Property should be positive"
    #     assert np.all(calculated_prop[non_zero] < 1e6), "Property should be reasonable"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Physics validation successful{NC}")


def test_conservation_laws():
    """Test conservation laws (if applicable)

    Validates:
    - [Conservation law 1 - e.g., mass conservation]
    - [Conservation law 2 - e.g., metal conservation]
    - Total conserved quantities unchanged or change by expected amount
    """
    print(f"\n{BLUE}TEST: Conservation laws{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="[module_name]_conservation",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("[module_name]", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "Param1Name": 1.0,
            "Param2Name": 0.5,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Validate conservation
    # Example: Total mass should be conserved
    # total_mass = halos['Component1'] + halos['Component2'] + halos['Component3']
    # assert np.all(total_mass >= 0), "Total mass should be non-negative"
    # assert np.all(np.isfinite(total_mass)), "Total mass should be finite"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Conservation laws validated{NC}")


def test_parameter_sensitivity():
    """Test that parameters affect results correctly

    Validates:
    - Param1 affects results as expected
    - Param2 affects results as expected
    - Different parameter values produce different results
    """
    print(f"\n{BLUE}TEST: Parameter sensitivity{NC}")

    # Run 1: Default parameters
    param_file1, output_dir1, temp_dir1 = create_test_param_file(
        output_name="[module_name]_params_default",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("[module_name]", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "Param1Name": 1.0,
            "Param2Name": 0.5,
        },
    )

    returncode1, stdout1, stderr1 = run_mimic(param_file1)
    assert returncode1 == 0, f"Run 1 should execute successfully\nSTDERR: {stderr1}"

    output_file1 = output_dir1 / "model_z0.000_0"
    halos1, metadata1 = load_binary_halos(output_file1)

    # Run 2: Different parameters
    param_file2, output_dir2, temp_dir2 = create_test_param_file(
        output_name="[module_name]_params_modified",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("[module_name]", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "Param1Name": 2.0,  # Changed value
            "Param2Name": 0.5,
        },
    )

    returncode2, stdout2, stderr2 = run_mimic(param_file2)
    assert returncode2 == 0, f"Run 2 should execute successfully\nSTDERR: {stderr2}"

    output_file2 = output_dir2 / "model_z0.000_0"
    halos2, metadata2 = load_binary_halos(output_file2)

    # Compare results
    # Example: Check that changing Param1 affects output
    # output1 = np.sum(halos1['OutputProperty'])
    # output2 = np.sum(halos2['OutputProperty'])
    # if output1 > 0 and output2 > 0:
    #     assert output1 != output2, "Different parameters should produce different results"

    shutil.rmtree(temp_dir1)
    shutil.rmtree(temp_dir2)
    print(f"{GREEN}✓ Parameter sensitivity validated{NC}")


def test_edge_cases():
    """Test edge cases and boundary conditions

    Validates:
    - Zero input values handled correctly
    - Boundary conditions respected
    - No NaNs or Infs in output
    - All values within expected ranges
    """
    print(f"\n{BLUE}TEST: Edge cases{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="[module_name]_edge_cases",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("[module_name]", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "Param1Name": 1.0,
            "Param2Name": 0.5,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Check for NaNs and Infs in key properties
    # for field in ['Property1', 'Property2', ...]:
    #     assert not np.any(np.isnan(halos[field])), f"{field} should not have NaN values"
    #     assert not np.any(np.isinf(halos[field])), f"{field} should not have Inf values"

    # Check values are non-negative if expected
    # for field in ['NonNegativeProperty1', ...]:
    #     assert np.all(halos[field] >= 0.0), f"{field} should be non-negative"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Edge cases handled correctly{NC}")


def test_memory_and_performance():
    """Test memory safety and performance baseline

    Validates:
    - No memory leaks
    - Execution completes successfully
    - Output file size reasonable
    - Performance acceptable
    """
    print(f"\n{BLUE}TEST: Memory and performance{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="[module_name]_memory",
        phase_config={
            "pre_timestep": [],
            "galaxy_physics": [
                ("[module_name]", "process_by_galaxy"),
            ],
            "satellite_mergers": [],
            "post_timestep": [],
        },
        model_params={
            "Param1Name": 1.0,
            "Param2Name": 0.5,
        },
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Check for memory leaks
    assert check_no_memory_leaks(output_dir), "Should not have memory leaks"

    # Check output file
    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"

    file_size = output_file.stat().st_size
    print(f"  Output file size: {file_size} bytes")
    assert file_size > 0, "Output file should have content"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Memory and performance validated{NC}")


def main():
    """Main test runner"""
    print(f"{BLUE}{'=' * 70}{NC}")
    print(f"{BLUE}Test Suite: [module_name] Integration Tests{NC}")
    print(f"{BLUE}{'=' * 70}{NC}")

    tests = [
        test_full_pipeline_execution,
        test_physics_validation,
        test_conservation_laws,
        test_parameter_sensitivity,
        test_edge_cases,
        test_memory_and_performance,
    ]

    passed = 0
    failed = 0
    skipped = 0

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
    print(f"{BLUE}{'=' * 70}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 70}{NC}")
    print(f"Passed:  {passed}")
    if skipped:
        print(f"Skipped: {skipped}")
    print(f"Failed:  {failed}")
    print(f"Total:   {passed + failed + skipped}")
    print(f"{BLUE}{'=' * 70}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}All integration tests passed!{NC}")
        return 0
    else:
        print(f"{RED}{failed} test(s) failed{NC}")
        return 1


if __name__ == "__main__":
    sys.exit(main())


"""
TEMPLATE USAGE INSTRUCTIONS:
============================

1. Copy this template to models/<model>/modules/<module_name>/_tests/test_integration_[module_name].py or models/<model>/modules/_tests/test_integration_[contract_name].py

2. Replace all [module_name] placeholders with your actual module name

3. Update the file header:
   - Fill in what the module validates
   - List the key integration tests
   - List all test cases

4. Implement test functions:
   - test_full_pipeline_execution: Basic pipeline test
   - test_physics_validation: Validate physics calculations
   - test_conservation_laws: Check conservation (if applicable)
   - test_parameter_sensitivity: Test parameter effects
   - test_edge_cases: Test boundary conditions
   - test_memory_and_performance: Memory and performance checks

5. Configure phase_config and model_params:
   - Set up correct pipeline phase for your module
   - Include all required module parameters
   - Add dependencies if your module requires other modules

6. Build and run:
   - make clean && make tests-integration
   - All tests should PASS
   - No memory leaks

KEY PRINCIPLES:
==============
- Python integration tests validate END-TO-END behavior
- Test full pipeline execution (not isolated functions)
- Validate physics correctness at system level
- Test conservation laws across pipeline
- Test parameter sensitivity
- Check memory safety
- Keep tests fast (<30 seconds per test)

INTEGRATION TESTING PATTERN:
===========================
For each module:
1. Test pipeline execution (loads, runs, completes)
2. Test physics validation (results are correct)
3. Test conservation (if applicable)
4. Test parameter sensitivity (params affect results)
5. Test edge cases (boundary conditions)
6. Test memory safety (no leaks)

EXAMPLE: Testing star formation module
- test_full_pipeline_execution: Module runs in pipeline
- test_physics_validation: Star formation rates are reasonable
- test_conservation_laws: Mass conserved during SF
- test_parameter_sensitivity: SFR efficiency affects results
- test_edge_cases: Zero cold gas handled correctly
- test_memory_and_performance: No leaks, acceptable speed

WHAT TO VALIDATE:
================
- Physics correctness (realistic values, expected relationships)
- Conservation laws (mass, metals, energy if applicable)
- Parameter effects (changing params changes results correctly)
- Edge cases (zero values, boundary conditions, type variations)
- Memory safety (no leaks)
- Performance (execution time acceptable)

WHAT NOT TO TEST:
================
- Internal calculations (that's for C unit tests)
- Detailed math validation (that's for C unit tests)
- Mock scenarios (use real pipeline execution)
- Individual function behavior (test system behavior)

DATA VALIDATION TIPS:
====================
- Check for NaN and Inf values
- Check value ranges are physical
- Check conservation laws
- Compare with expected scaling relations
- Validate across different galaxy types (centrals, satellites)
- Test with minimal dataset (fast tests)
"""
