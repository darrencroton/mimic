#!/usr/bin/env python3
"""
SAGE Infall Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration

This test validates software quality aspects of the sage_infall module:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- Output properties appear in output files
- Module works in multi-module pipelines
- Integration with sage_satellite_stripping (shared reionization utility)

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: HotGas properties in output
  - test_parameters_configurable: GlobalBaryonFraction parameter configuration
  - test_with_satellite_stripping: Integration with sage_satellite_stripping
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_multiple_module_pipeline: Multi-module integration

Author: Mimic Development Team
Date: 2025-12-11
"""

import os
import sys
import shutil
import subprocess
from pathlib import Path

# Add tests directory to path to import framework
REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    MIMIC_EXE,
    create_test_param_file,
    run_mimic,
    load_binary_halos,
)

# ANSI color codes
BLUE = '\033[1;34m'
GREEN = '\033[0;32m'
RED = '\033[0;31m'
YELLOW = '\033[1;33m'
NC = '\033[0m'


def get_available_modules():
    """
    Query Mimic to get list of available (registered) modules.

    Returns:
        set: Set of available module names, or empty set if query fails
    """
    import yaml
    import tempfile

    try:
        with tempfile.TemporaryDirectory(prefix="mimic_query_") as temp_dir:
            test_param = Path(temp_dir) / "_query_modules.yaml"

            # Use test data as reference
            ref_file = REPO_ROOT / "tests" / "data" / "test_binary.yaml"
            with open(ref_file, 'r') as f:
                config = yaml.safe_load(f)

            # Trigger error by requesting nonexistent module
            config['modules']['phase_1'] = [{'__nonexistent_module__': 'all'}]
            config['output']['output_format'] = 'binary'
            config['output']['output_directory'] = temp_dir

            with open(test_param, 'w') as f:
                yaml.dump(config, f, default_flow_style=False, sort_keys=False)

            result = subprocess.run(
                [str(MIMIC_EXE), str(test_param)],
                capture_output=True,
                text=True,
                timeout=10
            )

            # Parse available modules from error output
            available = set()
            in_module_list = False
            for line in result.stderr.split('\n'):
                if 'Available modules:' in line:
                    in_module_list = True
                    continue
                if in_module_list:
                    if '  - ' in line:
                        module_name = line.split('  - ')[-1].strip()
                        if module_name:
                            available.add(module_name)
                    elif 'Module system initialization failed' in line or 'Module' in line and 'listed in EnabledModules' not in line:
                        break

            return available
    except Exception:
        return set()


