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
import subprocess
import sys
import tempfile
from pathlib import Path

# Repository root and paths
# test_fixture is in src/module_system/test_fixture/_tests.
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
MIMIC_EXE = REPO_ROOT / "mimic"

# Add tests directory to path to import framework
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import core_input_file, load_binary_halos

# Test state
temp_dir = None
ref_param_file = None

# ANSI color codes (module-level constants)
BLUE = "\033[1;34m"
GREEN = "\033[0;32m"
RED = "\033[0;31m"
YELLOW = "\033[1;33m"
NC = "\033[0m"


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
        timeout=60,  # 60 second timeout
    )
    return result.returncode, result.stdout, result.stderr


def create_test_param_file(
    output_name, phase_config=None, model_params=None, first_file=0, last_file=0
):
    """
    Create a test YAML parameter file with specified module configuration.

    Args:
        output_name (str): Output file name
        phase_config (dict): Module phase configuration
        model_params (dict): Module parameters
        first_file (int): First tree file number
        last_file (int): Last tree file number

    Returns:
        Path: Path to created parameter file
    """
    import yaml

    if phase_config is None:
        phase_config = {}
    if model_params is None:
        model_params = {}

    # Use the compiled model's local test parameter file instead of a production run file.
    test_ref_file = core_input_file("test_binary.yaml")
    with open(test_ref_file, "r") as f:
        config = yaml.safe_load(f)

    # Create output directory
    output_dir = Path(temp_dir) / output_name
    output_dir.mkdir(parents=True, exist_ok=True)

    # Update configuration
    config["output"]["output_directory"] = str(output_dir)
    config["output"]["output_format"] = "binary"

    sim_config_path = REPO_ROOT / config["simulation"]["config"]
    with open(sim_config_path, "r") as f:
        sim_config = yaml.safe_load(f)
    sim_config["input"]["first_file"] = first_file
    sim_config["input"]["last_file"] = last_file
    generated_sim_config = Path(temp_dir) / f"{output_name}_simulation.yaml"
    with open(generated_sim_config, "w") as f:
        yaml.dump(sim_config, f, default_flow_style=False, sort_keys=False)
    config["simulation"]["config"] = str(generated_sim_config)

    # Update module configuration - multi-phase pipeline format
    # Put all modules in galaxy_physics with processing_mode=BY_GALAXY (test_fixture is a simple test module)
    config["modules"]["pre_timestep"] = []
    config["modules"]["galaxy_physics"] = []
    config["modules"]["satellite_mergers"] = []
    config["modules"]["post_timestep"] = []

    for phase_name, modules in phase_config.items():
        config["modules"][phase_name] = [
            {module_name: processing_mode} for module_name, processing_mode in modules
        ]

    # Add model_parameters (test_fixture needs TestFixtureDummyParameter and TestFixtureEnableLogging)
    config["modules"]["parameters"] = {
        "TestFixtureDummyParameter": 1.0,
        "TestFixtureEnableLogging": 0,
    }

    # Override model parameters if provided
    if model_params:
        for param_name, value in model_params.items():
            if param_name == "TestFixtureDummyParameter":
                try:
                    value = float(value)
                    if value.is_integer():
                        value = int(value)
                except (ValueError, AttributeError):
                    pass
                config["modules"]["parameters"]["TestFixtureDummyParameter"] = value
            elif param_name == "TestFixtureEnableLogging":
                config["modules"]["parameters"]["TestFixtureEnableLogging"] = int(value)

    # Write test parameter file as YAML
    param_path = Path(temp_dir) / f"{output_name}.yaml"
    with open(param_path, "w") as f:
        f.write("#" + "=" * 77 + "\n")
        f.write("# test_fixture Integration Test\n")
        f.write("#" + "=" * 77 + "\n\n")
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    return param_path


def setup_module():
    """Set up test environment - called before tests."""
    global temp_dir, ref_param_file
    temp_dir = tempfile.mkdtemp(prefix="mimic_test_fixture_")
    os.makedirs(f"{temp_dir}/output", exist_ok=True)
    ref_param_file = core_input_file("test_binary.yaml")

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
        phase_config={"galaxy_physics": [("test_fixture", "process_by_galaxy")]},
        model_params={"TestFixtureDummyParameter": "1.0"},
        first_file=0,
        last_file=0,
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
    assert (
        "Test fixture module initialized" in stdout
    ), "Module initialization message should appear in output"

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
        phase_config={"galaxy_physics": [("test_fixture", "process_by_galaxy")]},
        model_params={"TestFixtureDummyParameter": "3.14"},
        first_file=0,
        last_file=0,
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Validate
    assert returncode == 0, f"Mimic should exit successfully (got {returncode})"
    assert "DummyParameter = 3.14" in stdout, "Custom parameter value should appear in output"

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
        phase_config={"galaxy_physics": [("test_fixture", "process_by_galaxy")]},
        model_params={"TestFixtureDummyParameter": "1.0"},
        first_file=0,
        last_file=0,
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Validate execution
    assert returncode == 0, f"Mimic should exit successfully (got {returncode})\nStderr: {stderr}"

    # Check module initialized and cleaned up
    assert (
        "Test fixture module initialized" in stdout
    ), "Module initialization message should appear"

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
        phase_config={"galaxy_physics": [("test_fixture", "process_by_galaxy")]},
        model_params={"TestFixtureDummyParameter": "1.0"},
        first_file=0,
        last_file=0,
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Validate no memory leaks
    assert returncode == 0, f"Mimic should exit successfully (got {returncode})\nStderr: {stderr}"

    # Check for memory leak detection messages
    # Success message: "No memory leaks detected"
    # Failure message: "Memory leak detected"
    assert (
        "Memory leak detected" not in stdout
    ), "Should not have 'Memory leak detected' warning message"
    assert "Memory leak detected" not in stderr, "Should not have memory leak warnings in stderr"

    print(f"  {GREEN}✓{NC} No memory leaks detected")


def main():
    """
    Main test runner
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: Test Fixture Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")
    print()

    tests = [
        ("test_module_loads", test_module_loads),
        ("test_parameter_configuration", test_parameter_configuration),
        ("test_execution_completes", test_execution_completes),
        ("test_memory_safety", test_memory_safety),
    ]

    passed = 0
    failed = 0

    try:
        setup_module()

        for test_name, test_func in tests:
            try:
                test_func()
                passed += 1
            except AssertionError as e:
                print(f"{RED}✗ FAIL: {test_name}{NC}")
                print(f"  {e}")
                failed += 1
            except Exception as e:
                print(f"{RED}✗ ERROR: {test_name}{NC}")
                print(f"  {e}")
                failed += 1

        teardown_module()

    except Exception as e:
        print(f"{RED}Setup/teardown error: {e}{NC}")
        teardown_module()
        return 1

    # Print summary
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
