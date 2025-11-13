"""
Test Harness Utilities for Mimic

Centralized test utilities for integration and scientific testing.
Eliminates code duplication across test files.

Phase: Phase 4.2 (Testing Framework Refinement)
Author: Mimic Testing Team
Date: 2025-11-13
"""

import os
import subprocess
import tempfile
from pathlib import Path


# Repository paths
REPO_ROOT = Path(__file__).parent.parent.parent
TEST_DATA_DIR = REPO_ROOT / "tests" / "data"
MIMIC_EXE = REPO_ROOT / "mimic"


def ensure_output_dirs():
    """
    Create output directories if they don't exist

    Creates the binary and HDF5 output directories required by test parameter files.
    This ensures tests work correctly after make test-clean or in fresh clones.

    Usage:
        ensure_output_dirs()  # Call once at module level or in setUpClass
    """
    (TEST_DATA_DIR / "output" / "binary").mkdir(parents=True, exist_ok=True)
    (TEST_DATA_DIR / "output" / "hdf5").mkdir(parents=True, exist_ok=True)


def run_mimic(param_file, cwd=None):
    """
    Execute Mimic with specified parameter file

    Args:
        param_file (str or Path): Path to parameter file
        cwd (str or Path): Working directory for execution (default: repo root)

    Returns:
        tuple: (returncode, stdout, stderr)

    Raises:
        FileNotFoundError: If Mimic executable not found

    Usage:
        returncode, stdout, stderr = run_mimic("input/millennium.par")
        assert returncode == 0, f"Mimic failed: {stderr}"
    """
    if cwd is None:
        cwd = REPO_ROOT

    if not MIMIC_EXE.exists():
        raise FileNotFoundError(
            f"Mimic executable not found at {MIMIC_EXE}. "
            f"Build it first with: make"
        )

    result = subprocess.run(
        [str(MIMIC_EXE), str(param_file)],
        cwd=str(cwd),
        capture_output=True,
        text=True
    )

    return result.returncode, result.stdout, result.stderr


def read_param_file(param_file):
    """
    Read parameter file and return as dictionary

    Parses a Mimic parameter file (.par) and returns key-value pairs.
    Handles comments, empty lines, and arrow notation (->).

    Args:
        param_file (str or Path): Path to parameter file

    Returns:
        dict: Parameter name -> value mapping

    Usage:
        params = read_param_file("input/millennium.par")
        output_dir = params['OutputDir']
        hubble_h = float(params['Hubble_h'])
    """
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
                            module_params=None, first_file=0, last_file=0,
                            ref_param_file=None, temp_dir=None):
    """
    Create a test parameter file with specified module configuration

    Generates a parameter file for testing, based on a reference parameter file
    with custom module configuration and file range.

    Args:
        output_name (str): Name for output directory (created in temp_dir)
        enabled_modules (list): List of module names to enable (None = physics-free)
        module_params (dict): Dict of {ModuleName_ParameterName: value}
        first_file (int): First file to process (default: 0)
        last_file (int): Last file to process (default: 0)
        ref_param_file (str or Path): Reference parameter file (default: millennium.par)
        temp_dir (str or Path): Temporary directory for outputs (default: create new)

    Returns:
        tuple: (param_file_path, output_dir_path, temp_dir_path)
               - param_file_path: Path to created parameter file
               - output_dir_path: Path to output directory
               - temp_dir_path: Path to temporary directory (for cleanup)

    Usage:
        # Physics-free mode
        param_file, output_dir, temp_dir = create_test_param_file("test_run")

        # With modules
        param_file, output_dir, temp_dir = create_test_param_file(
            output_name="cooling_test",
            enabled_modules=["sage_infall", "sage_cooling"],
            module_params={
                "SageInfall_BaryonFrac": "0.17",
                "SageCooling_CoolFunctionsDir": "input/CoolFunctions"
            },
            first_file=0,
            last_file=0
        )

        # Cleanup when done
        import shutil
        shutil.rmtree(temp_dir)
    """
    # Set defaults
    if ref_param_file is None:
        ref_param_file = REPO_ROOT / "input" / "millennium.par"
    if temp_dir is None:
        temp_dir = tempfile.mkdtemp(prefix="mimic_test_")
    else:
        temp_dir = Path(temp_dir)

    # Read reference parameter file
    ref_params = read_param_file(ref_param_file)

    # Create output directory
    output_dir = Path(temp_dir) / output_name
    output_dir.mkdir(parents=True, exist_ok=True)

    # Update parameters
    ref_params['OutputDir'] = str(output_dir)
    ref_params['FirstFile'] = str(first_file)
    ref_params['LastFile'] = str(last_file)

    # Write test parameter file
    param_path = Path(temp_dir) / f"{output_name}.par"
    with open(param_path, 'w') as f:
        f.write("%------------------------------------------\n")
        f.write("%----- Test Configuration -----------------\n")
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
        else:
            f.write("# Physics-free mode (no modules enabled)\n")
            f.write("EnabledModules\n")

    return param_path, output_dir, Path(temp_dir)


def check_no_memory_leaks(output_dir):
    """
    Check that Mimic run had no memory leaks

    Scans log files in output directory for memory leak indicators.

    Args:
        output_dir (Path): Output directory containing metadata/logs

    Returns:
        bool: True if no leaks, False if leaks detected

    Usage:
        output_dir = Path("tests/data/output/binary")
        has_leaks = not check_no_memory_leaks(output_dir)
        assert not has_leaks, "Memory leaks detected"
    """
    # ANSI color codes
    YELLOW = '\033[1;33m'
    RED = '\033[0;31m'
    NC = '\033[0m'  # No Color

    log_dir = output_dir / "metadata"
    if not log_dir.exists():
        print(f"{YELLOW}Warning: Log directory not found: {log_dir}{NC}")
        return True  # Can't check, assume OK

    for log_file in log_dir.glob("*.log"):
        with open(log_file) as f:
            content = f.read().lower()
            if "memory leak" in content:
                print(f"{RED}Memory leak detected in {log_file}{NC}")
                # Print the relevant lines
                with open(log_file) as f2:
                    for line in f2:
                        if "memory leak" in line.lower():
                            print(f"  {line.strip()}")
                return False

    return True


# Convenience exports for common paths
__all__ = [
    'REPO_ROOT',
    'TEST_DATA_DIR',
    'MIMIC_EXE',
    'ensure_output_dirs',
    'run_mimic',
    'read_param_file',
    'create_test_param_file',
    'check_no_memory_leaks',
]
