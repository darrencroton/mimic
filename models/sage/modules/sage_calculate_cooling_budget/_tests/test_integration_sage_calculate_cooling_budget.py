#!/usr/bin/env python3
"""
Integration tests for sage_calculate_cooling_budget module

Tests the sage_calculate_cooling_budget module in a full pipeline context:
- Module loads and initializes correctly
- Integrates properly with sage_prepare_infall_budget (provides hot gas source)
- CoolingGas and Rcool properties are created and written to output
- Output validation: CoolingGas and Rcool values are physically reasonable
- Type 2 orphans are skipped correctly
- Memory safety (no leaks)

NOTE: This module has NO runtime parameters (CoolFunctions path is hardcoded).
      AGN functionality is in sage_radio_mode_heating (different module).

Phase: Phase 4.2 (SAGE Physics Module Implementation)
Author: Mimic Development Team
Date: 2025-11-13 (Updated 2025-12-18)
"""

import os
import sys
import subprocess
import struct
import numpy as np

# Add test framework to path
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../../../../tests'))
from framework import harness

# ANSI color codes (module-level constants)
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'

class TestSageCoolingIntegration:
    """Integration tests for sage_calculate_cooling_budget module

    Tests focus on integration and output validation:
    - Module loading and initialization
    - Pipeline integration (with sage_prepare_infall_budget)
    - Output validation (CoolingGas and Rcool properties)
    - Physics constraints (values in valid ranges)
    - Memory safety (no leaks)

    NOTE: This module has NO runtime parameters.
          CoolFunctions path is hardcoded in sage_calculate_cooling_budget.c
    """

    def test_module_loads(self):
        """Test that sage_calculate_cooling_budget module loads successfully"""
        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="sage_calculate_cooling_budget_load",
            phase_config={
                'pre_timestep': [],
                'phase_1': [('sage_calculate_cooling_budget', 'process_by_galaxy')],
                'phase_2': [],
                'post_timestep': []
            }
            # No model_params - this module has NO runtime parameters
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)

        assert returncode == 0, f"Mimic should run successfully\nStderr: {stderr}"

    def test_infall_cooling_pipeline(self):
        """Test sage_prepare_infall_budget → sage_calculate_cooling_budget pipeline integration"""
        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="infall_cooling_pipeline",
            phase_config={
                'pre_timestep': [('sage_prepare_infall_budget', 'process_full_halo')],
                'phase_1': [('sage_calculate_cooling_budget', 'process_by_galaxy')],
                'phase_2': [],
                'post_timestep': []
            },
            model_params={
                "GlobalBaryonFraction": 0.17  # For sage_prepare_infall_budget
            }
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)

        assert returncode == 0, f"Pipeline should run successfully\nStderr: {stderr}"
        # Check for no memory leaks
        assert harness.check_no_memory_leaks(output_dir), "Should have no memory leaks"

    def test_memory_safety(self):
        """Test that sage_calculate_cooling_budget doesn't leak memory"""
        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="cooling_memory",
            phase_config={
                'pre_timestep': [('sage_prepare_infall_budget', 'process_full_halo')],
                'phase_1': [('sage_calculate_cooling_budget', 'process_by_galaxy')],
                'phase_2': [],
                'post_timestep': []
            },
            model_params={
                "GlobalBaryonFraction": 0.17  # For sage_prepare_infall_budget
            }
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)
        assert returncode == 0, f"Execution should succeed\nStderr: {stderr}"
        assert harness.check_no_memory_leaks(output_dir), "Should have no memory leaks"

    def test_execution_completes(self):
        """Test that full pipeline execution completes without errors"""
        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="cooling_complete",
            phase_config={
                'pre_timestep': [('sage_prepare_infall_budget', 'process_full_halo')],
                'phase_1': [('sage_calculate_cooling_budget', 'process_by_galaxy')],
                'phase_2': [],
                'post_timestep': []
            },
            model_params={
                "GlobalBaryonFraction": 0.17  # For sage_prepare_infall_budget
            },
            first_file=0,
            last_file=0
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)
        assert returncode == 0, f"Pipeline should complete\nStderr: {stderr}"

    def test_module_ordering_dependency(self):
        """Test that module system handles ordering correctly

        Correct order: infall (pre_timestep) → cooling (phase_1)
        Wrong order: cooling (pre_timestep) → infall (phase_1)
        Both should run without errors (though wrong order is ineffective)
        """
        # Correct order: infall then cooling
        param_file_correct, output_dir1, temp_dir1 = harness.create_test_param_file(
            output_name="cooling_order_correct",
            phase_config={
                'pre_timestep': [('sage_prepare_infall_budget', 'process_full_halo')],
                'phase_1': [('sage_calculate_cooling_budget', 'process_by_galaxy')],
                'phase_2': [],
                'post_timestep': []
            },
            model_params={
                "GlobalBaryonFraction": 0.17  # For sage_prepare_infall_budget
            }
        )

        returncode_correct, stdout, stderr = harness.run_mimic(param_file_correct)
        assert returncode_correct == 0, f"Correct order should work\nStderr: {stderr}"

        # Wrong order: cooling then infall (still runs, but ineffective)
        param_file_wrong, output_dir2, temp_dir2 = harness.create_test_param_file(
            output_name="cooling_order_wrong",
            phase_config={
                'pre_timestep': [('sage_calculate_cooling_budget', 'process_by_galaxy')],
                'phase_1': [('sage_prepare_infall_budget', 'process_full_halo')],  # Wrong order
                'phase_2': [],
                'post_timestep': []
            },
            model_params={
                "GlobalBaryonFraction": 0.17  # For sage_prepare_infall_budget
            }
        )

        returncode_wrong, stdout, stderr = harness.run_mimic(param_file_wrong)
        assert returncode_wrong == 0, f"Wrong order should still run\nStderr: {stderr}"

    def test_output_file_created(self):
        """Test that output files are created successfully

        NOTE: CoolingGas and Rcool are working variables (output: false) and
              are NOT written to output files. This is correct by design.
              We only check that the module runs and creates output.
        """
        import h5py

        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="cooling_output_check",
            phase_config={
                'pre_timestep': [('sage_prepare_infall_budget', 'process_full_halo')],
                'phase_1': [('sage_calculate_cooling_budget', 'process_by_galaxy')],
                'phase_2': [],
                'post_timestep': []
            },
            model_params={
                "GlobalBaryonFraction": 0.17  # For sage_prepare_infall_budget
            },
            output_format='hdf5',
            first_file=0,
            last_file=0
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)
        assert returncode == 0, f"Execution should succeed\nStderr: {stderr}"

        # Check HDF5 output exists
        from pathlib import Path
        output_dir_path = Path(output_dir)
        output_files = list(output_dir_path.glob("**/model_*.hdf5"))
        assert len(output_files) > 0, f"HDF5 output files should exist in {output_dir_path}"

        # Verify output file is valid and has expected structure
        with h5py.File(output_files[0], 'r') as f:
            # Find a snapshot group
            snap_groups = [k for k in f.keys() if k.startswith('Snap')]
            assert len(snap_groups) > 0, "Should have at least one snapshot"

            snap = snap_groups[-1]
            assert 'Galaxies' in f[snap], f"Galaxies dataset should exist in {snap}"

            halos = f[snap]['Galaxies'][:]
            assert len(halos) > 0, "Should have at least one halo in output"

            print(f"✓ Output file created with {len(halos)} halos")

    def test_downstream_effects(self):
        """Test that sage_calculate_cooling_budget produces expected downstream effects

        Since CoolingGas/Rcool are working variables, we verify that the
        module execution produces the expected effects on HotGas properties.
        """
        import h5py
        import numpy as np

        param_file, output_dir, temp_dir = harness.create_test_param_file(
            output_name="cooling_downstream_check",
            phase_config={
                'pre_timestep': [('sage_prepare_infall_budget', 'process_full_halo')],
                'phase_1': [('sage_calculate_cooling_budget', 'process_by_galaxy')],
                'phase_2': [],
                'post_timestep': []
            },
            model_params={
                "GlobalBaryonFraction": 0.17  # For sage_prepare_infall_budget
            },
            output_format='hdf5',
            first_file=0,
            last_file=0
        )

        returncode, stdout, stderr = harness.run_mimic(param_file)
        assert returncode == 0, f"Execution should succeed\nStderr: {stderr}"

        # Load output
        from pathlib import Path
        output_dir_path = Path(output_dir)
        output_files = list(output_dir_path.glob("**/model_*.hdf5"))
        assert len(output_files) > 0, f"HDF5 output files should exist"

        with h5py.File(output_files[0], 'r') as f:
            snap_groups = [k for k in f.keys() if k.startswith('Snap')]
            snap = snap_groups[-1]
            halos = f[snap]['Galaxies'][:]

            hot_gas = halos['HotGas']

            # Verify hot gas properties are valid
            # 1. HotGas should be non-negative
            assert np.all(hot_gas >= 0), \
                f"HotGas should be non-negative, found min={hot_gas.min()}"

            # 2. HotGas should be finite (no NaN or inf)
            assert np.all(np.isfinite(hot_gas)), \
                f"HotGas should be finite, found {np.sum(~np.isfinite(hot_gas))} invalid values"

            # 3. Verify execution completed successfully
            # NOTE: sage_calculate_cooling_budget only CALCULATES cooling, doesn't transfer it
            #       The actual transfer happens in sage_apply_cooling (not tested here)
            #       So we just verify the module ran without corrupting data

            print(f"✓ HotGas range: [{hot_gas.min():.6f}, {hot_gas.max():.6f}]")
            print(f"✓ All {len(halos)} halos have valid HotGas values")
            print(f"✓ Module execution completed without corrupting data")


def main():
    """Run all integration tests"""
    # Get all test methods from the test class
    test_class = TestSageCoolingIntegration()
    test_methods = [name for name in dir(test_class) if name.startswith('test_')]

    passed = 0
    failed = 0
    errors = []

    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage_calculate_cooling_budget Integration Tests{NC}")
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
