#!/usr/bin/env python3
"""
SAGE Infall Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration
Phase: Phase 4.2 (SAGE Physics Module Implementation)

This test validates software quality aspects of the sage_infall module:
- Module loads and initializes correctly
- Parameters can be configured via .par files
- Module executes without errors or memory leaks
- Output properties appear in output files
- Module works in multi-module pipelines

NOTE: Physics validation (reionization correctness, infall amounts) deferred
      to Phase 4.3+ when downstream modules (cooling, star formation) are implemented.

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: HotGas properties in output
  - test_parameters_configurable: Parameter reading and validation
  - test_reionization_toggle: ReionizationOn parameter works
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_multiple_module_pipeline: Multi-module integration

Author: Mimic Development Team
Date: 2025-11-13
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
        timeout=60  # 60 second timeout for integration tests
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
        output_name: Name for output directory
        enabled_modules: List of module names to enable
        module_params: Dict of {ParamName: value} for module parameters
        first_file: First file to process (default: 0)
        last_file: Last file to process (default: 0)

    Returns:
        Path to created parameter file
    """
    # Read reference parameter file
    ref_params = read_param_file(ref_param_file)

    # Create output directory
    output_dir = temp_dir / output_name
    output_dir.mkdir(parents=True, exist_ok=True)

    # Update parameters
    ref_params['OutputDir'] = str(output_dir)
    ref_params['FirstFile'] = str(first_file)
    ref_params['LastFile'] = str(last_file)
    ref_params['OutputFormat'] = 'binary'  # Use binary format for testing

    # Write test parameter file
    param_path = temp_dir / f"{output_name}.par"
    with open(param_path, 'w') as f:
        f.write("%------------------------------------------\n")
        f.write("%----- sage_infall Integration Test ------\n")
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


