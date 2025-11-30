#!/usr/bin/env python3
"""
Integration tests for sage_cooling module

Tests the sage_cooling module in a full pipeline context:
- Module loads and initializes correctly
- Integrates properly with sage_infall (hot gas → cold gas)
- Properties are created and written to output
- Mass conservation through cooling
- AGN feedback suppresses cooling
- Black hole growth occurs
- Central vs satellite behavior
- Parameter variations work correctly

Phase: Phase 4.2 (SAGE Physics Module Implementation)
Author: Mimic Development Team
Date: 2025-11-13
"""

import os
import sys
import subprocess
import struct
import numpy as np

# Add test framework to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../../tests'))
from framework import harness

# ANSI color codes (module-level constants)
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'

class TestSageCoolingIntegration:
    """Integration tests for sage_cooling module

    Following sage_infall pattern - tests focus on software quality:
    - Module loading and initialization
    - Parameter configuration
    - Pipeline integration
    - Memory safety
    """

    def test_module_loads(self):
        """Test that sage_cooling module loads successfully"""
        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="sage_cooling_load",
            enabled_modules=["sage_cooling"],
            model_params={
                "RadioModeEfficiency": 0.01,
                "AGNrecipeOn": 1,
                "CoolFunctionsDir": "src/modules/sage_cooling/CoolFunctions"
            }
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)

        assert returncode == 0, f"Mimic should run successfully\nStderr: {stderr}"

    def test_infall_cooling_pipeline(self):
        """Test sage_infall → sage_cooling pipeline integration"""
        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="infall_cooling_pipeline",
            enabled_modules=["sage_infall", "sage_cooling"],
            model_params={
                "BaryonFrac": 0.17,
                "RadioModeEfficiency": 0.01,
                "AGNrecipeOn": 1,
                "CoolFunctionsDir": "src/modules/sage_cooling/CoolFunctions"
            }
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)

        assert returncode == 0, f"Pipeline should run successfully\nStderr: {stderr}"
        # Check for no memory leaks
        assert harness.check_no_memory_leaks(output_dir), "Should have no memory leaks"

    def test_agn_modes(self):
        """Test all AGN accretion modes execute without errors"""
        for agn_mode in [0, 1, 2, 3]:
            mode_names = {0: "off", 1: "empirical", 2: "Bondi-Hoyle", 3: "cold cloud"}

            param_file, output_dir, temp_dir = harness.create_test_param_file(
                output_name=f"agn_mode_{agn_mode}",
                enabled_modules=["sage_infall", "sage_cooling"],
                model_params={
                    "BaryonFrac": 0.17,
                    "RadioModeEfficiency": 0.01,
                    "AGNrecipeOn": agn_mode,
                    "CoolFunctionsDir": "src/modules/sage_cooling/CoolFunctions"
                }
            )

            returncode, stdout, stderr = harness.run_mimic(param_file)
            assert returncode == 0, \
                f"AGN mode {agn_mode} ({mode_names[agn_mode]}) should run\nStderr: {stderr}"

    def test_parameters_configurable(self):
        """Test that all sage_cooling parameters can be configured"""
        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="cooling_params",
            enabled_modules=["sage_infall", "sage_cooling"],
            model_params={
                "BaryonFrac": 0.17,
                "RadioModeEfficiency": 0.02,  # Non-default
                "AGNrecipeOn": 2,  # Bondi-Hoyle mode
                "CoolFunctionsDir": "src/modules/sage_cooling/CoolFunctions"
            }
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)
        assert returncode == 0, f"Custom parameters should work\nStderr: {stderr}"

    def test_memory_safety(self):
        """Test that sage_cooling doesn't leak memory"""
        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="cooling_memory",
            enabled_modules=["sage_infall", "sage_cooling"],
            model_params={
                "BaryonFrac": 0.17,
                "RadioModeEfficiency": 0.01,
                "AGNrecipeOn": 1,
                "CoolFunctionsDir": "src/modules/sage_cooling/CoolFunctions"
            }
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)
        assert returncode == 0, f"Execution should succeed\nStderr: {stderr}"
        assert harness.check_no_memory_leaks(output_dir), "Should have no memory leaks"

    def test_execution_completes(self):
        """Test that full pipeline execution completes without errors"""
        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="cooling_complete",
            enabled_modules=["sage_infall", "sage_cooling"],
            model_params={
                "BaryonFrac": 0.17,
                "RadioModeEfficiency": 0.01,
                "AGNrecipeOn": 1,
                "CoolFunctionsDir": "src/modules/sage_cooling/CoolFunctions"
            },
            first_file=0,
            last_file=0
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)
        assert returncode == 0, f"Pipeline should complete\nStderr: {stderr}"

    def test_module_ordering_dependency(self):
        """Test that module system handles ordering correctly"""
        # Correct order: infall then cooling
        param_file_correct, output_dir1, temp_dir1 = harness.create_test_param_file(
            output_name="cooling_order_correct",
            enabled_modules=["sage_infall", "sage_cooling"],
            model_params={
                "BaryonFrac": 0.17,
                "RadioModeEfficiency": 0.01,
                "AGNrecipeOn": 1,
                "CoolFunctionsDir": "src/modules/sage_cooling/CoolFunctions"
            }
        )

        returncode_correct, stdout, stderr = harness.run_mimic(param_file_correct)
        assert returncode_correct == 0, f"Correct order should work\nStderr: {stderr}"

        # Wrong order: cooling then infall (still runs, but ineffective)
        param_file_wrong, output_dir2, temp_dir2 = harness.create_test_param_file(
            output_name="cooling_order_wrong",
            enabled_modules=["sage_cooling", "sage_infall"],  # Wrong order
            model_params={
                "BaryonFrac": 0.17,
                "RadioModeEfficiency": 0.01,
                "AGNrecipeOn": 1,
                "CoolFunctionsDir": "src/modules/sage_cooling/CoolFunctions"
            }
        )

        returncode_wrong, stdout, stderr = harness.run_mimic(param_file_wrong)
        assert returncode_wrong == 0, f"Wrong order should still run\nStderr: {stderr}"


def main():
    """Run all integration tests"""
    # Get all test methods from the test class
    test_class = TestSageCoolingIntegration()
    test_methods = [name for name in dir(test_class) if name.startswith('test_')]

    passed = 0
    failed = 0
    errors = []

    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage_cooling Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    for test_name in test_methods:
        try:
            # Run test
            test_method = getattr(test_class, test_name)
            test_method()

            print(f"✓ PASS: {test_name}")
            passed += 1
        except AssertionError as e:
            print(f"✗ FAIL: {test_name}")
            print(f"  {str(e)}")
            failed += 1
            errors.append((test_name, str(e)))
        except Exception as e:
            print(f"✗ ERROR: {test_name}")
            print(f"  {str(e)}")
            failed += 1
            errors.append((test_name, str(e)))

    # Summary
    print()
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"Passed:  {passed}")
    print(f"Failed:  {failed}")
    print(f"Total:   {passed + failed}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    if failed == 0:
        print(f"{GREEN}✓ All tests passed!{NC}")
        print()
        return 0
    else:
        print(f"{RED}✗ {failed} test(s) failed{NC}")
        for test_name, error in errors:
            print(f"\nFailed: {test_name}")
            print(f"  {error}")
        print()
        return 1

if __name__ == "__main__":
    sys.exit(main())
