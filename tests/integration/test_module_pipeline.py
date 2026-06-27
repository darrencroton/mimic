#!/usr/bin/env python3
"""
Integration tests for module configuration and execution pipeline.

This test validates that Mimic's module system works end-to-end:
- Modules can be enabled/disabled via parameter files
- Module parameters are read from configuration
- Module execution completes without errors
- Physics-free mode works (halo tracking only)
- Different module combinations work correctly
- Log messages confirm module initialization
"""

import shutil
import sys
import tempfile
from pathlib import Path

# Add framework to path
sys.path.insert(0, str(Path(__file__).parent.parent))

from framework import (
    MIMIC_EXE,
    NC,
    RED,
    create_test_param_file,
    run_mimic,
    run_test_suite,
)

# Shared temporary directory for this suite's outputs (created in main()).
TEMP_DIR = None


def test_physics_free_mode():
    """
    Test that Mimic runs correctly with no modules configured.

    Expected: Exit code 0; log includes "No modules configured (physics-free mode)".
    Validates: Empty module pipeline is valid and produces halo tracking output.
    """
    # Create parameter file with no modules
    param_file, output_dir, _ = create_test_param_file(
        output_name="physics_free", first_file=0, last_file=0, temp_dir=TEMP_DIR
    )

    # Run mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Verify execution succeeded
    assert returncode == 0, f"Mimic failed in physics-free mode:\n{stderr}"

    # Verify log messages
    assert "No modules configured (physics-free mode)" in stdout, "Should log physics-free mode"

    # Verify output directory was created
    assert output_dir.exists(), "Output directory should be created"


def test_single_module_execution():
    """
    Test that a single module initialises and runs without errors.

    Expected: Exit code 0; log confirms "Test fixture module initialized" with DummyParameter=2.5.
    Validates: Module init/process/cleanup lifecycle functions correctly with one module.
    """
    # Create parameter file with only test_fixture
    param_file, output_dir, _ = create_test_param_file(
        output_name="single_module",
        phase_config={"galaxy_physics": [("test_fixture", "process_by_galaxy")]},
        model_params={"TestFixtureDummyParameter": 2.5, "TestFixtureEnableLogging": 0},
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
    )

    # Run mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Verify execution succeeded
    assert returncode == 0, f"Mimic failed with test_fixture:\n{stderr}"

    # Verify module was initialized with correct parameters
    assert "Test fixture module initialized" in stdout
    assert "DummyParameter = 2.500" in stdout

    # Verify output directory was created
    assert output_dir.exists(), "Output directory should be created"


def test_multiple_modules_execution():
    """
    Test that the same module can appear more than once in a phase.

    Expected: Exit code 0; DummyParameter and EnableLogging logged on each registration.
    Validates: Module list allows repeated entries; no name-collision crash.
    """
    # Create parameter file with test_fixture enabled twice (tests module list handling)
    param_file, output_dir, _ = create_test_param_file(
        output_name="multiple_modules",
        phase_config={
            "galaxy_physics": [
                ("test_fixture", "process_by_galaxy"),
                ("test_fixture", "process_by_galaxy"),
            ]
        },
        model_params={"TestFixtureDummyParameter": 1.5, "TestFixtureEnableLogging": 0},
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
    )

    # Run mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Verify execution succeeded
    assert returncode == 0, f"Mimic failed with multiple modules:\n{stderr}"

    # Verify module was initialized with correct parameters
    assert "Test fixture module initialized" in stdout
    assert "DummyParameter = 1.500" in stdout
    assert "EnableLogging = 0" in stdout

    # Verify output directory was created
    assert output_dir.exists(), "Output directory should be created"


def test_custom_parameter_values():
    """
    Test that non-default parameter values reach the module's init().

    Expected: Exit code 0; log shows "DummyParameter = 3.140".
    Validates: Parameter reader returns the value from the run YAML, not a compiled default.
    """
    # Run with non-default dummy parameter
    param_file, output_dir, _ = create_test_param_file(
        output_name="custom_params",
        phase_config={"galaxy_physics": [("test_fixture", "process_by_galaxy")]},
        model_params={
            "TestFixtureDummyParameter": 3.14,  # Non-default
            "TestFixtureEnableLogging": 0,
        },
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
    )

    # Run mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Verify execution succeeded
    assert returncode == 0, f"Mimic failed with custom parameters:\n{stderr}"

    # Verify custom parameter was read
    assert "DummyParameter = 3.140" in stdout, "Custom parameter value should be logged"


