#!/usr/bin/env python3
"""
SAGE Add Infall Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration

This test validates software quality aspects of the sage_add_infall module:
- Module loads and initializes correctly
- Module executes without errors or memory leaks
- Output properties appear in output files
- Module works with sage_infall in multi-phase pipeline
- Proper substep distribution

NOTE: sage_add_infall requires sage_infall (pre_timestep) to calculate InfallingGas

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: HotGas properties in output
  - test_with_sage_infall: Integration with sage_infall (required)
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_substep_distribution: Infall distributed over substeps

Author: Mimic Development Team
Date: 2025-12-11
"""

import os
import sys
import shutil
import subprocess
import tempfile
from pathlib import Path

# Repository root and paths
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
MIMIC_EXE = REPO_ROOT / "mimic"

# Add tests directory to path to import framework
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import load_binary_halos

# Test state
temp_dir = None
ref_param_file = None

# ANSI color codes
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


def run_mimic(param_file):
    """
    Execute Mimic with specified parameter file

    Args:
        param_file (Path): Path to parameter file

    Returns:
        tuple: (returncode, stdout, stderr)
    """
    result = subprocess.run(
        [str(MIMIC_EXE), "--verbose", str(param_file)],
        capture_output=True,
        text=True,
        timeout=60  # 60 second timeout
    )
    return result.returncode, result.stdout, result.stderr


