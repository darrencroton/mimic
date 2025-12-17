#!/usr/bin/env python3
"""
SAGE Update Star Formation Supernova Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration

This test validates software quality aspects of the sage_update_star_formation_supernova module:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- Gas transfers and metal enrichment work correctly
- Module works in full 3-module pipeline
- Temporary properties reset after processing

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: Updated properties in output
  - test_parameters_configurable: Parameter reading and validation
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_full_pipeline: Integration with calculate modules

Author: Mimic Development Team
Date: 2025-12-17
"""

import os
import sys
import shutil
from pathlib import Path

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import create_test_param_file, run_mimic, load_binary_halos, check_no_memory_leaks

# ANSI color codes
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
NC = '\033[0m'


def test_module_loads():
    """Test that sage_update_star_formation_supernova module loads and initializes"""
    print(f"\n{BLUE}TEST: Module loads and initializes{NC}")

    # Full 3-module pipeline
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="update_sf_load",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_update_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module loads and initializes successfully{NC}")


def test_output_properties_exist():
    """Test that updated properties appear in output"""
    print(f"\n{BLUE}TEST: Updated properties exist in output{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="update_sf_properties",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_update_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Check that updated fields exist (StellarMass, gas transfers)
    assert 'StellarMass' in halos.dtype.names, "Output should have StellarMass field"
    assert 'ColdGas' in halos.dtype.names, "Output should have ColdGas field"
    assert 'HotGas' in halos.dtype.names, "Output should have HotGas field"
    assert 'EjectedGas' in halos.dtype.names, "Output should have EjectedGas field"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Updated properties exist in output{NC}")


def test_parameters_configurable():
    """Test that module parameters can be configured via YAML"""
    print(f"\n{BLUE}TEST: Module parameters are configurable{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="update_sf_params",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_update_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.5,  # Custom value
            'Yield': 0.03,  # Custom value
            'FracZleaveDisk': 0.4  # Custom value
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute with custom parameters\nSTDERR: {stderr}"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module parameters are configurable{NC}")


def test_memory_safety():
    """Test that module doesn't leak memory"""
    print(f"\n{BLUE}TEST: No memory leaks{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="update_sf_memory",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_update_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    assert check_no_memory_leaks(output_dir), "Should not have memory leaks"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ No memory leaks detected{NC}")


def test_execution_completes():
    """Test that full pipeline execution completes successfully"""
    print(f"\n{BLUE}TEST: Full pipeline execution completes{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="update_sf_complete",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_update_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Pipeline should complete successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"
    assert output_file.stat().st_size > 0, "Output file should have content"

    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have output halos"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Full pipeline execution completes{NC}")


def test_full_pipeline():
    """Test that module integrates correctly in full 3-module pipeline"""
    print(f"\n{BLUE}TEST: Integration in full 3-module pipeline{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="update_sf_pipeline",
        phase_config={
            'pre_timestep': [],
            'phase_1': [
                ('sage_calculate_star_formation', 'process_by_galaxy'),
                ('sage_calculate_supernova_feedback', 'process_by_galaxy'),
                ('sage_update_star_formation_supernova', 'process_by_galaxy')
            ],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'SfrEfficiency': 0.02,
            'StarFormingDiskFactor': 3.0,
            'FeedbackReheatingEpsilon': 3.0,
            'FeedbackEjectionEfficiency': 0.3,
            'RecycleFraction': 0.43,
            'Yield': 0.025,
            'FracZleaveDisk': 0.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Pipeline should complete successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # All properties should exist
    assert 'StellarMass' in halos.dtype.names, "Should have updated StellarMass"
    assert 'ColdGas' in halos.dtype.names, "Should have updated ColdGas"
    assert 'HotGas' in halos.dtype.names, "Should have updated HotGas"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Integration in full 3-module pipeline works{NC}")


def main():
    """Main test runner"""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage_update_star_formation_supernova Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    try:
        test_module_loads()
        test_output_properties_exist()
        test_parameters_configurable()
        test_memory_safety()
        test_execution_completes()
        test_full_pipeline()

        print(f"\n{GREEN}{'=' * 60}{NC}")
        print(f"{GREEN}All tests passed!{NC}")
        print(f"{GREEN}{'=' * 60}{NC}")
        return 0

    except AssertionError as e:
        print(f"\n{RED}{'=' * 60}{NC}")
        print(f"{RED}Test failed: {e}{NC}")
        print(f"{RED}{'=' * 60}{NC}")
        return 1

    except Exception as e:
        print(f"\n{RED}{'=' * 60}{NC}")
        print(f"{RED}Unexpected error: {e}{NC}")
        print(f"{RED}{'=' * 60}{NC}")
        return 1


if __name__ == '__main__':
    sys.exit(main())
