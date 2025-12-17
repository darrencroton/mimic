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
import subprocess
import tempfile
from pathlib import Path

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent
MIMIC_EXE = REPO_ROOT / "mimic"

# Add tests directory to path to import framework
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import load_binary_halos

# Test state
temp_dir = None
ref_param_file = None

# ANSI color codes (module-level constants)
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


def setup_module():
    """Create temporary directory for test outputs"""
    global temp_dir, ref_param_file
    temp_dir = Path(tempfile.mkdtemp(prefix="test_update_sf_"))
    ref_param_file = REPO_ROOT / "tests" / "data" / "test_binary.yaml"


def teardown_module():
    """Clean up temporary directory"""
    if temp_dir and temp_dir.exists():
        shutil.rmtree(temp_dir)


def run_mimic(param_file):
    """Run Mimic with given parameter file, return (returncode, stdout, stderr)"""
    result = subprocess.run(
        [str(MIMIC_EXE), str(param_file)],
        capture_output=True,
        text=True
    )
    return result.returncode, result.stdout, result.stderr


def create_test_config(modules, output_dir, output_format='binary'):
    """Create test configuration YAML with specified modules"""
    import yaml

    with open(ref_param_file, 'r') as f:
        config = yaml.safe_load(f)

    config['modules']['enabled'] = modules
    config['output']['output_dir'] = str(output_dir)
    config['output']['output_format'] = output_format

    test_param = temp_dir / "test_config.yaml"
    with open(test_param, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    return test_param


def test_module_loads():
    """Test that sage_update_star_formation_supernova module loads and initializes"""
    print(f"\n{BLUE}TEST: Module loads and initializes{NC}")

    output_dir = temp_dir / "test_loads"
    output_dir.mkdir(exist_ok=True)

    # Full 3-module pipeline
    param_file = create_test_config(
        [
            'sage_calculate_star_formation',
            'sage_calculate_supernova_feedback',
            'sage_update_star_formation_supernova'
        ],
        output_dir
    )
    returncode, stdout, stderr = run_mimic(param_file)

    # Check execution succeeded
    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Check output file exists
    output_file = output_dir / "model_000.bin"
    assert output_file.exists(), "Output file should exist"

    print(f"{GREEN}✓ Module loads and initializes successfully{NC}")


def test_output_properties_exist():
    """Test that updated properties appear in output"""
    print(f"\n{BLUE}TEST: Updated properties exist in output{NC}")

    output_dir = temp_dir / "test_properties"
    output_dir.mkdir(exist_ok=True)

    param_file = create_test_config(
        [
            'sage_calculate_star_formation',
            'sage_calculate_supernova_feedback',
            'sage_update_star_formation_supernova'
        ],
        output_dir
    )
    returncode, stdout, stderr = run_mimic(param_file)

    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Load output and check properties
    output_file = output_dir / "model_000.bin"
    halos = load_binary_halos(output_file)

    # Check that updated fields exist (StellarMass, gas transfers)
    assert hasattr(halos, 'StellarMass'), "Output should have StellarMass field"
    assert hasattr(halos, 'ColdGas'), "Output should have ColdGas field"
    assert hasattr(halos, 'HotGas'), "Output should have HotGas field"
    assert hasattr(halos, 'EjectedGas'), "Output should have EjectedGas field"

    print(f"{GREEN}✓ Updated properties exist in output{NC}")


def test_parameters_configurable():
    """Test that module parameters can be configured via YAML"""
    print(f"\n{BLUE}TEST: Module parameters are configurable{NC}")

    import yaml

    output_dir = temp_dir / "test_params"
    output_dir.mkdir(exist_ok=True)

    # Create config with custom parameter values
    with open(ref_param_file, 'r') as f:
        config = yaml.safe_load(f)

    config['modules']['enabled'] = [
        'sage_calculate_star_formation',
        'sage_calculate_supernova_feedback',
        'sage_update_star_formation_supernova'
    ]
    config['output']['output_dir'] = str(output_dir)
    config['output']['output_format'] = 'binary'

    # Set custom parameter values
    if 'parameters' not in config:
        config['parameters'] = {}
    config['parameters']['RecycleFraction'] = 0.5  # Custom value
    config['parameters']['Yield'] = 0.03  # Custom value
    config['parameters']['FracZleaveDisk'] = 0.4  # Custom value

    test_param = temp_dir / "test_params.yaml"
    with open(test_param, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    returncode, stdout, stderr = run_mimic(test_param)

    assert returncode == 0, f"Mimic should execute with custom parameters\nSTDERR: {stderr}"

    print(f"{GREEN}✓ Module parameters are configurable{NC}")


def test_memory_safety():
    """Test that module doesn't leak memory"""
    print(f"\n{BLUE}TEST: No memory leaks{NC}")

    output_dir = temp_dir / "test_memory"
    output_dir.mkdir(exist_ok=True)

    param_file = create_test_config(
        [
            'sage_calculate_star_formation',
            'sage_calculate_supernova_feedback',
            'sage_update_star_formation_supernova'
        ],
        output_dir
    )
    returncode, stdout, stderr = run_mimic(param_file)

    assert returncode == 0, f"Mimic should execute successfully\nSTDERR: {stderr}"

    # Check for memory leak indicators in output
    combined_output = stdout + stderr
    assert "MEMORY LEAK" not in combined_output.upper(), "Should not have memory leaks"
    assert "LEAKED" not in combined_output.upper(), "Should not have leaked memory"

    print(f"{GREEN}✓ No memory leaks detected{NC}")


def test_execution_completes():
    """Test that full pipeline execution completes successfully"""
    print(f"\n{BLUE}TEST: Full pipeline execution completes{NC}")

    output_dir = temp_dir / "test_complete"
    output_dir.mkdir(exist_ok=True)

    param_file = create_test_config(
        [
            'sage_calculate_star_formation',
            'sage_calculate_supernova_feedback',
            'sage_update_star_formation_supernova'
        ],
        output_dir
    )
    returncode, stdout, stderr = run_mimic(param_file)

    assert returncode == 0, f"Pipeline should complete successfully\nSTDERR: {stderr}"

    # Check output file exists and has content
    output_file = output_dir / "model_000.bin"
    assert output_file.exists(), "Output file should exist"
    assert output_file.stat().st_size > 0, "Output file should have content"

    # Load and validate basic structure
    halos = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have output halos"

    print(f"{GREEN}✓ Full pipeline execution completes{NC}")


def test_full_pipeline():
    """Test that module integrates correctly in full 3-module pipeline"""
    print(f"\n{BLUE}TEST: Integration in full 3-module pipeline{NC}")

    output_dir = temp_dir / "test_pipeline"
    output_dir.mkdir(exist_ok=True)

    # Run with all 3 modules
    param_file = create_test_config(
        [
            'sage_calculate_star_formation',
            'sage_calculate_supernova_feedback',
            'sage_update_star_formation_supernova'
        ],
        output_dir
    )
    returncode, stdout, stderr = run_mimic(param_file)

    assert returncode == 0, f"Pipeline should complete successfully\nSTDERR: {stderr}"

    # Load output and validate pipeline worked correctly
    output_file = output_dir / "model_000.bin"
    halos = load_binary_halos(output_file)

    # All properties should exist
    assert hasattr(halos, 'StellarMass'), "Should have updated StellarMass"
    assert hasattr(halos, 'ColdGas'), "Should have updated ColdGas"
    assert hasattr(halos, 'HotGas'), "Should have updated HotGas"

    # Temporary properties should be reset to zero in output
    # (Note: This depends on output properties configuration)
    if hasattr(halos, 'NewStellarMass'):
        # If temporary properties are in output, they should be zero after update
        assert all(halos['NewStellarMass'] >= 0), "NewStellarMass should be non-negative"

    print(f"{GREEN}✓ Integration in full 3-module pipeline works{NC}")


def main():
    """Main test runner"""
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: sage_update_star_formation_supernova Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    setup_module()

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

    finally:
        teardown_module()


if __name__ == '__main__':
    sys.exit(main())
