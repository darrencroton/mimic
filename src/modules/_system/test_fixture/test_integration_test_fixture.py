#!/usr/bin/env python3
"""
Test Fixture Module - Integration Test

⚠️ WARNING: These tests validate the test fixture itself ⚠️

The test_fixture module exists solely for testing infrastructure.
These tests validate that the test fixture integrates correctly with
the module system.

Validates:
- Module loads and initializes correctly
- Parameters can be configured
- Module executes in pipeline without errors
- No memory leaks

Test cases:
  - test_module_loads: Module registration and initialization
  - test_parameter_configuration: DummyParameter configuration works
  - test_execution_completes: Module runs to completion
  - test_memory_safety: No memory leaks

Author: Mimic Development Team
Date: 2025-11-13
"""

import os
import sys
import subprocess
import tempfile
from pathlib import Path

# Repository root and paths
# Note: test_fixture is in src/modules/_system/test_fixture/ (4 levels deep)
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
MIMIC_EXE = REPO_ROOT / "mimic"

# Add tests directory to path to import framework
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import load_binary_halos

# Test state
temp_dir = None
ref_param_file = None

# ANSI color codes
RED = '\033[0;31m'
GREEN = '\033[0;32m'
BLUE = '\033[0;34m'
NC = '\033[0m'  # No Color


def run_mimic(param_file):
    """
    Execute Mimic with specified parameter file

    Args:
        param_file (Path): Path to parameter file

    Returns:
        tuple: (returncode, stdout, stderr)
    """
    result = subprocess.run(
        [str(MIMIC_EXE), str(param_file)],
        capture_output=True,
        text=True,
        timeout=60  # 60 second timeout
    )
    return result.returncode, result.stdout, result.stderr


def read_param_file(param_file):
    """Read YAML parameter file and return as dictionary."""
    import yaml

    with open(param_file, 'r') as f:
        config = yaml.safe_load(f)

    # Flatten hierarchical structure
    params = {}
    if 'output' in config:
        params['OutputFileBaseName'] = config['output'].get('file_base_name', 'model')
        params['OutputDir'] = config['output'].get('directory', './')
        params['OutputFormat'] = config['output'].get('format', 'binary')
    if 'input' in config:
        params['FirstFile'] = str(config['input'].get('first_file', 0))
        params['LastFile'] = str(config['input'].get('last_file', 0))
        params['TreeName'] = config['input'].get('tree_name', '')
        params['TreeType'] = config['input'].get('tree_type', 'lhalo_binary')
        params['SimulationDir'] = config['input'].get('simulation_dir', './')
        params['FileWithSnapList'] = config['input'].get('snapshot_list_file', '')
        params['LastSnapshotNr'] = str(config['input'].get('last_snapshot', 0))
    if 'simulation' in config:
        params['BoxSize'] = str(config['simulation'].get('box_size', 0.0))
        params['PartMass'] = str(config['simulation'].get('particle_mass', 0.0))
        if 'cosmology' in config['simulation']:
            params['Omega'] = str(config['simulation']['cosmology'].get('omega_matter', 0.0))
            params['OmegaLambda'] = str(config['simulation']['cosmology'].get('omega_lambda', 0.0))
            params['Hubble_h'] = str(config['simulation']['cosmology'].get('hubble_h', 0.0))
    if 'units' in config:
        params['UnitLength_in_cm'] = str(config['units'].get('length_in_cm', 0.0))
        params['UnitMass_in_g'] = str(config['units'].get('mass_in_g', 0.0))
        params['UnitVelocity_in_cm_per_s'] = str(config['units'].get('velocity_in_cm_per_s', 0.0))

    return params


def create_test_param_file(output_name, enabled_modules=None,
                           module_params=None, first_file=0, last_file=0):
    """
    Create a test YAML parameter file with specified module configuration.

    Args:
        output_name (str): Output file name
        enabled_modules (list): List of modules to enable
        module_params (dict): Module parameters {ModuleName_ParamName: value}
        first_file (int): First tree file number
        last_file (int): Last tree file number

    Returns:
        Path: Path to created parameter file
    """
    import yaml

    if enabled_modules is None:
        enabled_modules = []
    if module_params is None:
        module_params = {}

    # Use test data parameter file instead of millennium.yaml
    test_ref_file = REPO_ROOT / "tests" / "data" / "test_binary.yaml"
    with open(test_ref_file, 'r') as f:
        config = yaml.safe_load(f)

    # Create output directory
    output_dir = Path(temp_dir) / output_name
    output_dir.mkdir(parents=True, exist_ok=True)

    # Update configuration
    config['output']['directory'] = str(output_dir)
    config['output']['format'] = 'binary'
    config['input']['first_file'] = first_file
    config['input']['last_file'] = last_file

    # Update module configuration
    config['modules']['enabled'] = enabled_modules

    # Parse and add module parameters
    if module_params:
        if 'parameters' not in config['modules']:
            config['modules']['parameters'] = {}

        for param_name, value in module_params.items():
            # Parse ModuleName_ParameterName format
            if '_' in param_name:
                module_name, param_key = param_name.split('_', 1)
                if module_name not in config['modules']['parameters']:
                    config['modules']['parameters'][module_name] = {}
                # Try to convert to appropriate type
                try:
                    value = float(value)
                    if value.is_integer():
                        value = int(value)
                except (ValueError, AttributeError):
                    pass  # Keep as string
                config['modules']['parameters'][module_name][param_key] = value

    # Write test parameter file as YAML
    param_path = Path(temp_dir) / f"{output_name}.yaml"
    with open(param_path, 'w') as f:
        f.write("#" + "="*77 + "\n")
        f.write("# test_fixture Integration Test\n")
        f.write("#" + "="*77 + "\n\n")
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    return param_path


