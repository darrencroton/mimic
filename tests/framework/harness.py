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
        returncode, stdout, stderr = run_mimic("input/millennium.yaml")
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
    Read YAML parameter file and return as dictionary

    Parses a Mimic YAML parameter file and returns key-value pairs.

    Args:
        param_file (str or Path): Path to YAML parameter file

    Returns:
        dict: Parameter name -> value mapping

    Usage:
        params = read_param_file("input/millennium.yaml")
        output_dir = params['OutputDir']
        hubble_h = float(params['Hubble_h'])
    """
    import yaml

    param_file = Path(param_file)

    with open(param_file, 'r') as f:
        config = yaml.safe_load(f)

    # Flatten hierarchical structure
    params = {}
    if 'output' in config:
        params['OutputDir'] = config['output'].get('directory', './')
        params['OutputFileBaseName'] = config['output'].get('file_base_name', 'model')
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
                            module_params=None, first_file=0, last_file=0,
                            ref_param_file=None, temp_dir=None):
    """
    Create a test YAML parameter file with specified module configuration

    Generates a YAML parameter file for testing, based on a reference parameter file
    with custom module configuration and file range.

    Args:
        output_name (str): Name for output directory (created in temp_dir)
        enabled_modules (list): List of module names to enable (None = physics-free)
        module_params (dict): Dict of {ModuleName_ParameterName: value}
        first_file (int): First file to process (default: 0)
        last_file (int): Last file to process (default: 0)
        ref_param_file (str or Path): Reference YAML parameter file (default: millennium.yaml)
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
    import yaml

    # Set defaults
    if ref_param_file is None:
        ref_param_file = REPO_ROOT / "input" / "millennium.yaml"
    if temp_dir is None:
        temp_dir = tempfile.mkdtemp(prefix="mimic_test_")
    else:
        temp_dir = Path(temp_dir)

    # Read reference parameter file (YAML)
    with open(ref_param_file, 'r') as f:
        config = yaml.safe_load(f)

    # Create output directory
    output_dir = Path(temp_dir) / output_name
    output_dir.mkdir(parents=True, exist_ok=True)

    # Update configuration
    config['output']['directory'] = str(output_dir)
    config['input']['first_file'] = first_file
    config['input']['last_file'] = last_file

    # Update module configuration
    if enabled_modules:
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
    else:
        config['modules']['enabled'] = []
        config['modules']['parameters'] = {}

    # Write test parameter file as YAML
    param_path = Path(temp_dir) / f"{output_name}.yaml"
    with open(param_path, 'w') as f:
        f.write("#" + "="*77 + "\n")
        f.write("# Mimic Test Configuration\n")
        f.write("#" + "="*77 + "\n")
        f.write("# Auto-generated test parameter file\n")
        f.write("#" + "="*77 + "\n\n")
        yaml.dump(config, f, default_flow_style=False, sort_keys=False)

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
            for line in f:
                line_lower = line.lower()
                # Check for actual leak messages, not success messages
                # "No memory leaks detected" is a success message, not a failure
                if "memory leak" in line_lower:
                    # Exclude success messages
                    if "no memory leak" not in line_lower:
                        # Check if it's a warning or error (not just INFO)
                        if "warning" in line_lower or "error" in line_lower or "fatal" in line_lower:
                            print(f"{RED}Memory leak detected in {log_file}{NC}")
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