def test_module_loads():
    """
    Test that sage_infall module loads and initializes successfully

    Expected: Module initialization succeeds without errors
    Validates: Module registration, initialization, and cleanup lifecycle
    Note: Requires sage_reionization module to set HaloBaryonFraction property
    """
    print("Testing module load and initialization...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_infall_load",
        phase_config={
            'pre_timestep': [('sage_reionization', 'once'), ('sage_infall', 'once')],
            'phase_1': [],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Mimic should execute successfully with sage_infall\nStderr: {stderr}"

    # Check initialization log message
    assert "SAGE infall module initialized" in stdout, \
        "sage_infall should log initialization message"

    # Check that reionization module ran first
    assert "SAGE reionization module initialized" in stdout, \
        "sage_reionization should run before sage_infall"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Module loads and initializes successfully")


def test_output_properties_exist():
    """
    Test that HotGas and related properties appear in output

    Expected: HotGas, MetalsHotGas, EjectedMass, and ICS in output file
    Validates: Module creates expected output properties
    """
    print("Testing output properties...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_infall_output",
        phase_config={
            'pre_timestep': [('sage_reionization', 'once'), ('sage_infall', 'once')],
            'phase_1': [],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, "Mimic execution should succeed"

    # ===== VALIDATE =====
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
    assert 'EjectedMass' in halos.dtype.names, \
        "EjectedMass property should exist in output"
    assert 'ICS' in halos.dtype.names, \
        "ICS property should exist in output"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Output properties exist")
    print(f"  Found {len(halos)} halos")


def test_parameters_configurable():
    """
    Test that sage_reionization uses GlobalBaryonFraction model parameter

    Expected: Custom GlobalBaryonFraction value is read from model_parameters and logged
    Validates: Model parameter reading and usage
    Note: GlobalBaryonFraction is used by sage_reionization to set HaloBaryonFraction property
    """
    print("Testing parameter configuration...")

    # ===== SETUP =====
    import yaml

    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_infall_params",
        phase_config={
            'pre_timestep': [('sage_reionization', 'once'), ('sage_infall', 'once')],
            'phase_1': [],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.20}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution with custom parameters should succeed"

    # Verify parameter was read by sage_reionization
    assert "GlobalBaryonFraction = 0.2000" in stdout or "GlobalBaryonFraction: 0.2" in stdout, \
        f"Custom GlobalBaryonFraction should be logged\nStdout:\n{stdout}"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Parameters are configurable")


def test_with_satellite_stripping():
    """
    Test that sage_infall works with sage_satellite_stripping module

    Expected: Both modules execute successfully together
    Validates: Modules work together using HaloBaryonFraction property from sage_reionization
    Note: If sage_satellite_stripping not available, test skips gracefully
    """
    print("Testing with sage_satellite_stripping...")

    # ===== CHECK MODULE AVAILABILITY =====
    available_modules = get_available_modules()

    if "sage_satellite_stripping" not in available_modules:
        print(f"{YELLOW}⚠ SKIP: sage_satellite_stripping not available{NC}")
        print(f"  Available modules: {', '.join(sorted(available_modules))}")
        print(f"  This is expected if module is not yet registered")
        return

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_infall_stripping",
        phase_config={
            'pre_timestep': [('sage_reionization', 'once'), ('sage_infall', 'once')],
            'phase_1': [('sage_satellite_stripping', 'once')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, \
        f"Should run with both modules\nStderr: {stderr}"

    # Verify all modules initialized
    assert "SAGE reionization module initialized" in stdout, \
        "sage_reionization should initialize first"
    assert "SAGE infall module initialized" in stdout, \
        "sage_infall should initialize"
    assert "SAGE satellite stripping module initialized" in stdout, \
        "sage_satellite_stripping should initialize"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Works with sage_satellite_stripping")


def test_memory_safety():
    """
    Test that sage_infall doesn't leak memory

    Expected: No memory leak messages in output
    Validates: Proper memory management in module
    """
    print("Testing memory safety...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_infall_memory",
        phase_config={
            'pre_timestep': [('sage_reionization', 'once'), ('sage_infall', 'once')],
            'phase_1': [],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Execution should succeed"
    assert "Memory leak detected" not in stdout, \
        "Should not have memory leaks"
    assert "Memory leak detected" not in stderr, \
        "Should not have memory leaks in stderr"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ No memory leaks detected")


def test_execution_completes():
    """
    Test that full pipeline execution completes without errors

    Expected: Initialization, processing, and cleanup all succeed
    Validates: Complete module lifecycle
    """
    print("Testing full pipeline completion...")

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_infall_complete",
        phase_config={
            'pre_timestep': [('sage_reionization', 'once'), ('sage_infall', 'once')],
            'phase_1': [],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17},
        first_file=0,
        last_file=0
    )

    # ===== EXECUTE =====
    returncode, stdout, stderr = run_mimic(param_file)

    # ===== VALIDATE =====
    assert returncode == 0, "Pipeline should complete successfully"
    assert "SAGE infall module initialized" in stdout, \
        "Module initialization message"
    assert "SAGE infall module cleaned up" in stdout, \
        "Module cleanup message"

    # Cleanup
    shutil.rmtree(temp_dir)

    print("  ✓ Full pipeline completes")


def test_multiple_module_pipeline():
    """
    Test that sage_infall works with other modules in pipeline

    Expected: Multi-module pipeline executes successfully (if companion module available)
    Validates: Inter-module compatibility

    Note: Uses sage_add_infall as companion module. If not available, test is skipped
          with a warning (not a failure). This handles cases where modules have
          been archived or are not compiled.
    """
    print("Testing multi-module pipeline...")

    # ===== CHECK MODULE AVAILABILITY =====
    available_modules = get_available_modules()

    # Prefer sage_add_infall as companion module (downstream consumer)
    companion_module = None
    companion_init_msg = None

    if "sage_add_infall" in available_modules:
        companion_module = "sage_add_infall"
        companion_init_msg = "SAGE add infall module initialized"

    # If no companion module available, skip test with warning
    if not companion_module:
        print(f"{YELLOW}⚠ SKIP: No companion module available for multi-module test{NC}")
        print(f"  Available modules: {', '.join(sorted(available_modules))}")
        print(f"  Test requires: sage_add_infall")
        print(f"  This is not a failure - modules may be archived or not compiled")
        return

    # ===== SETUP =====
    param_file, output_dir, temp_dir = create_test_param_file(
        output_name="sage_infall_multi",
        phase_config={
            'pre_timestep': [('sage_reionization', 'once'), ('sage_infall', 'once')],
            'phase_1': [('sage_add_infall', 'once')],
            'phase_2': [],
            'post_timestep': []
        },
        model_params={'GlobalBaryonFraction': 0.17}
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

    # Cleanup
    shutil.rmtree(temp_dir)

    print(f"  ✓ Multi-module pipeline works (tested with {companion_module})")


def main():
    """
    Main test runner

    Executes all test cases and reports results.
    Can be run directly or via pytest.
    """
    print(f"{BLUE}{'=' * 60}{NC}")
    print(f"{BLUE}Test Suite: SAGE Infall Integration Tests{NC}")
    print(f"{BLUE}{'=' * 60}{NC}")

    # Check prerequisites
    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found: {MIMIC_EXE}{NC}")
        print("Build it first with: make")
        return 1

    tests = [
        test_module_loads,
        test_output_properties_exist,
        test_parameters_configurable,
        test_with_satellite_stripping,
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
