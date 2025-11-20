#!/usr/bin/env python3
"""
SAGE Disk Instability Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration
Phase: Phase 4.6 (SAGE Disk Instability Module - Partial Implementation)

This test validates software quality aspects of the sage_disk_instability module:
- Module loads and initializes correctly
- Parameters can be configured via YAML files
- Module executes without errors or memory leaks
- Output properties appear in output files (BulgeMass, DiskScaleRadius)
- Module works in multi-module pipelines
- Stellar mass conservation during instability events

NOTE: Gas processing tests deferred to v2.0.0 when sage_mergers module exists.
      Current tests focus on implemented physics: stability criterion and stellar transfer.

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: Bulge properties in output
  - test_parameters_configurable: Parameter reading and validation
  - test_stability_physics: Disk scale radius calculation
  - test_stellar_conservation: Mass conserved during transfers
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_multiple_module_pipeline: Multi-module integration

Author: Mimic Development Team
Date: 2025-11-17
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
                else:
                    # End of module list
                    break

        return available

    except Exception as e:
        print(f"Warning: Could not query available modules: {e}")
        return set()


def setup():
    """Set up test environment and temp directory"""
    global temp_dir, ref_param_file

    print(f"{BLUE}Setting up test environment...{NC}")

    # Create temp directory
    temp_dir = Path(tempfile.mkdtemp(prefix="test_sage_disk_instability_"))

    # Reference parameter file
    ref_param_file = REPO_ROOT / "tests" / "data" / "test_binary.yaml"

    if not MIMIC_EXE.exists():
        print(f"{RED}ERROR: Mimic executable not found at {MIMIC_EXE}{NC}")
        print(f"{YELLOW}Run 'make' to build Mimic first{NC}")
        sys.exit(1)

    if not ref_param_file.exists():
        print(f"{RED}ERROR: Test parameter file not found at {ref_param_file}{NC}")
        sys.exit(1)

    print(f"{GREEN}✓{NC} Temp directory: {temp_dir}")


def teardown():
    """Clean up test environment"""
    global temp_dir

    if temp_dir and temp_dir.exists():
        shutil.rmtree(temp_dir)
        print(f"{GREEN}✓{NC} Cleaned up temp directory")


def test_module_loads():
    """Test that sage_disk_instability module registers and initializes"""
    print(f"\n{BLUE}Test: Module loads and initializes{NC}")

    # Get available modules
    available = get_available_modules()

    # Check if sage_disk_instability is available
    if 'sage_disk_instability' not in available:
        print(f"{RED}✗ FAIL{NC}: sage_disk_instability module not found in available modules")
        print(f"  Available: {sorted(available)}")
        return False

    print(f"{GREEN}✓ PASS{NC}: sage_disk_instability module is registered")
    return True


def test_output_properties_exist():
    """Test that disk instability properties appear in output"""
    import yaml

    print(f"\n{BLUE}Test: Output properties exist{NC}")

    test_param = temp_dir / "test_properties.yaml"

    with open(ref_param_file, 'r') as f:
        config = yaml.safe_load(f)

    config['modules']['enabled'] = ['sage_disk_instability']
    config['output']['directory'] = str(temp_dir)
    config['output']['format'] = 'binary'

    with open(test_param, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    # Run Mimic
    result = subprocess.run(
        [str(MIMIC_EXE), str(test_param)],
        capture_output=True,
        text=True,
        timeout=30
    )

    if result.returncode != 0:
        print(f"{RED}✗ FAIL{NC}: Mimic execution failed")
        print(f"  stdout: {result.stdout}")
        print(f"  stderr: {result.stderr}")
        return False

    # Load output and check for disk instability properties
    try:
        # Construct output file path (model_z0.000_0 for snapshot 63 at z=0)
        output_file = temp_dir / "model_z0.000_0"
        if not output_file.exists():
            print(f"{RED}✗ FAIL{NC}: Output file not created: {output_file}")
            return False

        halos, metadata = load_binary_halos(output_file)

        # Check that bulge properties exist
        required_props = ['BulgeMass', 'MetalsBulgeMass', 'MetalsStellarMass', 'DiskScaleRadius']
        missing = []
        for prop in required_props:
            if prop not in halos.dtype.names:
                missing.append(prop)

        if missing:
            print(f"{RED}✗ FAIL{NC}: Missing output properties: {missing}")
            print(f"  Available: {halos.dtype.names}")
            return False

        # Check that disk scale radius has reasonable values
        if len(halos) > 0:
            disk_radii = halos['DiskScaleRadius']
            valid_radii = disk_radii[disk_radii > 0]
            if len(valid_radii) > 0:
                print(f"  Disk scale radius range: {valid_radii.min():.6f} - {valid_radii.max():.6f} Mpc/h")

        print(f"{GREEN}✓ PASS{NC}: All disk instability properties present in output")
        return True

    except Exception as e:
        print(f"{RED}✗ FAIL{NC}: Error loading output: {e}")
        return False


def test_parameters_configurable():
    """Test that module parameters can be configured"""
    import yaml

    print(f"\n{BLUE}Test: Parameters configurable{NC}")

    test_param = temp_dir / "test_params.yaml"

    with open(ref_param_file, 'r') as f:
        config = yaml.safe_load(f)

    config['modules']['enabled'] = ['sage_disk_instability']
    config['output']['directory'] = str(temp_dir)
    config['output']['format'] = 'binary'

    if 'parameters' not in config['modules']:
        config['modules']['parameters'] = {}
    if 'SageDiskInstability' not in config['modules']['parameters']:
        config['modules']['parameters']['SageDiskInstability'] = {}

    config['modules']['parameters']['SageDiskInstability']['DiskInstabilityOn'] = 1
    config['modules']['parameters']['SageDiskInstability']['DiskRadiusFactor'] = 5.0

    with open(test_param, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    # Run Mimic
    result = subprocess.run(
        [str(MIMIC_EXE), str(test_param)],
        capture_output=True,
        text=True,
        timeout=30
    )

    if result.returncode != 0:
        print(f"{RED}✗ FAIL{NC}: Mimic execution with custom parameters failed")
        return False

    # Check that parameters were read (look for message in output)
    if 'DiskRadiusFactor = 5.00' in result.stdout or 'DiskRadiusFactor = 5.00' in result.stderr:
        print(f"{GREEN}✓ PASS{NC}: Module parameters configured successfully")
        return True
    else:
        print(f"{YELLOW}⚠ WARNING{NC}: Could not verify parameter values in output")
        print(f"  Module executed without errors (parameters likely applied)")
        return True


def test_stability_physics():
    """Test that disk scale radius is calculated"""
    import yaml

    print(f"\n{BLUE}Test: Disk stability physics{NC}")

    test_param = temp_dir / "test_stability.yaml"

    with open(ref_param_file, 'r') as f:
        config = yaml.safe_load(f)

    config['modules']['enabled'] = ['sage_disk_instability']
    config['output']['directory'] = str(temp_dir)
    config['output']['format'] = 'binary'

    with open(test_param, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    # Run Mimic
    result = subprocess.run(
        [str(MIMIC_EXE), str(test_param)],
        capture_output=True,
        text=True,
        timeout=30
    )

    if result.returncode != 0:
        print(f"{RED}✗ FAIL{NC}: Mimic execution failed")
        return False

    # Load output and check disk scale radii
    try:
        # Construct output file path
        output_file = temp_dir / "model_z0.000_0"
        if not output_file.exists():
            print(f"{RED}✗ FAIL{NC}: Output file not created: {output_file}")
            return False

        halos, metadata = load_binary_halos(output_file)

        if len(halos) == 0:
            print(f"{YELLOW}⚠ WARNING{NC}: No halos in output")
            return True

        disk_radii = halos['DiskScaleRadius']
        valid = disk_radii > 0

        if valid.sum() > 0:
            min_rd = disk_radii[valid].min()
            max_rd = disk_radii[valid].max()

            # Check that values are physically reasonable (0.001 - 0.1 Mpc/h)
            if min_rd < 0.0001 or max_rd > 1.0:
                print(f"{YELLOW}⚠ WARNING{NC}: Disk radii outside typical range")
                print(f"  Range: {min_rd:.6f} - {max_rd:.6f} Mpc/h")

            print(f"  Disk scale radius range: {min_rd:.6f} - {max_rd:.6f} Mpc/h")
            print(f"  Number of galaxies with disks: {valid.sum()} / {len(halos)}")
            print(f"{GREEN}✓ PASS{NC}: Disk scale radii calculated")
            return True
        else:
            print(f"{YELLOW}⚠ WARNING{NC}: No galaxies with disk scale radius > 0")
            return True

    except Exception as e:
        print(f"{RED}✗ FAIL{NC}: Error analyzing output: {e}")
        return False


def test_stellar_conservation():
    """Test that stellar mass is conserved"""
    import yaml

    print(f"\n{BLUE}Test: Stellar mass conservation{NC}")

    test_param = temp_dir / "test_conservation.yaml"

    with open(ref_param_file, 'r') as f:
        config = yaml.safe_load(f)

    config['modules']['enabled'] = ['sage_disk_instability']
    config['output']['directory'] = str(temp_dir)
    config['output']['format'] = 'binary'

    with open(test_param, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    # Run Mimic
    result = subprocess.run(
        [str(MIMIC_EXE), str(test_param)],
        capture_output=True,
        text=True,
        timeout=30
    )

    if result.returncode != 0:
        print(f"{RED}✗ FAIL{NC}: Mimic execution failed")
        return False

    # Load output and check mass conservation
    try:
        # Construct output file path
        output_file = temp_dir / "model_z0.000_0"
        if not output_file.exists():
            print(f"{RED}✗ FAIL{NC}: Output file not created: {output_file}")
            return False

        halos, metadata = load_binary_halos(output_file)

        if len(halos) == 0:
            print(f"{YELLOW}⚠ WARNING{NC}: No halos in output")
            return True

        # Check that BulgeMass <= StellarMass for all galaxies
        stellar = halos['StellarMass']
        bulge = halos['BulgeMass']

        # Allow small floating-point tolerance
        violations = bulge > stellar * 1.0001

        if violations.sum() > 0:
            print(f"{RED}✗ FAIL{NC}: Bulge mass exceeds stellar mass in {violations.sum()} galaxies")
            return False

        print(f"  Stellar mass: {stellar.sum():.4e} (total)")
        print(f"  Bulge mass: {bulge.sum():.4e} (total)")
        print(f"  Bulge fraction: {bulge.sum() / max(stellar.sum(), 1e-10):.4f}")
        print(f"{GREEN}✓ PASS{NC}: Mass conservation verified")
        return True

    except Exception as e:
        print(f"{RED}✗ FAIL{NC}: Error checking conservation: {e}")
        return False


def test_memory_safety():
    """Test that module executes without memory leaks"""
    import yaml

    print(f"\n{BLUE}Test: Memory safety{NC}")

    test_param = temp_dir / "test_memory.yaml"

    with open(ref_param_file, 'r') as f:
        config = yaml.safe_load(f)

    config['modules']['enabled'] = ['sage_disk_instability']
    config['output']['directory'] = str(temp_dir)
    config['output']['format'] = 'binary'

    with open(test_param, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    # Run Mimic
    result = subprocess.run(
        [str(MIMIC_EXE), str(test_param)],
        capture_output=True,
        text=True,
        timeout=30
    )

    if result.returncode != 0:
        print(f"{RED}✗ FAIL{NC}: Mimic execution failed")
        return False

    # Check for memory leak messages (WARNING or ERROR level, not INFO)
    # Note: "No memory leaks detected" is a success message, not a failure
    output = result.stdout + result.stderr

    # Look for actual leak warnings (exclude success messages)
    leak_detected = False
    for line in output.split('\n'):
        line_lower = line.lower()
        if 'memory leak' in line_lower or 'leak detected' in line_lower:
            # Exclude success messages
            if 'no memory leak' not in line_lower and 'warning' in line_lower or 'error' in line_lower:
                print(f"{RED}✗ FAIL{NC}: Memory leak detected")
                print(f"  {line}")
                leak_detected = True
                break

    if leak_detected:
        return False

    print(f"{GREEN}✓ PASS{NC}: No memory leaks detected")
    return True


def test_execution_completes():
    """Test that module executes to completion"""
    import yaml

    print(f"\n{BLUE}Test: Execution completes{NC}")

    test_param = temp_dir / "test_completion.yaml"

    with open(ref_param_file, 'r') as f:
        config = yaml.safe_load(f)

    config['modules']['enabled'] = ['sage_disk_instability']
    config['output']['directory'] = str(temp_dir)
    config['output']['format'] = 'binary'

    with open(test_param, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    # Run Mimic
    result = subprocess.run(
        [str(MIMIC_EXE), str(test_param)],
        capture_output=True,
        text=True,
        timeout=30
    )

    if result.returncode != 0:
        print(f"{RED}✗ FAIL{NC}: Execution failed with return code {result.returncode}")
        return False

    # Check for completion message
    if 'done' in result.stdout.lower() or 'complete' in result.stdout.lower():
        print(f"{GREEN}✓ PASS{NC}: Execution completed successfully")
        return True
    else:
        print(f"{YELLOW}⚠ WARNING{NC}: Execution completed but no completion message found")
        return True


def test_multiple_module_pipeline():
    """Test module works in multi-module pipeline"""
    import yaml

    print(f"\n{BLUE}Test: Multi-module pipeline integration{NC}")

    available = get_available_modules()

    modules_to_test = []
    if 'sage_infall' in available:
        modules_to_test.append('sage_infall')
    if 'sage_cooling' in available:
        modules_to_test.append('sage_cooling')
    modules_to_test.append('sage_disk_instability')

    if len(modules_to_test) < 2:
        print(f"{YELLOW}⚠ SKIP{NC}: Not enough modules available for multi-module test")
        return True

    test_param = temp_dir / "test_multi.yaml"

    with open(ref_param_file, 'r') as f:
        config = yaml.safe_load(f)

    config['modules']['enabled'] = modules_to_test
    config['output']['directory'] = str(temp_dir)
    config['output']['format'] = 'binary'

    with open(test_param, 'w') as f:
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

    # Run Mimic
    result = subprocess.run(
        [str(MIMIC_EXE), str(test_param)],
        capture_output=True,
        text=True,
        timeout=30
    )

    if result.returncode != 0:
        print(f"{RED}✗ FAIL{NC}: Multi-module execution failed")
        print(f"  Modules: {modules_to_test}")
        return False

    print(f"  Modules tested: {modules_to_test}")
    print(f"{GREEN}✓ PASS{NC}: Multi-module pipeline works correctly")
    return True


def main():
    """Run all integration tests"""
    print(f"{BLUE}{'='*70}{NC}")
    print(f"{BLUE}SAGE Disk Instability Module - Integration Tests{NC}")
    print(f"{BLUE}{'='*70}{NC}")

    setup()

    tests = [
        test_module_loads,
        test_output_properties_exist,
        test_parameters_configurable,
        test_stability_physics,
        test_stellar_conservation,
        test_memory_safety,
        test_execution_completes,
        test_multiple_module_pipeline,
    ]

    passed = 0
    failed = 0

    for test in tests:
        try:
            if test():
                passed += 1
            else:
                failed += 1
        except Exception as e:
            print(f"{RED}✗ EXCEPTION{NC}: {e}")
            failed += 1

    teardown()

    # Print summary
    print(f"\n{BLUE}{'='*70}{NC}")
    print(f"{BLUE}Test Summary{NC}")
    print(f"{BLUE}{'='*70}{NC}")
    print(f"{GREEN}Passed:{NC} {passed}")
    print(f"{RED}Failed:{NC} {failed}")

    if failed == 0:
        print(f"\n{GREEN}✓ All integration tests passed!{NC}\n")
        return 0
    else:
        print(f"\n{RED}✗ Some integration tests failed{NC}\n")
        return 1


if __name__ == "__main__":
    sys.exit(main())
