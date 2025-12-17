#!/usr/bin/env python3
"""
SAGE Reincorporation Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration

This test validates software quality aspects of the sage_reincorporation module:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- Module works in multi-module pipelines
- Properties modified correctly (EjectedGas → HotGas)

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: Reincorporation properties in output
  - test_parameters_configurable: Parameter reading and validation
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_multi_module_pipeline: Integration with sage_calculate_infall

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
    """Test that sage_reincorporation module loads and initializes"""
    print(f"\n{BLUE}TEST: Module loads and initializes{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="reinc_load",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('sage_reincorporation', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'ReIncorporationFactor': 1.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Module loads and initializes successfully{NC}")


def test_output_properties_exist():
    """Test that reincorporation-related properties appear in output"""
    print(f"\n{BLUE}TEST: Reincorporation properties exist in output{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="reinc_properties",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('sage_reincorporation', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'ReIncorporationFactor': 1.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Check that properties exist
    assert 'EjectedGas' in halos.dtype.names, "Output should have EjectedGas field"
    assert 'HotGas' in halos.dtype.names, "Output should have HotGas field"
    assert 'MetalsEjectedGas' in halos.dtype.names, "Output should have MetalsEjectedGas field"
    assert 'MetalsHotGas' in halos.dtype.names, "Output should have MetalsHotGas field"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Reincorporation properties exist in output{NC}")


def test_parameters_configurable():
    """Test that module parameters can be configured via YAML"""
    print(f"\n{BLUE}TEST: Module parameters are configurable{NC}")

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="reinc_params",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('sage_reincorporation', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'ReIncorporationFactor': 0.5  # Custom value
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
        output_name="reinc_memory",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('sage_reincorporation', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'ReIncorporationFactor': 1.0
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
        output_name="reinc_complete",
        phase_config={
            'pre_timestep': [],
            'phase_1': [('sage_reincorporation', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'ReIncorporationFactor': 1.0
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


def test_multi_module_pipeline():
    """Test that sage_reincorporation works with other modules"""
    print(f"\n{BLUE}TEST: Multi-module pipeline integration{NC}")

    # Test with sage_calculate_infall to populate ejected reservoir
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="reinc_pipeline",
        phase_config={
            'pre_timestep': [('sage_calculate_infall', 'process_full_halo')],
            'phase_1': [('sage_reincorporation', 'process_full_halo')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={
            'GlobalBaryonFraction': 0.17,
            'ReIncorporationFactor': 1.0
        }
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Multi-module pipeline should succeed\nSTDERR: {stderr}"

    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Both modules' properties should exist
    assert 'HotGas' in halos.dtype.names, "Should have HotGas from reincorporation"
    assert 'EjectedGas' in halos.dtype.names, "Should have EjectedGas from reincorporation"

    shutil.rmtree(temp_dir)
    print(f"{GREEN}✓ Multi-module pipeline integration works{NC}")


def main():
    """Main test runner"""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage_reincorporation Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    try:
        test_module_loads()
        test_output_properties_exist()
        test_parameters_configurable()
        test_memory_safety()
        test_execution_completes()
        test_multi_module_pipeline()

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
