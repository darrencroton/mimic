"""
Test Harness Utilities for Mimic

Centralized test utilities for integration and scientific testing.
Eliminates code duplication across test files.

Phase: Phase 4.2 (Testing Framework Refinement)
Author: Mimic Testing Team
Date: 2025-12-09 (Updated for multi-phase pipeline)
"""

import os
import subprocess
import tempfile
from pathlib import Path


# Repository paths
REPO_ROOT = Path(__file__).parent.parent.parent
TEST_DATA_DIR = REPO_ROOT / "tests" / "data"
MIMIC_EXE = REPO_ROOT / "mimic"


def compiled_model():
    """Return the model selected for this test run."""
    return os.environ.get("MODEL", "sage")


def model_input_file(filename):
    """Return a test input file from the selected model package."""
    path = REPO_ROOT / "models" / compiled_model() / "input" / filename
    if not path.exists():
        raise FileNotFoundError(
            f"Model-local test input not found: {path}. "
            f"Create models/{compiled_model()}/input/{filename}."
        )
    return path


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
        returncode, stdout, stderr = run_mimic(model_input_file("test_binary.yaml"))
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
        [str(MIMIC_EXE), "--verbose", str(param_file)],
        cwd=str(cwd),
        capture_output=True,
        text=True
    )

    return result.returncode, result.stdout, result.stderr


def run_mimic_fresh(param_file, expected_output=None, cwd=None):
    """
    Run Mimic, always (re)generating output for the selected model.

    Unlike a bare ``run_mimic`` call guarded by ``if not output.exists()``, this
    helper removes any pre-existing ``expected_output`` first, then runs Mimic
    and asserts a clean exit. This prevents a stale output file from a previous
    run -- possibly produced by a different ``MODEL`` writing to the same shared
    output path -- from silently satisfying a later assertion (a false positive).

    Args:
        param_file (str or Path): Run/input YAML for the selected model.
        expected_output (str or Path, optional): The output file this test will
            validate. Removed before the run so a stale file cannot survive a
            failed or skipped regeneration.
        cwd (str or Path): Working directory for execution (default: repo root).

    Returns:
        tuple: (returncode, stdout, stderr) from the Mimic run.

    Usage:
        run_mimic_fresh(model_input_file("test_binary.yaml"), output_file)
    """
    if expected_output is not None:
        expected_output = Path(expected_output)
        if expected_output.exists():
            expected_output.unlink()

    returncode, stdout, stderr = run_mimic(param_file, cwd=cwd)
    assert returncode == 0, (
        f"Mimic execution failed (rc={returncode})\n"
        f"STDOUT:\n{stdout}\nSTDERR:\n{stderr}"
    )
    return returncode, stdout, stderr


def resolve_sim_config_path(sim_config_path, param_file):
    """
    Resolve a ``simulation.config`` path identically across harness code paths.

    Absolute paths are used as-is. Relative paths are tried against the repo
    root first, then against the parameter file's parent directory. This matches
    runtime behaviour and lets model-local input YAMLs reference their
    simulation config relative to either location.

    Args:
        sim_config_path (str or Path): The ``simulation.config`` value from a run file.
        param_file (str or Path): The run file the config path was read from.

    Returns:
        Path: Resolved simulation config path.
    """
    sim_config_path = Path(sim_config_path)
    if sim_config_path.is_absolute():
        return sim_config_path
    resolved = REPO_ROOT / sim_config_path
    if not resolved.exists():
        resolved = Path(param_file).parent / sim_config_path
    return resolved


def read_param_file(param_file):
    """
    Read YAML parameter file and return as dictionary

    Parses a Mimic YAML parameter file and returns key-value pairs.

    Args:
        param_file (str or Path): Path to YAML parameter file

    Returns:
        dict: Parameter name -> value mapping

    Usage:
        params = read_param_file(model_input_file("test_binary.yaml"))
        output_dir = params['OutputDir']
        hubble_h = float(params['Hubble_h'])
    """
    import yaml

    param_file = Path(param_file)

    with open(param_file, 'r') as f:
        config = yaml.safe_load(f)

    sim_config = {}
    sim_config_path = config.get('simulation', {}).get('config')
    if sim_config_path:
        sim_config_path = resolve_sim_config_path(sim_config_path, param_file)
        with open(sim_config_path, 'r') as f:
            sim_config = yaml.safe_load(f) or {}

    # Flatten hierarchical structure
    params = {}
    if 'output' in config:
        params['OutputDir'] = config['output'].get('output_directory', './')
        params['OutputFileBaseName'] = config['output'].get('output_filename', 'model')
        params['OutputFormat'] = config['output'].get('output_format', 'binary')

    input_config = sim_config.get('input', config.get('input', {}))
    if input_config:
        params['FirstFile'] = str(input_config.get('first_file', 0))
        params['LastFile'] = str(input_config.get('last_file', 0))
        params['TreeName'] = input_config.get('tree_name', '')
        params['TreeType'] = input_config.get('tree_type', 'lhalo_binary')
        params['SimulationDir'] = input_config.get('simulation_dir', './')
        params['FileWithSnapList'] = input_config.get('snapshot_list_file', '')
        params['LastSnapshotNr'] = str(input_config.get('last_snapshot', 0))

    simulation_config = sim_config.get('simulation', config.get('simulation', {}))
    if simulation_config:
        params['BoxSize'] = str(simulation_config.get('box_size', 0.0))
        params['PartMass'] = str(simulation_config.get('particle_mass', 0.0))
        if 'cosmology' in simulation_config:
            params['Omega'] = str(simulation_config['cosmology'].get('omega_matter', 0.0))
            params['OmegaLambda'] = str(simulation_config['cosmology'].get('omega_lambda', 0.0))
            params['Hubble_h'] = str(simulation_config['cosmology'].get('hubble_h', 0.0))
        if 'units' in simulation_config:
            params['UnitLength_in_cm'] = str(simulation_config['units'].get('length_in_cm', 0.0))
            params['UnitMass_in_g'] = str(simulation_config['units'].get('mass_in_g', 0.0))
            params['UnitVelocity_in_cm_per_s'] = str(simulation_config['units'].get('velocity_in_cm_per_s', 0.0))

    return params


