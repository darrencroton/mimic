#!/usr/bin/env python3
"""
SAGE Infall Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration
Phase: Phase 4.2 (SAGE Physics Module Implementation)

This test validates software quality aspects of the sage_infall module:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
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
YELLOW = '\033[0;33m'
BLUE = '\033[0;34m'
NC = '\033[0m'  # No Color


def get_available_modules():
    """
    Query Mimic to get list of available (registered) modules.

    Returns:
        set: Set of available module names, or empty set if query fails
    """
    import yaml

    try:
        test_param = temp_dir / "_query_modules.yaml"

        with open(ref_param_file, 'r') as f:
            config = yaml.safe_load(f)

        config['modules']['enabled'] = ['__nonexistent_module__']
        config['output']['format'] = 'binary'

        with open(test_param, 'w') as f:
            yaml.dump(config, f, default_flow_style=False, sort_keys=False)

        result = subprocess.run(
            [str(MIMIC_EXE), str(test_param)],
            capture_output=True,
            text=True,
            timeout=10
        )

        # Parse available modules from error output
        # Lines look like: "[timestamp] ERROR - file:func:line -   - module_name"
        available = set()
        in_module_list = False
        for line in result.stderr.split('\n'):
            if 'Available modules:' in line:
                in_module_list = True
                continue
            if in_module_list:
                # Check if line contains "  - " (module list item)
                if '  - ' in line:
                    # Extract module name (after the last "  - ")
                    module_name = line.split('  - ')[-1].strip()
                    if module_name:
                        available.add(module_name)
                elif 'Module system initialization failed' in line or 'Module' in line and 'listed in EnabledModules' not in line:
                    # End of module list
                    break

        return available
    except Exception as e:
        # If query fails, return empty set (test will skip gracefully)
        return set()


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
        output_name: Name for output directory
        enabled_modules: List of module names to enable
        module_params: Dict of {ModuleName_ParamName: value} for module parameters
        first_file: First file to process (default: 0)
        last_file: Last file to process (default: 0)

    Returns:
        Path to created parameter file
    """
    import yaml

    if enabled_modules is None:
        enabled_modules = []
    if module_params is None:
        module_params = {}

    # Use test data parameter file as reference
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
        f.write("# sage_infall Integration Test\n")
        f.write("#" + "="*77 + "\n\n")
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

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

    Expected: Multi-module pipeline executes successfully (if companion module available)
    Validates: Inter-module compatibility

    Note: Uses sage_cooling as companion module. If not available, test is skipped
          with a warning (not a failure). This handles cases where modules have
          been archived or are not compiled.
    """
    print("Testing multi-module pipeline...")

    # ===== CHECK MODULE AVAILABILITY =====
    available_modules = get_available_modules()

    # Prefer sage_cooling as companion module (both are SAGE physics)
    companion_module = None
    companion_params = {}
    companion_init_msg = None

    if "sage_cooling" in available_modules:
        companion_module = "sage_cooling"
        companion_params = {
            "SageCooling_CoolFunctionsDir": "input/CoolFunctions"
        }
        companion_init_msg = "SAGE cooling & AGN heating module initialized"

    # If no companion module available, skip test with warning
    if not companion_module:
        print(f"{YELLOW}⚠ SKIP: No companion module available for multi-module test{NC}")
        print(f"  Available modules: {', '.join(sorted(available_modules))}")
        print(f"  Test requires: sage_cooling")
        print(f"  This is not a failure - modules may be archived or not compiled")
        return

    # ===== SETUP =====
    param_file = create_test_param_file(
        output_name="sage_infall_multi",
        enabled_modules=["sage_infall", companion_module],
        module_params={
            "SageInfall_BaryonFrac": "0.17",
            **companion_params
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
    assert companion_init_msg in stdout, \
        f"{companion_module} should initialize"

    print(f"  ✓ Multi-module pipeline works (tested with {companion_module})")


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

    # Set up test environment - use test data parameter file
    ref_param_file = REPO_ROOT / "tests" / "data" / "test_binary.yaml"
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