def create_test_param_file(output_name, enabled_modules=None,
                          module_params=None, substeps=1, first_file=0, last_file=0):
    """
    Create a test YAML parameter file with specified module configuration.

    Args:
        output_name: Name for output directory
        enabled_modules: List of module names to enable
        module_params: Dict of {ParamName: value} for module parameters
        substeps: Number of substeps (default: 1)
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
    config['output']['output_directory'] = str(output_dir)
    config['output']['output_format'] = 'binary'
    config['input']['first_file'] = first_file
    config['input']['last_file'] = last_file

    # Update module configuration - multi-phase pipeline format
    config['modules']['pre_timestep'] = []
    config['modules']['phase_1'] = []
    config['modules']['phase_2'] = []
    config['modules']['post_timestep'] = []

    # Set substeps
    config['modules']['substeps'] = substeps

    for module_name in enabled_modules:
        if module_name in ['sage_reionization', 'sage_infall']:
            # sage_infall runs in pre_timestep (calculates InfallingGas)
            config['modules']['pre_timestep'].append({module_name: 'once'})
        elif module_name == 'sage_add_infall':
            # sage_add_infall runs in phase_1 (distributes InfallingGas)
            config['modules']['phase_1'].append({module_name: 'once'})
        else:
            config['modules']['phase_1'].append({module_name: 'all'})

    # Add model_parameters
    config['modules']['parameters'] = {
        'GlobalBaryonFraction': 0.17,
    }

    # Override model parameters if provided
    if module_params:
        for param_name, value in module_params.items():
            try:
                value = float(value)
                if value.is_integer():
                    value = int(value)
            except (ValueError, AttributeError):
                pass
            config['modules']['parameters'][param_name] = value

    # Write test parameter file as YAML
    param_path = Path(temp_dir) / f"{output_name}.yaml"
    with open(param_path, 'w') as f:
        f.write("#" + "="*77 + "\n")
        f.write("# sage_add_infall Integration Test\n")
        f.write("#" + "="*77 + "\n\n")
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    return param_path


def test_module_loads():
    """
    Test that sage_add_infall module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    Note: Requires sage_infall in pre_timestep to set InfallingGas property
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file = create_test_param_file(
        output_name="sage_add_infall_load",
        enabled_modules=["sage_reionization", "sage_infall", "sage_add_infall"]
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Mimic should execute successfully with sage_add_infall\nStderr: {stderr}"

    # Check initialization log message
    assert "SAGE add infall module initialized" in stdout, \
        "sage_add_infall should log initialization message"

    # Check that sage_infall ran first
    assert "SAGE infall module initialized" in stdout, \
        "sage_infall should run before sage_add_infall"

    print("  ✓ Module loads and initializes successfully")


def test_output_properties_exist():
    """
    Test that HotGas properties appear in output

    Expected: HotGas and MetalsHotGas in output file
    Validates: Module creates expected output properties
    """
    print("Testing output properties...")

    # ===== SETUP =====
    param_file = create_test_param_file(
        output_name="sage_add_infall_output",
        enabled_modules=["sage_reionization", "sage_infall", "sage_add_infall"]
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic execution should succeed"

    # ===== VALIDATE =====
    output_dir = temp_dir / "sage_add_infall_output"
    output_file = output_dir / "model_z0.000_0"
    assert output_file.exists(), "Output file should exist"

    # Load and check halos
    halos, metadata = load_binary_halos(output_file)
    assert len(halos) > 0, "Should have halos in output"

    # Check output properties exist
    assert 'HotGas' in halos.dtype.names, \
        "HotGas property should exist in output"
    assert 'MetalsHotGas' in halos.dtype.names, \
        "MetalsHotGas property should exist in output"

    print("  ✓ Output properties exist")
    print(f"  Found {len(halos)} halos")


def test_with_sage_infall():
    """
    Test that sage_add_infall works with sage_infall module

    Expected: Both modules execute successfully together
    Validates: sage_add_infall requires sage_infall to set InfallingGas
    """
    print("Testing with sage_infall...")

    # ===== SETUP =====
    param_file = create_test_param_file(
        output_name="sage_add_infall_with_infall",
        enabled_modules=["sage_reionization", "sage_infall", "sage_add_infall"]
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Should run with both modules\nStderr: {stderr}"

    # Verify all modules initialized
    assert "SAGE reionization module initialized" in stdout, \
        "sage_reionization should initialize"
    assert "SAGE infall module initialized" in stdout, \
        "sage_infall should initialize"
    assert "SAGE add infall module initialized" in stdout, \
        "sage_add_infall should initialize"

    print("  ✓ Works with sage_infall")


def test_memory_safety():
    """
    Test that sage_add_infall doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file = create_test_param_file(
        output_name="sage_add_infall_memory",
        enabled_modules=["sage_reionization", "sage_infall", "sage_add_infall"]
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
        output_name="sage_add_infall_complete",
        enabled_modules=["sage_reionization", "sage_infall", "sage_add_infall"],
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Pipeline should complete successfully"
    assert "SAGE add infall module initialized" in stdout, \
        "Module initialization message"
    assert "SAGE add infall module cleaned up" in stdout, \
        "Module cleanup message"

    print("  ✓ Full pipeline completes")


def test_substep_distribution():
    """
    Test that infall is properly distributed over substeps

    Expected: Module executes with multiple substeps without errors
    Validates: Substep distribution logic
    """
    print("Testing substep distribution...")

    # ===== SETUP =====
    param_file = create_test_param_file(
        output_name="sage_add_infall_substeps",
        enabled_modules=["sage_reionization", "sage_infall", "sage_add_infall"],
        substeps=4  # Test with 4 substeps
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with substeps should succeed"
    assert "SAGE add infall module initialized" in stdout, \
        "Module should initialize with substeps"

    # Load output and verify HotGas is reasonable
    output_dir = temp_dir / "sage_add_infall_substeps"
    output_file = output_dir / "model_z0.000_0"
    halos, metadata = load_binary_halos(output_file)

    # Just check that HotGas exists and is non-negative
    assert all(halos['HotGas'] >= 0), "HotGas should be non-negative"

    print("  ✓ Substep distribution works (4 substeps)")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    global temp_dir, ref_param_file

    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: SAGE Add Infall Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    # Set up test environment
    ref_param_file = REPO_ROOT / "tests" / "data" / "test_binary.yaml"
    if not ref_param_file.exists():
        print(f"{RED}ERROR: Reference parameter file not found: {ref_param_file}{NC}")
        return 1

    temp_dir = Path(tempfile.mkdtemp(prefix="mimic_sage_add_infall_test_"))

    try:
        tests = [
            test_module_loads,
            test_output_properties_exist,
            test_with_sage_infall,
            test_memory_safety,
            test_execution_completes,
            test_substep_distribution,
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

    finally:
        # Clean up
        if temp_dir and temp_dir.exists():
            shutil.rmtree(temp_dir)


if __name__ == "__main__":
    sys.exit(main())