def test_module_loads():
    """
    Test that sage_infall module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file = create_test_param_file(
        output_name="sage_infall_load",
        enabled_modules=["sage_infall"],
        module_params={
            "SageInfall_BaryonFrac": "0.17",
            "SageInfall_ReionizationOn": "1"
        }
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Mimic should execute successfully with sage_infall\nStderr: {stderr}"

    # Check initialization log message
    assert "SAGE infall module initialized" in stdout, \
        "sage_infall should log initialization message"

    print("  ✓ Module loads and initializes successfully")


def test_output_properties_exist():
    """
    Test that HotGas and related properties appear in output

    Expected: HotGas, MetalsHotGas, and related properties in output file
    Validates: Module creates expected output properties
    """
    print("Testing output properties...")

    # ===== SETUP =====
    param_file = create_test_param_file(
        output_name="sage_infall_output",
        enabled_modules=["sage_infall"]
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic execution should succeed"

    # ===== VALIDATE =====
    # Load output file
    output_dir = temp_dir / "sage_infall_output"
    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"

    # Load and check halos
    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have halos in output"

    # Check HotGas property exists
    assert 'HotGas' in halos.dtype.names, \
        "HotGas property should exist in output"
    assert 'MetalsHotGas' in halos.dtype.names, \
        "MetalsHotGas property should exist in output"

    print("  ✓ Output properties exist")
    print(f"  Found {len(halos)} halos")


def test_parameters_configurable():
    """
    Test that all sage_infall parameters can be configured

    Expected: Custom parameter values are read and logged
    Validates: Parameter reading and validation system
    """
    print("Testing parameter configuration...")

    # ===== SETUP =====
    # Test with non-default parameter values
    param_file = create_test_param_file(
        output_name="sage_infall_params",
        enabled_modules=["sage_infall"],
        module_params={
            "SageInfall_BaryonFrac": "0.20",
            "SageInfall_ReionizationOn": "0",
            "SageInfall_Reionization_z0": "9.0",
            "SageInfall_Reionization_zr": "6.0",
            "SageInfall_StrippingSteps": "5"
        }
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with custom parameters should succeed"

    # Verify parameters were read
    assert "BaryonFrac = 0.2000" in stdout, \
        "Custom BaryonFrac should be logged"
    assert "ReionizationOn = 0" in stdout, \
        "Custom ReionizationOn should be logged"

    print("  ✓ Parameters are configurable")


def test_reionization_toggle():
    """
    Test that ReionizationOn parameter affects execution

    Expected: Both ON and OFF modes execute without errors
    Validates: Parameter toggle doesn't cause crashes
    """
    print("Testing reionization toggle...")

    # ===== SETUP & EXECUTE =====
    # Test with reionization ON
    param_file_on = create_test_param_file(
        output_name="sage_infall_reion_on",
        enabled_modules=["sage_infall"],
        module_params={"SageInfall_ReionizationOn": "1"}
    )
    returncode_on, stdout_on, stderr_on = run_mimic(param_file_on)

    # Test with reionization OFF
    param_file_off = create_test_param_file(
        output_name="sage_infall_reion_off",
        enabled_modules=["sage_infall"],
        module_params={"SageInfall_ReionizationOn": "0"}
    )
    returncode_off, stdout_off, stderr_off = run_mimic(param_file_off)

    # ===== VALIDATE =====
    assert returncode_on == 0, "Should run with reionization ON"
    assert returncode_off == 0, "Should run with reionization OFF"
    assert "ReionizationOn = 1" in stdout_on, "ON mode logged"
    assert "ReionizationOn = 0" in stdout_off, "OFF mode logged"

    print("  ✓ Reionization toggle works")


def test_memory_safety():
    """
    Test that sage_infall doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file = create_test_param_file(
        output_name="sage_infall_memory",
        enabled_modules=["sage_infall"]
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution should succeed"
    assert "Memory leak detected" not in stdout, \
        "Should not have memory leaks"
    assert "Memory leak detected" not in stderr, \
        "Should not have memory leaks in stderr"

    print("  ✓ No memory leaks detected")


def test_execution_completes():
    """
    Test that full pipeline execution completes without errors

    Expected: Initialization, processing, and cleanup all succeed
    Validates: Complete module lifecycle
    """
    print("Testing full pipeline completion...")

    # ===== SETUP =====
    param_file = create_test_param_file(
        output_name="sage_infall_complete",
        enabled_modules=["sage_infall"],
        first_file=0,
        last_file=0  # Process single file
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Pipeline should complete successfully"
    assert "SAGE infall module initialized" in stdout, \
        "Module initialization message"
    assert "SAGE infall module cleaned up" in stdout, \
        "Module cleanup message"

    print("  ✓ Full pipeline completes")


def test_multiple_module_pipeline():
    """
    Test that sage_infall works with other modules in pipeline

    Expected: Multi-module pipeline executes successfully
    Validates: Inter-module compatibility
    """
    print("Testing multi-module pipeline...")

    # ===== SETUP =====
    # Test sage_infall with simple_sfr (simple_cooling archived)
    param_file = create_test_param_file(
        output_name="sage_infall_multi",
        enabled_modules=["sage_infall", "simple_sfr"],
        module_params={
            "SageInfall_BaryonFrac": "0.17",
            "SimpleSFR_Efficiency": "0.02"
        }
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Multi-module pipeline should execute successfully\nStderr: {stderr}"

    # Verify all modules initialized
    assert "SAGE infall module initialized" in stdout, \
        "sage_infall should initialize"
    assert "Simple star formation rate module initialized" in stdout, \
        "simple_sfr should initialize"

    print("  ✓ Multi-module pipeline works")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    global temp_dir, ref_param_file

    print("=" * 60)
    print("Integration Test Suite: sage_infall")
    print("=" * 60)
    print()

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    # Set up test environment
    ref_param_file = REPO_ROOT / "input" / "millennium.par"
    if not ref_param_file.exists():
        print(f"{RED}ERROR: Reference parameter file not found: {ref_param_file}{NC}")
        return 1

    temp_dir = Path(tempfile.mkdtemp(prefix="mimic_sage_infall_test_"))

    try:
        tests = [
            test_module_loads,
            test_output_properties_exist,
            test_parameters_configurable,
            test_reionization_toggle,
            test_memory_safety,
            test_execution_completes,
            test_multiple_module_pipeline,
        ]

        passed = 0
        failed = 0

        for test in tests:
            print()
            try:
                test()
                passed += 1
            except AssertionError as e:
                print(f"{RED}✗ FAIL: {test.__name__}{NC}")
                print(f"  {e}")
                failed += 1
            except Exception as e:
                print(f"{RED}✗ ERROR: {test.__name__}{NC}")
                print(f"  {e}")
                failed += 1

        print()
        print("=" * 60)
        print("Test Summary")
        print("=" * 60)
        print(f"Passed: {passed}")
        print(f"Failed: {failed}")
        print(f"Total:  {passed + failed}")
        print("=" * 60)

        if failed == 0:
            print(f"{GREEN}✓ All tests passed!{NC}")
            return 0
        else:
            print(f"{RED}✗ {failed} test(s) failed{NC}")
            return 1

    finally:
        # Clean up
        if temp_dir and temp_dir.exists():
            shutil.rmtree(temp_dir)


if __name__ == "__main__":
    sys.exit(main())
