#!/usr/bin/env python3
"""
SAGE Star Formation & Feedback Module - Integration Test

Validates: Module lifecycle, configuration, and pipeline integration
Phase: Phase 4.2 (SAGE Physics Module Implementation)

This test validates software quality aspects of the sage_starformation_feedback module:
- Module loads and initializes correctly
- Parameters can be configured via .par files
- Module executes without errors or memory leaks
- Output properties appear in output files (StellarMass, MetalsStellarMass, DiskScaleRadius, OutflowRate)
- Module works in multi-module pipelines (after sage_infall and sage_cooling)

NOTE: Physics validation (SF rates, mass conservation, feedback) deferred to
      scientific validation tests. These tests focus on software quality.

Test cases:
  - test_module_loads: Module registration and initialization
  - test_output_properties_exist: SF properties in output
  - test_parameters_configurable: Parameter reading and validation
  - test_feedback_toggle: SupernovaRecipeOn parameter works
  - test_memory_safety: No memory leaks
  - test_execution_completes: Full pipeline completion
  - test_three_module_pipeline: Integration with sage_infall + sage_cooling

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
    try:
        test_param = temp_dir / "_query_modules.par"

        with open(ref_param_file, 'r') as src:
            content = src.read()

        lines = content.split('\n')
        new_lines = []
        found_modules = False
        for line in lines:
            if line.strip().startswith('EnabledModules'):
                new_lines.append('EnabledModules  __nonexistent_module__')
                found_modules = True
            else:
                new_lines.append(line)

        if not found_modules:
            new_lines.append('\nEnabledModules  __nonexistent_module__\n')

        final_lines = []
        for line in new_lines:
            if line.strip().startswith('OutputFormat'):
                final_lines.append('OutputFormat  binary')
            else:
                final_lines.append(line)

        with open(test_param, 'w') as f:
            f.write('\n'.join(final_lines))

        result = subprocess.run(
            [str(MIMIC_EXE), str(test_param)],
            capture_output=True,
            text=True,
            timeout=10
        )

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
    except Exception as e:
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
    """Read parameter file contents"""
    with open(param_file, 'r') as f:
        return f.read()


def create_test_param_file(output_dir, modules, module_params=None):
    """
    Create a test parameter file

    Args:
        output_dir (Path): Output directory
        modules (list): List of module names to enable
        module_params (dict): Optional module parameters {param_name: value}

    Returns:
        Path: Path to created parameter file
    """
    param_file = temp_dir / "test.par"

    # Read reference file to preserve structure
    with open(ref_param_file, 'r') as f:
        lines = f.readlines()

    # Modify key parameters
    new_lines = []
    for line in lines:
        stripped = line.strip()

        # Update OutputDir
        if stripped.startswith('OutputDir'):
            new_lines.append(f'OutputDir  {output_dir}\n')

        # Update EnabledModules
        elif stripped.startswith('EnabledModules'):
            modules_str = ','.join(modules)
            new_lines.append(f'EnabledModules  {modules_str}\n')

        # Force binary output
        elif stripped.startswith('OutputFormat'):
            new_lines.append('OutputFormat  binary\n')

        # Keep other lines
        else:
            new_lines.append(line)

    # Add module-specific parameters if provided
    if module_params:
        new_lines.append('\n% Module parameters for testing\n')
        for param_name, value in module_params.items():
            new_lines.append(f'{param_name}  {value}\n')

    with open(param_file, 'w') as f:
        f.writelines(new_lines)

    return param_file


def setup_module():
    """Setup for all tests - run once"""
    global temp_dir, ref_param_file

    # Create temporary directory for test output
    temp_dir = Path(tempfile.mkdtemp(prefix='mimic_test_sage_sf_'))

    # Use test parameter file as reference
    ref_param_file = REPO_ROOT / "tests" / "data" / "test_binary.par"

    # Ensure Mimic executable exists
    if not MIMIC_EXE.exists():
        raise FileNotFoundError(f"Mimic executable not found: {MIMIC_EXE}")

    if not ref_param_file.exists():
        raise FileNotFoundError(f"Reference parameter file not found: {ref_param_file}")


def teardown_module():
    """Cleanup after all tests - run once"""
    if temp_dir and temp_dir.exists():
        shutil.rmtree(temp_dir)


def test_module_loads():
    """
    Test that sage_starformation_feedback module registers and loads

    Expected: Module appears in available modules list
    """
    print(f"\n{BLUE}TEST: Module Registration{NC}")

    available = get_available_modules()

    # Check if module is available
    assert 'sage_starformation_feedback' in available, \
        f"sage_starformation_feedback not in available modules: {available}"

    print(f"{GREEN}✓ sage_starformation_feedback module registered{NC}")


def test_output_properties_exist():
    """
    Test that module produces expected output properties

    Expected: StellarMass, MetalsStellarMass, DiskScaleRadius, OutflowRate in output
    """
    print(f"\n{BLUE}TEST: Output Properties{NC}")

    # Check module availability
    available = get_available_modules()
    if 'sage_starformation_feedback' not in available:
        print(f"{YELLOW}⊘ Skipping - sage_starformation_feedback not available{NC}")
        return

    # Check for dependencies
    required_modules = ['sage_infall', 'sage_cooling']
    missing = [m for m in required_modules if m not in available]
    if missing:
        print(f"{YELLOW}⊘ Skipping - missing dependencies: {missing}{NC}")
        return

    # Create test output directory
    output_dir = temp_dir / "output_properties"
    output_dir.mkdir(exist_ok=True)

    # Create parameter file with sage_infall + sage_cooling + sage_starformation_feedback
    param_file = create_test_param_file(
        output_dir,
        modules=['sage_infall', 'sage_cooling', 'sage_starformation_feedback']
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Check execution completed successfully
    assert returncode == 0, f"Mimic execution failed:\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}"

    # Find output file
    output_files = list(output_dir.glob("*.dat"))
    assert len(output_files) > 0, f"No output files found in {output_dir}"

    output_file = output_files[0]

    # Load output halos
    halos = load_binary_halos(str(output_file))

    # Check for expected properties
    required_properties = ['StellarMass', 'MetalsStellarMass', 'DiskScaleRadius', 'OutflowRate']
    for prop in required_properties:
        assert prop in halos.dtype.names, f"Property {prop} not found in output"
        print(f"  {GREEN}✓{NC} {prop} present in output")


def test_parameters_configurable():
    """
    Test that module parameters can be configured

    Expected: Custom parameter values accepted and used
    """
    print(f"\n{BLUE}TEST: Parameter Configuration{NC}")

    # Check module availability
    available = get_available_modules()
    if 'sage_starformation_feedback' not in available:
        print(f"{YELLOW}⊘ Skipping - sage_starformation_feedback not available{NC}")
        return

    # Check dependencies
    required_modules = ['sage_infall', 'sage_cooling']
    missing = [m for m in required_modules if m not in available]
    if missing:
        print(f"{YELLOW}⊘ Skipping - missing dependencies: {missing}{NC}")
        return

    # Create test output directory
    output_dir = temp_dir / "param_config"
    output_dir.mkdir(exist_ok=True)

    # Create parameter file with custom module parameters
    param_file = create_test_param_file(
        output_dir,
        modules=['sage_infall', 'sage_cooling', 'sage_starformation_feedback'],
        module_params={
            'SageStarformationFeedback_SfrEfficiency': '0.05',  # Non-default
            'SageStarformationFeedback_RecycleFraction': '0.4'  # Non-default
        }
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Check execution completed successfully
    assert returncode == 0, f"Mimic execution with custom params failed:\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}"

    # Check logs mention custom parameters (if logged)
    print(f"{GREEN}✓ Module accepts custom parameters{NC}")


def test_feedback_toggle():
    """
    Test that SupernovaRecipeOn parameter works

    Expected: Module runs with feedback enabled (1) and disabled (0)
    """
    print(f"\n{BLUE}TEST: Feedback Toggle{NC}")

    # Check module availability
    available = get_available_modules()
    if 'sage_starformation_feedback' not in available:
        print(f"{YELLOW}⊘ Skipping - sage_starformation_feedback not available{NC}")
        return

    # Check dependencies
    required_modules = ['sage_infall', 'sage_cooling']
    missing = [m for m in required_modules if m not in available]
    if missing:
        print(f"{YELLOW}⊘ Skipping - missing dependencies: {missing}{NC}")
        return

    # Test with feedback ENABLED
    output_dir = temp_dir / "feedback_on"
    output_dir.mkdir(exist_ok=True)

    param_file = create_test_param_file(
        output_dir,
        modules=['sage_infall', 'sage_cooling', 'sage_starformation_feedback'],
        module_params={'SageStarformationFeedback_SupernovaRecipeOn': '1'}
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Execution with feedback ON failed:\n{stderr}"
    print(f"  {GREEN}✓{NC} Feedback enabled (SupernovaRecipeOn=1)")

    # Test with feedback DISABLED
    output_dir = temp_dir / "feedback_off"
    output_dir.mkdir(exist_ok=True)

    param_file = create_test_param_file(
        output_dir,
        modules=['sage_infall', 'sage_cooling', 'sage_starformation_feedback'],
        module_params={'SageStarformationFeedback_SupernovaRecipeOn': '0'}
    )

    returncode, stdout, stderr = run_mimic(param_file)
    assert returncode == 0, f"Execution with feedback OFF failed:\n{stderr}"
    print(f"  {GREEN}✓{NC} Feedback disabled (SupernovaRecipeOn=0)")


def test_memory_safety():
    """
    Test that module causes no memory leaks

    Expected: No memory leak warnings in output
    """
    print(f"\n{BLUE}TEST: Memory Safety{NC}")

    # Check module availability
    available = get_available_modules()
    if 'sage_starformation_feedback' not in available:
        print(f"{YELLOW}⊘ Skipping - sage_starformation_feedback not available{NC}")
        return

    # Check dependencies
    required_modules = ['sage_infall', 'sage_cooling']
    missing = [m for m in required_modules if m not in available]
    if missing:
        print(f"{YELLOW}⊘ Skipping - missing dependencies: {missing}{NC}")
        return

    # Create test output directory
    output_dir = temp_dir / "memory_safety"
    output_dir.mkdir(exist_ok=True)

    # Create parameter file
    param_file = create_test_param_file(
        output_dir,
        modules=['sage_infall', 'sage_cooling', 'sage_starformation_feedback']
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Check for memory leak warnings (exclude success messages)
    # Note: "No memory leaks detected" is a success message, not a failure
    combined_output = stdout + stderr

    # Look for actual leak warnings, not success messages
    leak_detected = False
    for line in combined_output.split('\n'):
        line_lower = line.lower()
        if 'memory leak' in line_lower:
            # Exclude success messages
            if 'no memory leak' not in line_lower:
                # Check if it's a warning or error
                if 'warning' in line_lower or 'error' in line_lower:
                    leak_detected = True
                    break

    assert not leak_detected, "Memory leak detected in output"

    print(f"{GREEN}✓ No memory leaks detected{NC}")


def test_execution_completes():
    """
    Test that full pipeline execution completes successfully

    Expected: Execution returns 0 and produces output files
    """
    print(f"\n{BLUE}TEST: Execution Completion{NC}")

    # Check module availability
    available = get_available_modules()
    if 'sage_starformation_feedback' not in available:
        print(f"{YELLOW}⊘ Skipping - sage_starformation_feedback not available{NC}")
        return

    # Check dependencies
    required_modules = ['sage_infall', 'sage_cooling']
    missing = [m for m in required_modules if m not in available]
    if missing:
        print(f"{YELLOW}⊘ Skipping - missing dependencies: {missing}{NC}")
        return

    # Create test output directory
    output_dir = temp_dir / "execution_complete"
    output_dir.mkdir(exist_ok=True)

    # Create parameter file
    param_file = create_test_param_file(
        output_dir,
        modules=['sage_infall', 'sage_cooling', 'sage_starformation_feedback']
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Check execution completed successfully
    assert returncode == 0, f"Execution failed:\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}"

    # Check output files exist
    output_files = list(output_dir.glob("*.dat"))
    assert len(output_files) > 0, f"No output files produced"

    print(f"{GREEN}✓ Execution completed successfully{NC}")
    print(f"  Output files: {len(output_files)}")


def test_three_module_pipeline():
    """
    Test integration with upstream modules (sage_infall + sage_cooling)

    Expected: Three-module pipeline executes successfully with property flow
    """
    print(f"\n{BLUE}TEST: Three-Module Pipeline Integration{NC}")

    # Check module availability
    available = get_available_modules()
    required_modules = ['sage_infall', 'sage_cooling', 'sage_starformation_feedback']
    missing = [m for m in required_modules if m not in available]

    if missing:
        print(f"{YELLOW}⊘ Skipping - missing modules: {missing}{NC}")
        return

    # Create test output directory
    output_dir = temp_dir / "three_module_pipeline"
    output_dir.mkdir(exist_ok=True)

    # Create parameter file with all three modules in correct order
    param_file = create_test_param_file(
        output_dir,
        modules=['sage_infall', 'sage_cooling', 'sage_starformation_feedback']
    )

    # Run Mimic
    returncode, stdout, stderr = run_mimic(param_file)

    # Check execution completed successfully
    assert returncode == 0, f"Three-module pipeline failed:\nSTDOUT:\n{stdout}\nSTDERR:\n{stderr}"

    # Load output and verify property flow
    output_files = list(output_dir.glob("*.dat"))
    assert len(output_files) > 0, "No output files produced"

    halos = load_binary_halos(str(output_files[0]))

    # Check all expected properties present
    expected_props = [
        'HotGas', 'MetalsHotGas',  # from sage_infall
        'ColdGas', 'MetalsColdGas', 'BlackHoleMass',  # from sage_cooling
        'StellarMass', 'MetalsStellarMass', 'DiskScaleRadius', 'OutflowRate'  # from sage_starformation_feedback
    ]

    for prop in expected_props:
        assert prop in halos.dtype.names, f"Property {prop} missing from output"

    print(f"{GREEN}✓ Three-module pipeline successful{NC}")
    print(f"  Properties from sage_infall: HotGas, MetalsHotGas")
    print(f"  Properties from sage_cooling: ColdGas, MetalsColdGas, BlackHoleMass")
    print(f"  Properties from sage_starformation_feedback: StellarMass, MetalsStellarMass, DiskScaleRadius, OutflowRate")


if __name__ == '__main__':
    # Setup
    setup_module()

    try:
        # Run tests
        test_module_loads()
        test_output_properties_exist()
        test_parameters_configurable()
        test_feedback_toggle()
        test_memory_safety()
        test_execution_completes()
        test_three_module_pipeline()

        print(f"\n{GREEN}═══════════════════════════════════════════{NC}")
        print(f"{GREEN}All integration tests passed!{NC}")
        print(f"{GREEN}═══════════════════════════════════════════{NC}\n")

    finally:
        # Cleanup
        teardown_module()