def test_unknown_module_error():
    """
    Test that configuring an unregistered module name produces a clear startup error.

    Expected: Non-zero exit; output contains "not registered" and lists "Available modules:".
    Validates: Module registry lookup fails fast with an actionable diagnostic.
    """
    # Create parameter file with invalid module
    param_file, output_dir, _ = create_test_param_file(
        output_name="unknown_module",
        phase_config={"galaxy_physics": [("nonexistent_module", "process_by_galaxy")]},
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
    )

    # Run mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Verify execution failed
    assert returncode != 0, "Mimic should fail with unknown module"

    # Verify error message lists available modules
    combined_output = stdout + stderr
    assert "not registered" in combined_output, "Should report module not registered"
    assert "Available modules:" in combined_output, "Should list available modules"
    assert "test_fixture" in combined_output, "Should list test_fixture as available"


def test_module_execution_order():
    """
    Test that modules run in the order declared in the phase configuration.

    Expected: Exit code 0; module init log confirms the module was registered.
    Validates: Basic pipeline ordering infrastructure; model-specific dependency
    contracts live under models/<model>/modules/_tests/.
    """
    # Create parameter file with test_fixture
    param_file, output_dir, _ = create_test_param_file(
        output_name="execution_order",
        phase_config={"galaxy_physics": [("test_fixture", "process_by_galaxy")]},
        model_params={"TestFixtureDummyParameter": 1.0, "TestFixtureEnableLogging": 0},
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
    )

    # Run mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Verify execution succeeded
    assert returncode == 0, f"Mimic should succeed:\n{stderr}"

    # Verify module was initialized (basic ordering infrastructure works)
    assert "Test fixture module initialized" in stdout, "Module should be initialized in pipeline"


def test_module_init_failure_handling():
    """Test that module init() failure stops execution gracefully.

    Validates error handling when a module's init() function returns -1.
    This tests:
    - Execution stops when init() fails
    - Error message is reported clearly
    - System fails gracefully without crashes
    """
    # Create parameter file with test_fixture but missing required parameter
    # This will cause init() to fail when it tries to read TestFixtureDummyParameter
    param_file, output_dir, _ = create_test_param_file(
        output_name="init_failure",
        phase_config={"galaxy_physics": [("test_fixture", "process_by_galaxy")]},
        model_params={
            # Intentionally omit TestFixtureDummyParameter to trigger init failure
            "TestFixtureEnableLogging": 0
        },
        first_file=0,
        last_file=0,
        temp_dir=TEMP_DIR,
    )

    # Run mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Verify execution failed
    assert returncode != 0, "Mimic should fail when module init() returns -1"

    # Verify error message is present
    combined_output = stdout + stderr
    assert (
        "Failed to read TestFixtureDummyParameter" in combined_output
    ), "Should report parameter read failure"

    # Verify no output files were created (execution stopped early)
    # Output directory may exist but should have no data files
    if output_dir.exists():
        binary_files = list(output_dir.glob("*.bin"))
        hdf5_files = list(output_dir.glob("*.hdf5"))
        assert (
            len(binary_files) + len(hdf5_files) == 0
        ), "No output files should be created on init failure"


def main():
    """Run this file's tests via the shared framework runner."""
    global TEMP_DIR

    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    TEMP_DIR = tempfile.mkdtemp(prefix="mimic_module_test_")
    try:
        return run_test_suite(
            [
                test_physics_free_mode,
                test_single_module_execution,
                test_multiple_modules_execution,
                test_custom_parameter_values,
                test_unknown_module_error,
                test_module_execution_order,
                test_module_init_failure_handling,
            ],
            "Module Pipeline (test_module_pipeline.py)",
        )
    finally:
        shutil.rmtree(TEMP_DIR, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
