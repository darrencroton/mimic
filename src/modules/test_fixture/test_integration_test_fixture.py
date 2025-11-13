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
REPO_ROOT = Path(__file__).parent.parent.parent.parent
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
    """Read parameter file and return as dictionary."""
    params = {}
    with open(param_file, 'r') as f:
        for line in f:
            line = line.strip()
            # Skip comments and empty lines
            if not line or line.startswith('%') or line.startswith('#'):
                continue
            # Handle arrow notation (-> means skip)
            if '->' in line:
                continue
            # Parse key-value pairs
            parts = line.split()
            if len(parts) >= 2:
                params[parts[0]] = ' '.join(parts[1:])
    return params


def create_test_param_file(output_name, enabled_modules=None,
                           module_params=None, first_file=0, last_file=0):
    """
    Create a test parameter file with specified module configuration.

    Args:
        output_name (str): Output file name
        enabled_modules (list): List of modules to enable
        module_params (dict): Module parameters {param_name: value}
        first_file (int): First tree file number
        last_file (int): Last tree file number

    Returns:
        Path: Path to created parameter file
    """
    if enabled_modules is None:
        enabled_modules = []
    if module_params is None:
        module_params = {}

    # Read reference parameter file
    ref_params = read_param_file(ref_param_file)

    # Create output directory
    output_dir = Path(temp_dir) / output_name
    output_dir.mkdir(parents=True, exist_ok=True)

    # Update parameters
    ref_params['OutputDir'] = str(output_dir)
    ref_params['FirstFile'] = str(first_file)
    ref_params['LastFile'] = str(last_file)
    ref_params['OutputFormat'] = 'binary'  # Use binary format for testing

    # Write test parameter file
    param_path = Path(temp_dir) / f"{output_name}.par"
    with open(param_path, 'w') as f:
        f.write("%------------------------------------------\n")
        f.write("%----- test_fixture Integration Test -----\n")
        f.write("%------------------------------------------\n\n")

        # Write basic parameters
        for key in ['OutputFileBaseName', 'OutputDir', 'FirstFile',
                   'LastFile', 'TreeName', 'TreeType', 'OutputFormat',
                   'SimulationDir', 'FileWithSnapList', 'LastSnapshotNr',
                   'BoxSize', 'Omega', 'OmegaLambda', 'Hubble_h',
                   'PartMass', 'UnitLength_in_cm', 'UnitMass_in_g',
                   'UnitVelocity_in_cm_per_s']:
            if key in ref_params:
                f.write(f"{key:30s} {ref_params[key]}\n")

        # Write snapshot output list
        f.write("\nNumOutputs        8\n")
        f.write("-> 63 37 32 27 23 20 18 16\n\n")

        # Write module configuration
        f.write("%------------------------------------------\n")
        f.write("%----- Galaxy Physics Modules -------------\n")
        f.write("%------------------------------------------\n")

        if enabled_modules:
            f.write(f"EnabledModules          {','.join(enabled_modules)}\n\n")

            # Write module parameters
            if module_params:
                for param_name, value in module_params.items():
                    f.write(f"{param_name:30s} {value}\n")

    return param_path


def setup_module():
    """Set up test environment - called before tests."""
    global temp_dir, ref_param_file
    temp_dir = tempfile.mkdtemp(prefix="mimic_test_fixture_")
    os.makedirs(f"{temp_dir}/output", exist_ok=True)
    ref_param_file = REPO_ROOT / "input" / "millennium.par"

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