def setup_module():
    """Set up test environment - called before tests."""
    global temp_dir, ref_param_file
    temp_dir = tempfile.mkdtemp(prefix="mimic_test_fixture_")
    os.makedirs(f"{temp_dir}/output", exist_ok=True)
    # Use test data parameter file which has correct test paths
    ref_param_file = REPO_ROOT / "tests" / "data" / "test_binary.yaml"

    print("\n" + "=" * 71)
    print("TEST FIXTURE MODULE - INTEGRATION TESTS")
    print("=" * 71)
    print()
    print("⚠️  These tests validate the test_fixture module itself")
    print("    The test_fixture exists for testing infrastructure only")
    print()
    print(f"Temp directory: {temp_dir}")
    print()


def teardown_module():
    """Clean up test environment - called after tests."""
    import shutil
    if temp_dir and Path(temp_dir).exists():
        shutil.rmtree(temp_dir)
        print(f"\nCleaned up: {temp_dir}")


def test_module_loads():
    """
    Test: Module loads and initializes correctly

    Expected: Module registration succeeds, init completes without error
    """
    print(f"{BLUE}TEST:{NC} test_module_loads")

    # Create minimal parameter file with test_fixture
    param_file = create_test_param_file(
        "test_fixture_load",
        enabled_modules=["test_fixture"],
        module_params={"TestFixture_DummyParameter": "1.0"},
        first_file=0,
        last_file=0
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Validate
    if returncode != 0:
        print(f"\n{RED}Test failed: Mimic should exit successfully (got {returncode}){NC}")
        print(f"\nGenerated param file: {param_file}")
        print(f"\nSTDOUT:\n{stdout}")
        print(f"\nSTDERR:\n{stderr}\n")
    assert returncode == 0, f"Mimic should exit successfully (got {returncode})"
    assert "Test fixture module initialized" in stdout, \
        "Module initialization message should appear in output"

    print(f"  {GREEN}✓{NC} Module loaded and initialized")


def test_parameter_configuration():
    """
    Test: DummyParameter can be configured

    Expected: Custom parameter value is read correctly
    """
    print(f"{BLUE}TEST:{NC} test_parameter_configuration")

    # Create parameter file with custom DummyParameter
    param_file = create_test_param_file(
        "test_fixture_param",
        enabled_modules=["test_fixture"],
        module_params={"TestFixture_DummyParameter": "3.14"},
        first_file=0,
        last_file=0
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Validate
    assert returncode == 0, f"Mimic should exit successfully (got {returncode})"
    assert "DummyParameter = 3.14" in stdout, \
        "Custom parameter value should appear in output"

    print(f"  {GREEN}✓{NC} Parameter configuration works")


def test_execution_completes():
    """
    Test: Module executes to completion without errors

    Expected: Full pipeline completes, output files created
    """
    print(f"{BLUE}TEST:{NC} test_execution_completes")

    # Create parameter file for full execution
    param_file = create_test_param_file(
        "test_fixture_exec",
        enabled_modules=["test_fixture"],
        module_params={"TestFixture_DummyParameter": "1.0"},
        first_file=0,
        last_file=0
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Validate execution
    assert returncode == 0, \
        f"Mimic should exit successfully (got {returncode})\nStderr: {stderr}"

    # Check module initialized and cleaned up
    assert "Test fixture module initialized" in stdout, \
        "Module initialization message should appear"

    # Check output directory was created
    output_dir = Path(temp_dir) / "test_fixture_exec"
    assert output_dir.exists(), f"Output directory should exist: {output_dir}"

    print(f"  {GREEN}✓{NC} Execution completed successfully")


def test_memory_safety():
    """
    Test: No memory leaks during execution

    Expected: "No memory leaks detected" message appears
    """
    print(f"{BLUE}TEST:{NC} test_memory_safety")

    # Create parameter file
    param_file = create_test_param_file(
        "test_fixture_memory",
        enabled_modules=["test_fixture"],
        module_params={"TestFixture_DummyParameter": "1.0"},
        first_file=0,
        last_file=0
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Validate no memory leaks
    assert returncode == 0, \
        f"Mimic should exit successfully (got {returncode})\nStderr: {stderr}"

    # Check for memory leak detection messages
    # Success message: "No memory leaks detected"
    # Failure message: "Memory leak detected"
    assert "Memory leak detected" not in stdout, \
        "Should not have 'Memory leak detected' warning message"
    assert "Memory leak detected" not in stderr, \
        "Should not have memory leak warnings in stderr"

    print(f"  {GREEN}✓{NC} No memory leaks detected")


if __name__ == "__main__":
    # Run tests
    try:
        setup_module()

        test_module_loads()
        test_parameter_configuration()
        test_execution_completes()
        test_memory_safety()

        print()
        print("=" * 71)
        print(f"{GREEN}All tests passed!{NC}")
        print("=" * 71)
        print()

        teardown_module()
        sys.exit(0)

    except AssertionError as e:
        print()
        print(f"{RED}Test failed: {e}{NC}")
        print()
        teardown_module()
        sys.exit(1)

    except Exception as e:
        print()
        print(f"{RED}Error: {e}{NC}")
        print()
        teardown_module()
        sys.exit(1)