def create_test_param_file(output_name, phase_config=None,
                            model_params=None, first_file=0, last_file=0,
                            ref_param_file=None, temp_dir=None, output_format=None):
    """
    Create a test YAML parameter file with multi-phase module configuration

    Generates a YAML parameter file for testing, based on a reference parameter file
    with custom module configuration and file range.

    Args:
        output_name (str): Name for output directory (created in temp_dir)
        phase_config (dict): Multi-phase pipeline configuration.
                            Format: {
                                'pre_timestep': [('module1', 'process_full_halo'), ('module2', 'process_full_halo')],
                                'phase_1': [('module3', 'process_by_galaxy'), ('module4', 'process_by_galaxy')],
                                'phase_2': [('module5', 'process_by_galaxy')],
                                'post_timestep': [('module6', 'process_full_halo')]
                            }
                            Each tuple is (module_name, processing_mode) where processing_mode is
                            'process_full_halo', 'process_per_event', or 'process_by_galaxy'
        model_params (dict): Dict of {parameter_name: value} for modules.parameters section
        first_file (int): First file to process (default: 0)
        last_file (int): Last file to process (default: 0)
        ref_param_file (str or Path): Reference YAML parameter file
                                      (default: models/<MODEL>/input/test_binary.yaml)
        temp_dir (str or Path): Temporary directory for outputs (default: create new)
        output_format (str): Output format override ('binary' or 'hdf5', default: from ref file)

    Returns:
        tuple: (param_file_path, output_dir_path, temp_dir_path)
               - param_file_path: Path to created parameter file
               - output_dir_path: Path to output directory
               - temp_dir_path: Path to temporary directory (for cleanup)

    Usage:
        # Physics-free mode
        param_file, output_dir, temp_dir = create_test_param_file("test_run")

        # Multi-phase configuration (preferred; use modules from the selected model)
        param_file, output_dir, temp_dir = create_test_param_file(
            output_name="infall_test",
            phase_config={
                'pre_timestep': [('my_prepare_module', 'process_full_halo')],
                'phase_1': [('my_process_module', 'process_by_galaxy')],
                'phase_2': [],
                'post_timestep': []
            },
            model_params={
                "GlobalBaryonFraction": 0.17
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
        ref_param_file = model_input_file("test_binary.yaml")
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

    # Update run configuration
    config['output']['output_directory'] = str(output_dir)
    if output_format is not None:
        config['output']['output_format'] = output_format  # Override format if specified

    sim_config_path = resolve_sim_config_path(config['simulation']['config'], ref_param_file)
    with open(sim_config_path, 'r') as f:
        sim_config = yaml.safe_load(f)
    sim_config['input']['first_file'] = first_file
    sim_config['input']['last_file'] = last_file

    generated_sim_config = Path(temp_dir) / f"{output_name}_simulation.yaml"
    with open(generated_sim_config, 'w') as f:
        yaml.dump(sim_config, f, default_flow_style=False, sort_keys=False)
    config['simulation']['config'] = str(generated_sim_config)

    # Set SubSteps (default to 1 if not in reference file)
    if 'SubSteps' not in config:
        config['SubSteps'] = 1

    # Update module configuration to multi-phase structure
    if not isinstance(config.get('modules'), dict):
        config['modules'] = {}

    # Handle multi-phase configuration (preferred)
    if phase_config is not None:
        # Clear old format if present
        if 'enabled' in config['modules']:
            del config['modules']['enabled']

        # Set each phase
        for phase_name in ['pre_timestep', 'phase_1', 'phase_2', 'post_timestep']:
            phase_modules = phase_config.get(phase_name, [])
            if phase_modules:
                # Convert list of tuples to YAML dict format
                config['modules'][phase_name] = [
                    {module_name: processing_mode} for module_name, processing_mode in phase_modules
                ]
            else:
                config['modules'][phase_name] = []

    # Physics-free mode (no modules)
    else:
        # Clear old format if present
        if 'enabled' in config['modules']:
            del config['modules']['enabled']

        # Empty all phases
        config['modules']['pre_timestep'] = []
        config['modules']['phase_1'] = []
        config['modules']['phase_2'] = []
        config['modules']['post_timestep'] = []

    # Initialize modules.parameters section if model_params provided
    if model_params:
        if 'parameters' not in config['modules']:
            config['modules']['parameters'] = {}
        for param_name, value in model_params.items():
            # Try to convert to appropriate type
            try:
                value_float = float(value)
                if value_float.is_integer():
                    value = int(value_float)
                else:
                    value = value_float
            except (ValueError, TypeError):
                pass  # Keep as string
            config['modules']['parameters'][param_name] = value

    # Write test parameter file as YAML
    param_path = Path(temp_dir) / f"{output_name}.yaml"
    with open(param_path, 'w') as f:
        f.write("#" + "="*77 + "\n")
        f.write("# Mimic Test Configuration\n")
        f.write("#" + "="*77 + "\n")
        f.write("# Auto-generated test parameter file\n")
        f.write("# Multi-phase pipeline: pre_timestep, phase_1, phase_2, post_timestep\n")
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
    'run_mimic_fresh',
    'resolve_sim_config_path',
    'read_param_file',
    'create_test_param_file',
    'check_no_memory_leaks',
]
