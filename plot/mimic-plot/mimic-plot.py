#!/usr/bin/env python

"""
Mimic Plotting Tool - Master plotting script for Mimic galaxy evolution framework output

Usage:
  python mimic-plot.py --param-file=<param_file> [options]

Options:
  --param-file=<file>    Mimic parameter file (required)
  --first-file=<num>     First file to read [default: 0]
  --last-file=<num>      Last file to read [default: use LastFile from param file]
  --snapshot=<num>       Process only this snapshot number
  --all-snapshots        Process all available snapshots
  --evolution-plots      Generate evolution plots only
  --snapshot-plots       Generate snapshot plots only
  --output-dir=<dir>     Output directory for plots [default: <OutputDir>/plots]
  --format=<format>      Output format (.png, .pdf) [default: .png]
  --plots=<list>         Comma-separated list of plots to generate
                         [default: all available plots]
  --use-tex              Use LaTeX for text rendering (not recommended)
  --verbose, -v          Show detailed output including skipped plots
  --quiet, -q            Show minimal output (only summary)
  --help                 Show this help message

Note: By default, both snapshot and evolution plots are generated if neither
      --evolution-plots nor --snapshot-plots is specified.
"""

import argparse
import glob
import importlib
import os
import random
import sys
from pathlib import Path

import matplotlib
import matplotlib.pyplot as plt
import numpy as np
from tqdm import tqdm

# Import HDF5 reader module (optional dependency)
try:
    import hdf5_reader

    HDF5_AVAILABLE = True
except ImportError:
    HDF5_AVAILABLE = False
    print("Warning: h5py not available. HDF5 format reading will not be supported.")

# >>> SAGE-NATIVE-HDF5 (isolated add-on) >>>
# Optional reader for native sage-model HDF5 output (different on-disk layout
# from Mimic HDF5). Enabled by `--input-format=sage-hdf5`. Remove this block
# and the matching markers below to drop SAGE-native support.
try:
    import sage_native_hdf5

    SAGE_NATIVE_AVAILABLE = sage_native_hdf5.H5PY_AVAILABLE
except ImportError:
    SAGE_NATIVE_AVAILABLE = False
# <<< SAGE-NATIVE-HDF5 <<<

random.seed(42)  # For reproducibility with sample data

from output_schema import dtype_from_schema, load_schema

# Import shared output utilities
from output_utils import colour_enabled, error, warn

# Import the SnapshotRedshiftMapper
from snapshot_redshift_mapper import SnapshotRedshiftMapper, read_expansion_factors

REPO_ROOT = Path(__file__).resolve().parents[2]
SNAPSHOT_PLOTS = []
EVOLUTION_PLOTS = []
PLOT_REQUIREMENTS = {}
PLOT_FUNCS = {}
PLOT_PROFILE = {}
PROFILE_PLOTS = {"snapshot": None, "evolution": None}
check_required_properties = None


def print_banner(param_file, quiet=False):
    """Print a coloured MIMIC PLOT ASCII banner and basic run context."""

    if colour_enabled():
        reset = "\x1b[0m"
        # Match the C run_log.c banner colours
        magenta = "\x1b[95m"
        blue = "\x1b[94m"
        cyan = "\x1b[96m"
        green = "\x1b[92m"
        yellow = "\x1b[93m"
        bold = "\x1b[1m"
    else:
        reset = magenta = blue = cyan = green = yellow = bold = ""

    # Skip ASCII art in quiet mode, but keep informational lines
    if not quiet:
        print(
            f"{bold}{magenta}    __  ___  ____  __  ___  ____  ______       ____    __     ___    ______{reset}"
        )
        print(
            f"{blue}   /  |/  / /  _/ /  |/  / /  _/ / ____/      / __ \\  / /    / __ \\ /_  __/{reset}"
        )
        print(
            f"{cyan}  / /|_/ /  / /  / /|_/ /  / /  / /          / /_/ / / /    / / / /  / /   {reset}"
        )
        print(
            f"{green} / /  / / _/ /  / /  / / _/ /  / /___       / ____/ / /___ / /_/ /  / /    {reset}"
        )
        print(
            f"{yellow}/_/  /_/ /___/ /_/  /_/ /___/  \\____/      /_/     /_____/ \\____/  /_/     {bold}{reset}\n"
        )

    print(f"{bold}MIMIC Galaxy Evolution Plotting Tool{reset}")
    print(f"Parameter file : {param_file}")


def print_phase(title):
    """Print a simple phase header similar to Mimic run phases."""

    if colour_enabled():
        cyan = "\x1b[1;36m"
        reset = "\x1b[0m"
    else:
        cyan = reset = ""

    print("")
    print("=" * 63)
    print(f"{cyan}{title}{reset}")
    print("=" * 63)


# Halo data structure definition (read from run metadata)
def get_dtype(output_path):
    """
    Return the NumPy dtype for Mimic binary halo data.

    The dtype is read from metadata/output_schema.json written by the Mimic run
    that produced the output. This keeps plotting tied to the exact model schema
    used for the data, even when the repository has since changed.
    """
    return dtype_from_schema(load_schema(output_path), binary=True)


def resolve_relative_path(path, param_file_path):
    """
    Resolve a run-configuration path relative to the Mimic repository root.

    Args:
        path: Path to resolve (can be relative or absolute)
        param_file_path: Active run file path, retained for the caller API

    Returns:
        Resolved absolute path
    """
    if os.path.isabs(path):
        return path

    relative_part = path[2:] if path.startswith("./") else path
    resolved_path = REPO_ROOT / relative_part

    return os.path.abspath(resolved_path)


def validate_required_params(params, required_params, context=""):
    """
    Validate that required parameters are present.

    Args:
        params: Dictionary of parameters to validate
        required_params: List of required parameter names
        context: Optional context string for error message

    Returns:
        List of missing parameter names (empty if all present)
    """
    missing = [p for p in required_params if p not in params]
    if missing:
        context_str = f" for {context}" if context else ""
        error(f"Required parameters missing from parameter file{context_str}: {', '.join(missing)}")
    return missing


def configure_figure_package(params, param_file, verbose=False):
    """Load the active model's figure package and registry."""
    global SNAPSHOT_PLOTS, EVOLUTION_PLOTS, PLOT_REQUIREMENTS, PLOT_FUNCS
    global check_required_properties

    model_path = params.get("ModelPath")
    if not model_path:
        raise RuntimeError("model.name is required for figure discovery")

    plots_dir = Path(resolve_relative_path(model_path, param_file)) / "plots"
    if not plots_dir.exists():
        raise RuntimeError(f"Model plots directory not found: {plots_dir}")

    sys.path.insert(0, str(plots_dir))
    figures = importlib.import_module("figures")

    SNAPSHOT_PLOTS = list(getattr(figures, "SNAPSHOT_PLOTS", []))
    EVOLUTION_PLOTS = list(getattr(figures, "EVOLUTION_PLOTS", []))
    PLOT_REQUIREMENTS = dict(getattr(figures, "PLOT_REQUIREMENTS", {}))
    PLOT_FUNCS = dict(getattr(figures, "PLOT_FUNCS", {}))
    check_required_properties = getattr(figures, "check_required_properties")

    if verbose:
        print(f"Loaded figure package from {plots_dir}")


def merge_profile(base, override):
    """Recursively merge plot profile dictionaries."""
    merged = dict(base)
    for key, value in override.items():
        if isinstance(value, dict) and isinstance(merged.get(key), dict):
            merged[key] = merge_profile(merged[key], value)
        else:
            merged[key] = value
    return merged


def read_profile_file(path, chain=None):
    """Read one profile file and resolve its inherited profiles.

    Relative ``inherits`` entries are resolved from the directory containing the
    profile file that declares them.

    ``chain`` is the ordered list of ancestor profile paths on the current
    inheritance branch. Each inherited entry is resolved on its own branch, so
    a diamond (two parents sharing a common grandparent) is not mistaken for a
    cycle; only a genuine ancestor repeat raises an error.
    """
    import yaml

    profile_path = Path(path).resolve()
    chain = chain or []
    if profile_path in chain:
        cycle = " -> ".join(str(p) for p in chain + [profile_path])
        raise RuntimeError(f"Plot profile inheritance cycle: {cycle}")
    branch = chain + [profile_path]

    with open(profile_path, "r") as f:
        profile = yaml.safe_load(f) or {}

    merged = {}
    for inherited in profile.get("inherits", []) or []:
        inherited_path = Path(inherited)
        if not inherited_path.is_absolute():
            inherited_path = profile_path.parent / inherited
        merged = merge_profile(merged, read_profile_file(inherited_path, branch))

    profile.pop("inherits", None)
    return merge_profile(merged, profile)


def configure_plot_profile(params, param_file, verbose=False):
    """Load and apply the active plot profile stack."""
    global PLOT_PROFILE, PROFILE_PLOTS

    profile_paths = [REPO_ROOT / "plot/mimic-plot/profiles/default.yaml"]

    model_path = params.get("ModelPath")
    if model_path:
        model_default = (
            Path(resolve_relative_path(model_path, param_file)) / "plots/profiles/default.yaml"
        )
        if model_default.exists():
            profile_paths.append(model_default)

    simulation_path = params.get("SimulationPath")
    if simulation_path:
        simulation_profile = (
            Path(resolve_relative_path(simulation_path, param_file)) / "plot_profile.yaml"
        )
        if simulation_profile.exists():
            profile_paths.append(simulation_profile)

    simulation_name = params.get("SimulationName")
    if model_path and simulation_name:
        model_simulation_profile = (
            Path(resolve_relative_path(model_path, param_file))
            / "plots/profiles"
            / f"{simulation_name}_plot_profile.yaml"
        )
        if model_simulation_profile.exists():
            profile_paths.append(model_simulation_profile)

    configured_profile = params.get("PlottingProfilePath")
    if configured_profile:
        profile_paths.append(Path(resolve_relative_path(configured_profile, param_file)))

    profile = {}
    for profile_path in profile_paths:
        if not profile_path.exists():
            raise RuntimeError(f"Plot profile not found: {profile_path}")
        profile = merge_profile(profile, read_profile_file(profile_path))

    # Run-YAML plotting overrides are the most specific layer and win over every
    # profile file (output -> model default -> simulation -> configured profile).
    run_overrides = params.get("PlottingOverrides") or {}
    if run_overrides:
        profile = merge_profile(profile, run_overrides)

    plots = profile.get("plots", {})
    PROFILE_PLOTS = {
        "snapshot": plots.get("snapshot"),
        "evolution": plots.get("evolution"),
    }
    PLOT_PROFILE = profile
    params["PlotProfile"] = profile

    if verbose:
        loaded = ", ".join(str(path) for path in profile_paths)
        print(f"Loaded plot profiles: {loaded}")


class MimicParameters:
    """Class to parse and store Mimic parameter file settings."""

    # Comment markers used in Mimic parameter files
    COMMENT_MARKERS = ("%", "#", ";")

    def __init__(self, param_file):
        """Initialize with parameter file path."""
        self.param_file = os.path.abspath(param_file)
        self.params = {}

        # Detect format by extension
        if param_file.endswith(".yaml") or param_file.endswith(".yml"):
            self.parse_yaml_file()
        else:
            self.parse_param_file()

    def parse_param_file(self):
        """Parse the Mimic parameter file."""
        if not os.path.exists(self.param_file):
            raise FileNotFoundError(f"Parameter file not found: {self.param_file}")

        # Check for snapshot output list line
        output_snapshots = []

        # Parse the parameter file
        with open(self.param_file, "r") as f:
            for line in f:
                # Skip empty lines and full comment lines
                stripped_line = line.strip()
                if not stripped_line or any(
                    stripped_line.startswith(marker) for marker in self.COMMENT_MARKERS[:2]
                ):
                    continue

                # Legacy SAGE .par files may list snapshots with a leading arrow
                # (e.g. "-> 63 37 32 27 23 20 18 16").
                if stripped_line.startswith("->"):
                    snapshot_list = stripped_line.split("->")[1].strip().split()
                    output_snapshots = [int(snap) for snap in snapshot_list]
                    self.params["OutputSnapshots"] = output_snapshots
                    continue

                # Parse key-value pairs
                if "=" in line:
                    # Standard equals-separated key-value
                    parts = line.split("=")
                    key = parts[0].strip()
                    value_part = parts[1].strip()
                else:
                    # Handle space-separated key-value pairs (common in parameter files)
                    parts = line.split(None, 1)  # Split on whitespace, max 1 split
                    if len(parts) >= 2:
                        key = parts[0].strip()
                        value_part = parts[1].strip()
                    else:
                        continue  # Skip lines that don't match our format

                # Handle inline comments - crucial to remove them before type conversion
                # Check for comment markers and take the earliest one
                comment_positions = [
                    pos for marker in self.COMMENT_MARKERS if (pos := value_part.find(marker)) != -1
                ]

                if comment_positions:
                    # Take everything before the first comment marker
                    first_comment_pos = min(comment_positions)
                    value = value_part[:first_comment_pos].strip()
                else:
                    value = value_part

                # Clean the value - especially important for paths
                value = value.strip()

                # Convert to appropriate type
                if value.lstrip("-").isdigit():
                    value = int(value)
                elif self._is_float(value):
                    value = float(value)
                elif key in ["OutputDir", "SimulationDir"]:
                    # Ensure directory paths are properly formatted
                    value = value.strip('"').strip("'")
                    # Make sure directory paths have a trailing slash
                    if value and not value.endswith("/"):
                        value = value + "/"
                elif key in ["FileWithSnapList"]:
                    # Ensure file paths are properly formatted
                    value = value.strip('"').strip("'")
                    # Don't add trailing slash to file paths

                self.params[key] = value

        if "FirstFile" in self.params and "LastFile" in self.params:
            self.params["NumSimulationTreeFiles"] = (
                self.params["LastFile"] - self.params["FirstFile"] + 1
            )
            # For .par format there is no separate simulation-extent concept; the
            # specified file range is the full volume being analysed.
            self.params["SimulationTotalTreeFiles"] = self.params["NumSimulationTreeFiles"]
        elif "NumSimulationTreeFiles" not in self.params:
            error("Could not determine NumSimulationTreeFiles, check FirstFile and LastFile")
            sys.exit(1)

    def parse_yaml_file(self):
        """Parse YAML format parameter file."""
        import yaml

        with open(self.param_file, "r") as f:
            config = yaml.safe_load(f) or {}

        model_config = config.get("model") or {}
        simulation_config_run = config.get("simulation") or {}
        unknown_model_keys = set(model_config) - {"name"}
        unknown_simulation_keys = set(simulation_config_run) - {
            "name",
            "config",
            "cosmology",
            "box_size",
            "particle_mass",
            "units",
        }
        if unknown_model_keys:
            keys = ", ".join(sorted(unknown_model_keys))
            raise RuntimeError(f"Unknown model key(s): {keys}")
        if unknown_simulation_keys:
            keys = ", ".join(sorted(unknown_simulation_keys))
            raise RuntimeError(f"Unknown simulation key(s): {keys}")

        model_name = model_config.get("name", "")
        simulation_name = simulation_config_run.get("name", "")
        model_path = f"models/{model_name}" if model_name else ""
        simulation_path = f"simulations/{simulation_name}" if simulation_name else ""
        simulation_config_path = simulation_config_run.get("config", "")
        if not simulation_config_path and simulation_path:
            simulation_config_path = f"{simulation_path}/simulation_info.yaml"

        sim_config = {}
        if simulation_config_path:
            sim_config_path = resolve_relative_path(simulation_config_path, self.param_file)
            with open(sim_config_path, "r") as f:
                sim_config = yaml.safe_load(f) or {}

        if model_config:
            self.params["ModelName"] = model_name
            self.params["ModelPath"] = model_path
            self.params["ModelPropertiesPath"] = (
                f"{model_path}/model_properties.yaml" if model_path else ""
            )

        if simulation_config_run:
            self.params["SimulationName"] = simulation_name
            self.params["SimulationPath"] = simulation_path
            self.params["SimulationConfigPath"] = simulation_config_path
            self.params["SimulationHaloPropertiesPath"] = (
                f"{simulation_path}/halo_properties.yaml" if simulation_path else ""
            )

        if "plotting" in config:
            self.params["PlottingProfilePath"] = config["plotting"].get("profile", "")
            # Any other keys under `plotting` are inline run overrides applied as
            # the most specific profile layer (see configure_plot_profile).
            self.params["PlottingOverrides"] = {
                key: value for key, value in (config["plotting"] or {}).items() if key != "profile"
            }

        # Flatten hierarchical YAML structure
        # Output section
        if "output" in config:
            output_snapshots = config["output"].get("snapshot_list", [])
            self.params["OutputFileBaseName"] = config["output"].get("output_filename", "model")
            self.params["OutputDir"] = config["output"].get("output_directory", "./")
            self.params["OutputFormat"] = config["output"].get("output_format", "binary")
            self.params["NumOutputs"] = len(output_snapshots)
            self.params["OutputSnapshots"] = output_snapshots

        # Input section from simulation package
        input_config = sim_config.get("input", {})
        if input_config:
            sim_first = input_config.get("first_file", 0)
            sim_last = input_config.get("last_file", 0)
            self.params["FirstFile"] = sim_first
            self.params["LastFile"] = sim_last
            self.params["TreeName"] = input_config.get("tree_name", "")
            self.params["TreeType"] = input_config.get("tree_type", "lhalo_binary")
            self.params["SimulationDir"] = input_config.get("simulation_dir", "./")
            self.params["FileWithSnapList"] = input_config.get("snapshot_list_file", "")

            # Full simulation file count — denominator for volume fraction
            self.params["SimulationTotalTreeFiles"] = sim_last - sim_first + 1

            # Apply first_file/last_file overrides from the run YAML's input: block.
            # These are the only input-block fields that affect plotting calculations.
            run_input = config.get("input", {})
            if run_input:
                if "first_file" in run_input:
                    self.params["FirstFile"] = run_input["first_file"]
                if "last_file" in run_input:
                    self.params["LastFile"] = run_input["last_file"]

            # Files actually processed — numerator for volume fraction
            self.params["NumSimulationTreeFiles"] = (
                self.params["LastFile"] - self.params["FirstFile"] + 1
            )

            a_list_path = resolve_relative_path(self.params["FileWithSnapList"], self.param_file)
            if os.path.exists(a_list_path):
                num_snapshots = len(read_expansion_factors(a_list_path))
                self.params["LastSnapshotNr"] = num_snapshots - 1
                if not self.params["OutputSnapshots"]:
                    self.params["OutputSnapshots"] = list(range(num_snapshots))
                    self.params["NumOutputs"] = num_snapshots

        # Simulation section from simulation package
        simulation_config = sim_config.get("simulation", {})
        if simulation_config:
            self.params["BoxSize"] = simulation_config.get("box_size", 0.0)
            self.params["PartMass"] = simulation_config.get("particle_mass", 0.0)
            if "cosmology" in simulation_config:
                self.params["Omega"] = simulation_config["cosmology"].get("omega_matter", 0.0)
                self.params["OmegaLambda"] = simulation_config["cosmology"].get("omega_lambda", 0.0)
                self.params["Hubble_h"] = simulation_config["cosmology"].get("hubble_h", 0.0)
            if "units" in simulation_config:
                self.params["UnitLength_in_cm"] = simulation_config["units"].get(
                    "length_in_cm", 0.0
                )
                self.params["UnitMass_in_g"] = simulation_config["units"].get("mass_in_g", 0.0)
                self.params["UnitVelocity_in_cm_per_s"] = simulation_config["units"].get(
                    "velocity_in_cm_per_s", 0.0
                )

        # Modules section (for reference, though not used in plotting currently)
        if "modules" in config:
            self.params["EnabledModules"] = config["modules"]

    def _is_float(self, value):
        """Check if a string can be converted to float."""
        try:
            float(value)
            return True
        except ValueError:
            return False

    def get(self, key, default=None):
        """Get a parameter value."""
        return self.params.get(key, default)

    def __getitem__(self, key):
        """Allow dictionary-like access to parameters."""
        return self.params[key]

    def __contains__(self, key):
        """Check if a parameter exists."""
        return key in self.params


def setup_matplotlib(use_tex=False):
    """Set up matplotlib with standard settings."""
    matplotlib.rcdefaults()
    plt.rc("xtick", labelsize="x-large")
    plt.rc("ytick", labelsize="x-large")
    plt.rc("lines", linewidth="2.0")
    plt.rc("legend", numpoints=1, fontsize="x-large")

    # Only use LaTeX if explicitly requested
    if use_tex:
        try:
            plt.rc("text", usetex=True)
            print("LaTeX rendering enabled for text")
        except Exception as e:
            print(f"Warning: Could not enable LaTeX: {e}")
            # Fall back to regular text rendering
            plt.rc("text", usetex=False)
    else:
        # Explicitly disable LaTeX
        plt.rc("text", usetex=False)

    # Set up nice math rendering even without LaTeX
    plt.rcParams["mathtext.fontset"] = "dejavusans"
    plt.rcParams["mathtext.default"] = "regular"


def read_data(model_path, first_file, last_file, params=None, verbose=False, quiet=False):
    """
    Read galaxy data from Mimic output files.

    Args:
        model_path: Path to model files
        first_file: First file number to read
        last_file: Last file number to read
        params: Dictionary with Mimic parameters
        verbose: Enable verbose output
        quiet: Suppress all output

    Returns:
        Tuple containing:
            - Numpy recarray of galaxy data
            - Volume of the simulation
            - Dictionary of metadata
    """
    if verbose:
        print(f"Reading galaxy data from {model_path}")
    # Get required parameters from the parameter file
    if not params:
        error("Parameter dictionary is required.")
        sys.exit(1)

    # Ensure required parameters exist
    if validate_required_params(params, ["Hubble_h", "BoxSize"], "galaxy data reading"):
        sys.exit(1)

    hubble_h = params["Hubble_h"]
    box_size = params["BoxSize"]

    # >>> SAGE-NATIVE-HDF5 (isolated add-on) >>>
    # Route to the SAGE-native reader before any Mimic-format checks so the
    # OutputFormat value in the parameter file does not interfere.
    if params.get("_input_format") == "sage-hdf5":
        if not SAGE_NATIVE_AVAILABLE:
            print(
                "ERROR: --input-format=sage-hdf5 requires h5py. " "Install with: pip install h5py"
            )
            sys.exit(1)
        return sage_native_hdf5.read_data_sage_native(
            model_path, first_file, last_file, params, verbose, quiet
        )
    # <<< SAGE-NATIVE-HDF5 <<<

    # Detect output format from parameter file
    output_format = params.get("OutputFormat", "binary")
    if isinstance(output_format, str):
        output_format = output_format.lower()

    # Handle HDF5 format
    if output_format == "hdf5":
        if not HDF5_AVAILABLE:
            error(
                "OutputFormat=hdf5 specified in parameter file, but h5py is not installed. "
                "Install with: pip install h5py"
            )
            sys.exit(1)

        if verbose:
            print(f"Using HDF5 format reader")
        return read_data_hdf5(model_path, first_file, last_file, params, verbose, quiet)

    # For binary format (default)
    if verbose:
        print(f"Using binary format reader")

    # For volume calculation, we'll use the number of good files read
    # No need for MaxTreeFiles parameter - we'll calculate based on actual files read

    # Print the model path for debugging
    if verbose:
        print(f"Looking for galaxy files with base: {model_path}")

    # Look for files matching the pattern in the same directory
    dir_path = os.path.dirname(model_path)
    base_name = os.path.basename(model_path)

    # First try exact file number pattern (model_z0.000_0, model_z0.000_1, etc.)
    pattern1 = f"{model_path}_{first_file}"

    # Then try generic pattern (model_z0.000_*)
    pattern2 = os.path.join(dir_path, f"{base_name}_*")

    # Log the patterns we're trying
    if verbose:
        print(f"  Trying exact pattern: {pattern1}")
        print(f"  Trying generic pattern: {pattern2}")

    # Try the exact pattern first
    exact_files = glob.glob(pattern1)
    if exact_files:
        existing_files = exact_files
        if verbose:
            print(f"  Found file with exact pattern")
    else:
        # Fall back to the generic pattern
        existing_files = glob.glob(pattern2)

    if existing_files:
        if verbose:
            print(f"Found {len(existing_files)} files matching the pattern.")
            for f in existing_files[:5]:  # Show first 5 files
                print(f"  {f}")
            if len(existing_files) > 5:
                print(f"  ... and {len(existing_files) - 5} more")
    else:
        warn(f"No files found matching the pattern {base_name}_*")

    # Get the galaxy data dtype
    galdesc = get_dtype(dir_path)

    # Initialize variables
    tot_ntrees = 0
    tot_ngals = 0
    good_files = 0

    if verbose:
        print(f"Determining storage requirements for files {first_file} to {last_file}...")

    # First pass: Determine total number of galaxies
    # Only show progress bar when verbose is enabled
    file_iterator = (
        tqdm(range(first_file, last_file + 1), desc="Counting galaxies")
        if verbose
        else range(first_file, last_file + 1)
    )
    for fnr in file_iterator:
        fname = f"{model_path}_{fnr}"

        if not os.path.isfile(fname):
            continue

        if os.path.getsize(fname) == 0:
            print(f"File {fname} is empty! Skipping...")
            continue

        try:
            with open(fname, "rb") as fin:
                ntrees = np.fromfile(fin, np.dtype(np.int32), 1)[0]  # Extract scalar value
                ntotgals = np.fromfile(fin, np.dtype(np.int32), 1)[0]
                tot_ntrees += ntrees
                tot_ngals += ntotgals
                good_files += 1
        except Exception as e:
            print(f"Error reading file {fname}: {e}")
            continue

    print(f"Input files contain: {tot_ntrees} trees, {tot_ngals} galaxies.")

    # Check if we found any galaxies
    if tot_ngals == 0:
        error_msg = "No galaxies found in the model files"
        if verbose:
            error(f"{error_msg}. Please check that the model files exist and are not empty.")
        raise FileNotFoundError(error_msg)

    # Initialize the storage array
    galaxies = np.empty(tot_ngals, dtype=galdesc)

    # Second pass: Read the galaxy data
    offset = 0
    # Only show progress bar when verbose is enabled
    file_iterator = (
        tqdm(range(first_file, last_file + 1), desc="Reading galaxies")
        if verbose
        else range(first_file, last_file + 1)
    )
    for fnr in file_iterator:
        fname = f"{model_path}_{fnr}"

        if not os.path.isfile(fname) or os.path.getsize(fname) == 0:
            continue

        try:
            with open(fname, "rb") as fin:
                ntrees = np.fromfile(fin, np.dtype(np.int32), 1)[0]  # Extract scalar value
                ntotgals = np.fromfile(fin, np.dtype(np.int32), 1)[0]
                gals_per_tree = np.fromfile(fin, np.dtype((np.int32, ntrees)), 1)

                if verbose:
                    print(f"Reading {ntotgals} galaxies from file: {fname}")

                gg = np.fromfile(fin, galdesc, ntotgals)

                # Slice the file array into the global array with a copy
                galaxies[offset : offset + ntotgals] = gg[0:ntotgals].copy()

                offset += ntotgals
        except Exception as e:
            print(f"Error reading file {fname}: {e}")
            continue

    # Convert to recarray for attribute access
    galaxies = galaxies.view(np.recarray)

    volume = box_size**3.0

    # Scale volume by the fraction of simulation files actually read.
    # NumSimulationTreeFiles reflects any first_file/last_file overrides from the run YAML.
    # SimulationTotalTreeFiles is always the full simulation extent from simulation_info.yaml.
    if "NumSimulationTreeFiles" in params and "SimulationTotalTreeFiles" in params:
        total_files = params["SimulationTotalTreeFiles"]
        if total_files > 0:
            volume = volume * good_files / total_files
            if verbose:
                print(
                    f"  Volume fraction: {good_files}/{total_files} = {good_files/total_files:.4f}"
                )
                print(f"  Adjusted volume: {volume:.2f} (Mpc/h)³")

    # Create metadata dictionary
    metadata = {
        "hubble_h": hubble_h,
        "box_size": box_size,
        "volume": volume,
        "ntrees": tot_ntrees,
        "ngals": tot_ngals,
        "good_files": good_files,
    }

    return galaxies, volume, metadata


def read_data_hdf5(model_path, first_file, last_file, params, verbose=False, quiet=False):
    """
    Read halo data from Mimic HDF5 output files.

    Args:
        model_path: Path to model files (base name, may include _z0.000 suffix from binary format)
        first_file: First file number to read
        last_file: Last file number to read
        params: Dictionary with Mimic parameters
        verbose: Enable verbose output
        quiet: Suppress all output

    Returns:
        Tuple containing:
            - Numpy recarray of galaxy data
            - Volume of the simulation
            - Dictionary of metadata
    """
    if verbose:
        print(f"Reading galaxy data from HDF5 files: {model_path}")

    hubble_h = params["Hubble_h"]
    box_size = params["BoxSize"]

    # Get the directory and base name
    dir_path = os.path.dirname(model_path)
    base_name = os.path.basename(model_path)

    # Extract redshift string from model_path to determine which snapshot to read
    # Model path format: /path/to/model_z1.386 or /path/to/model
    redshift_str = None
    if "_z" in base_name:
        # Extract the redshift string (e.g., "_z1.386")
        parts = base_name.split("_z")
        redshift_str = f"_z{parts[1]}"
        base_name = parts[0]  # Remove the redshift suffix for HDF5 filename
        if verbose:
            print(f"Extracted redshift string: {redshift_str}, base name: {base_name}")

    # Map redshift string to snapshot number using SnapshotRedshiftMapper
    # We need to import and use the mapper to find which snapshot corresponds to this redshift
    from snapshot_redshift_mapper import SnapshotRedshiftMapper

    # Create mapper to find snapshot from redshift
    # Note: param_file not available in this context, pass None
    # Add quiet and verbose flags to params for mapper
    mapper_params = params.copy()
    mapper_params["quiet"] = quiet
    mapper_params["verbose"] = verbose
    mapper = SnapshotRedshiftMapper(None, mapper_params, dir_path)

    # Find the snapshot number that matches the redshift string
    snapshot_num = None
    if redshift_str:
        # Find matching snapshot in mapper's redshift_strs
        try:
            idx = mapper.redshift_strs.index(redshift_str)
            snapshot_num = mapper.snapshots[idx]
            if verbose:
                print(f"Mapped {redshift_str} to snapshot {snapshot_num}")
        except ValueError:
            print(f"Error: Redshift string {redshift_str} not found in snapshot mapping")
            print(f"Available redshift strings: {mapper.redshift_strs[:10]}...")
            sys.exit(1)
    else:
        # No redshift suffix in model_path - use first snapshot from OutputSnapshots
        # Note: OutputSnapshots order is defined by the parameter file.
        # Typically listed in descending order (highest first), e.g., "-> 63 37 32..."
        output_snapshots = params.get("OutputSnapshots", [])
        if output_snapshots:
            snapshot_num = output_snapshots[0]
            if verbose:
                print(f"No redshift in path, using first OutputSnapshot: {snapshot_num}")
        else:
            print("Error: No redshift in model path and no OutputSnapshots in parameter file")
            sys.exit(1)

    if verbose:
        print(f"Reading snapshot {snapshot_num}")

    # Try to find the master file first
    master_file = os.path.join(dir_path, f"{base_name}.hdf5")

    galaxies_list = []
    tot_ngals = 0
    good_files = 0

    # Try master file first
    if os.path.exists(master_file):
        if verbose:
            print(f"Found master file: {master_file}")
        halos = hdf5_reader.read_hdf5_snapshot(master_file, snapshot_num)
        if halos is not None:
            tot_ngals = len(halos)
            galaxies_list.append(halos)
            good_files = last_file - first_file + 1  # All files represented in master
            if verbose:
                print(f"Read {tot_ngals} halos from master file")
    else:
        # Fall back to individual files
        if verbose:
            print(f"Master file not found, reading individual files...")

        file_iterator = (
            tqdm(range(first_file, last_file + 1), desc="Reading HDF5 files")
            if verbose
            else range(first_file, last_file + 1)
        )

        for fnr in file_iterator:
            fname = os.path.join(dir_path, f"{base_name}_{fnr:03d}.hdf5")

            if not os.path.isfile(fname):
                continue

            halos = hdf5_reader.read_hdf5_snapshot(fname, snapshot_num)
            if halos is not None:
                galaxies_list.append(halos)
                tot_ngals += len(halos)
                good_files += 1
                if verbose:
                    print(f"Read {len(halos)} halos from {fname}")

    if not galaxies_list:
        error_msg = "No halos found in HDF5 files"
        if verbose:
            error(f"{error_msg}. Please check that the HDF5 files exist and contain data.")
        raise FileNotFoundError(error_msg)

    # Concatenate all halos
    galaxies = np.concatenate(galaxies_list)
    galaxies = galaxies.view(np.recarray)

    if verbose:
        print(f"Total halos read: {tot_ngals}")

    volume = box_size**3.0

    # Scale volume by the fraction of simulation files actually read.
    # NumSimulationTreeFiles reflects any first_file/last_file overrides from the run YAML.
    # SimulationTotalTreeFiles is always the full simulation extent from simulation_info.yaml.
    if "NumSimulationTreeFiles" in params and "SimulationTotalTreeFiles" in params:
        total_files = params["SimulationTotalTreeFiles"]
        if total_files > 0:
            volume = volume * good_files / total_files
            if verbose:
                print(
                    f"  Volume fraction: {good_files}/{total_files} = {good_files/total_files:.4f}"
                )
                print(f"  Adjusted volume: {volume:.2f} (Mpc/h)³")

    # Create metadata dictionary
    metadata = {
        "hubble_h": hubble_h,
        "box_size": box_size,
        "volume": volume,
        "ntrees": 0,  # Not tracked in HDF5 format
        "ngals": tot_ngals,
        "good_files": good_files,
    }

    return galaxies, volume, metadata


def parse_arguments():
    """Parse command-line arguments."""
    parser = argparse.ArgumentParser(description="Mimic Plotting Tool")
    parser.add_argument("--param-file", required=True, help="Mimic parameter file (required)")
    parser.add_argument(
        "--first-file", type=int, help="First file to read (overrides parameter file)"
    )
    parser.add_argument(
        "--last-file", type=int, help="Last file to read (overrides parameter file)"
    )
    parser.add_argument(
        "--snapshot", type=int, help="Process only this snapshot number (overrides parameter file)"
    )
    parser.add_argument(
        "--all-snapshots", action="store_true", help="Process all available snapshots"
    )
    parser.add_argument(
        "--evolution-plots", action="store_true", help="Generate evolution plots only"
    )
    parser.add_argument(
        "--snapshot-plots", action="store_true", help="Generate snapshot plots only"
    )
    parser.add_argument(
        "--output-dir",
        help="Output directory for plots (default: <OutputDir>/plots)",
    )
    parser.add_argument("--format", default=".png", help="Output format (.png, .pdf)")
    parser.add_argument(
        "--plots", help="Comma-separated list of plots to generate (default: all available plots)"
    )
    parser.add_argument(
        "--use-tex",
        action="store_true",
        help="Use LaTeX for text rendering (not recommended)",
    )
    parser.add_argument("--verbose", "-v", action="store_true", help="Show detailed output")
    parser.add_argument("--quiet", "-q", action="store_true", help="Show minimal output")

    # >>> SAGE-NATIVE-HDF5 (isolated add-on) >>>
    parser.add_argument(
        "--input-format",
        choices=["mimic", "sage-hdf5"],
        default="mimic",
        help="Input data format. 'mimic' (default) reads Mimic binary or "
        "HDF5 output. 'sage-hdf5' reads native sage-model HDF5 output "
        "from the OutputDir set in the parameter file, applying the "
        "SAGE-to-Mimic field mapping so the same plotting code works "
        "on both.",
    )
    # <<< SAGE-NATIVE-HDF5 <<<

    args = parser.parse_args()

    # Quiet and verbose are mutually exclusive
    if args.quiet and args.verbose:
        parser.error("--quiet and --verbose cannot be used together")

    # Default to both snapshot and evolution plots if neither is specified
    if not args.evolution_plots and not args.snapshot_plots:
        args.snapshot_plots = True
        args.evolution_plots = True

    # Default to all plots if not specified
    if args.plots is None:
        args.plots = "all"

    return args


def get_available_plot_modules(plot_type, verbose=False):
    """
    Get available plot modules of a specific type.

    Args:
        plot_type: 'snapshot' or 'evolution'
        verbose: Enable verbose output

    Returns:
        Dictionary mapping plot names to their modules
    """
    modules = {}

    # Get the module patterns from the figures module
    if plot_type == "snapshot":
        module_patterns = SNAPSHOT_PLOTS
    else:  # 'evolution'
        module_patterns = EVOLUTION_PLOTS

    profile_plots = PROFILE_PLOTS.get(plot_type)
    if profile_plots is not None:
        known = set(module_patterns)
        missing = [name for name in profile_plots if name not in known]
        if missing and verbose:
            print(
                f"Warning: Plot profile references unknown {plot_type} plot(s): {', '.join(missing)}"
            )
        module_patterns = [name for name in profile_plots if name in known]

    # Import modules
    for pattern in module_patterns:
        try:
            module_name = f"figures.{pattern}"
            module = importlib.import_module(module_name)
            # Get the main plotting function from the module
            plot_func = getattr(module, "plot", None)
            if plot_func:
                modules[pattern] = plot_func
        except (ImportError, AttributeError) as e:
            if verbose:
                print(f"Warning: Could not import {module_name}: {e}")

    return modules


def main():
    """Main execution function."""
    args = parse_arguments()

    # Startup banner
    print_banner(args.param_file, quiet=args.quiet)

    # Parse the parameter file
    if not args.quiet:
        print_phase("CONFIGURATION")
    try:
        params = MimicParameters(args.param_file)

        # >>> SAGE-NATIVE-HDF5 (isolated add-on) >>>
        # Stash the input-format choice so read_data() can dispatch to the
        # SAGE-native reader without changing its function signature.
        params.params["_input_format"] = args.input_format
        if args.input_format == "sage-hdf5":
            # SAGE .par files use different key names for two params Mimic expects.
            if "FileNameGalaxies" in params.params and "OutputFileBaseName" not in params.params:
                params.params["OutputFileBaseName"] = params.params["FileNameGalaxies"]
            if "LastSnapShotNr" in params.params and "LastSnapshotNr" not in params.params:
                params.params["LastSnapshotNr"] = params.params["LastSnapShotNr"]
            if not args.quiet:
                print("Input format    : sage-hdf5 (native sage-model HDF5)")
        # <<< SAGE-NATIVE-HDF5 <<<

        # Show a concise summary unless in quiet mode
        if not args.quiet:
            summary_keys = [
                "OutputDir",
                "OutputFileBaseName",
                "FirstFile",
                "LastFile",
                "BoxSize",
                "Hubble_h",
            ]
            print("Parameter file summary:")
            for key in summary_keys:
                if key in params.params:
                    print(f"  {key:16s} = {params.params[key]}")

        # Extra detail only in verbose mode
        if args.verbose:
            print(f"\nLoaded parameters from {args.param_file}")
            print("Raw parameter values:")
            for key, value in params.params.items():
                print(f"  {key} = {value} (type: {type(value)})")
    except Exception as e:
        error(f"Could not load parameter file: {e}")
        sys.exit(1)

    # Verify all required parameters exist
    required_params = [
        "OutputDir",
        "OutputFileBaseName",
        "FirstFile",
        "LastFile",
        "BoxSize",
        "Hubble_h",
        "FileWithSnapList",
    ]

    if validate_required_params(params.params, required_params):
        sys.exit(1)

    try:
        configure_figure_package(params.params, args.param_file, args.verbose)
        configure_plot_profile(params.params, args.param_file, args.verbose)
    except Exception as e:
        error(f"Could not load model plotting package: {e}")
        sys.exit(1)

    # Resolve and update paths from parameter file
    output_dir = resolve_relative_path(params["OutputDir"], args.param_file)
    params.params["OutputDir"] = output_dir  # Update the params dictionary

    simulation_dir = params.get("SimulationDir")  # May not be used directly
    if simulation_dir:
        simulation_dir = resolve_relative_path(simulation_dir, args.param_file)
        params.params["SimulationDir"] = simulation_dir  # Update the params dictionary

    file_name_base = params["OutputFileBaseName"]

    file_with_snap_list = resolve_relative_path(params["FileWithSnapList"], args.param_file)
    params.params["FileWithSnapList"] = file_with_snap_list  # Update the params dictionary

    if args.verbose:
        print(f"Parameter file details:")
        print(f"  OutputDir: {output_dir}")
        print(f"  SimulationDir: {simulation_dir if simulation_dir else 'Not specified'}")
        print(f"  OutputFileBaseName: {file_name_base}")
        print(f"  FirstFile: {params['FirstFile']}")
        print(f"  LastFile: {params['LastFile']}")
        print(f"  FileWithSnapList: {file_with_snap_list}")
        print(f"  BoxSize: {params['BoxSize']}")
        print(f"  Hubble_h: {params['Hubble_h']}")

    # Check if OutputDir exists
    if not os.path.exists(output_dir):
        error(f"OutputDir '{output_dir}' from parameter file does not exist.")
        sys.exit(1)

    # Check if FileWithSnapList exists (path already resolved)
    if not os.path.exists(file_with_snap_list):
        error(
            f"FileWithSnapList '{file_with_snap_list}' not found. "
            "Please verify the path is correct in the parameter file."
        )
        sys.exit(1)

    # Set up matplotlib
    setup_matplotlib(args.use_tex)

    # Get output directory from parameter file - required parameter
    if "OutputDir" not in params:
        error("OutputDir parameter is required in the parameter file.")
        sys.exit(1)

    # Get the output directory path (already resolved)
    model_output_dir = params["OutputDir"]

    if args.verbose:
        print(f"\nOutput directory handling:")
        print(f"  model_output_dir from params: '{model_output_dir}'")

    # Check if output directory exists
    if not os.path.exists(model_output_dir):
        error(f"OutputDir '{model_output_dir}' specified in parameter file does not exist.")
        sys.exit(1)

    # Check if output directory is writable
    if not os.access(model_output_dir, os.W_OK):
        error(f"OutputDir '{model_output_dir}' specified in parameter file is not writable.")
        sys.exit(1)

    # Set the plots directory independently from the model output directory.
    if args.output_dir:
        output_dir = resolve_relative_path(args.output_dir, args.param_file)
    else:
        output_dir = os.path.join(model_output_dir, "plots")

    if args.verbose:
        print(f"  Using output directory: '{output_dir}'")

    # Create the plots directory if it doesn't exist
    try:
        os.makedirs(output_dir, exist_ok=True)
        if args.verbose:
            print(f"  Successfully created/verified output directory: {output_dir}")
    except Exception as e:
        error(f"Could not create output directory {output_dir}: {e}")
        sys.exit(1)

    # Determine which plots to generate
    if args.plots == "all":
        selected_plots = None  # All available
    else:
        selected_plots = [p.strip() for p in args.plots.split(",")]

    # Show simple progress message in quiet mode
    if args.quiet:
        print("\nCreating plots ...")

    # Generate snapshot plots
    if args.snapshot_plots:
        if not args.quiet:
            print_phase("SNAPSHOT PLOTS")
        # Get required parameters for finding model files
        if "OutputDir" not in params:
            error("OutputDir parameter is required in the parameter file.")
            sys.exit(1)

        if "OutputFileBaseName" not in params:
            error("OutputFileBaseName parameter is required in the parameter file.")
            sys.exit(1)

        # Get output model path and snapshot number (already resolved)
        model_path = params["OutputDir"]
        snapshot = args.snapshot if args.snapshot is not None else params.get("LastSnapshotNr")

        if snapshot is None:
            error(
                "Could not derive the last snapshot from FileWithSnapList "
                "and no snapshot was specified."
            )
            sys.exit(1)

        # File name from parameter file
        file_name_base = params["OutputFileBaseName"]

        if args.verbose:
            print(f"\nModel file discovery:")
            print(f"  model_path from params: '{model_path}'")
            print(f"  file_name_base: '{file_name_base}'")
            print(f"  Using snapshot: {snapshot}")

        # Check if model_path exists
        if not os.path.exists(model_path):
            error(f"OutputDir '{model_path}' from parameter file does not exist.")
            sys.exit(1)

        # Get the redshift for this snapshot using the mapper
        # Add quiet and verbose flags to params for mapper
        mapper_params = params.params.copy()
        mapper_params["quiet"] = args.quiet
        mapper_params["verbose"] = args.verbose
        mapper = SnapshotRedshiftMapper(args.param_file, mapper_params, model_path)
        redshift_str = mapper.get_redshift_str(snapshot)

        if args.verbose:
            print(f"  Redshift string for snapshot {snapshot}: {redshift_str}")

        # Construct the base model file path directly
        base_model_file = os.path.join(model_path, f"{file_name_base}{redshift_str}")

        if args.verbose:
            print(f"  Using model file base: {base_model_file}")

        # Required parameters check
        if validate_required_params(params.params, ["FirstFile", "LastFile"], "snapshot plots"):
            sys.exit(1)

        # Get first and last file numbers, prioritizing command-line arguments
        if args.first_file is not None:
            first_file = args.first_file
            if args.verbose:
                print(f"Using first_file={first_file} from command-line argument")
        else:
            first_file = params["FirstFile"]
            if args.verbose:
                print(f"Using first_file={first_file} from parameter file")

        if args.last_file is not None:
            last_file = args.last_file
            if args.verbose:
                print(f"Using last_file={last_file} from command-line argument")
        else:
            last_file = params["LastFile"]
            if args.verbose:
                print(f"Using last_file={last_file} from parameter file")

        # Validate file range
        if first_file > last_file:
            error(f"FirstFile ({first_file}) is greater than LastFile ({last_file})")
            sys.exit(1)

        # Read galaxy data
        try:
            galaxies, volume, metadata = read_data(
                model_path=base_model_file,
                first_file=first_file,
                last_file=last_file,
                params=params.params,
                verbose=args.verbose,
                quiet=args.quiet,
            )
            if args.verbose:
                print(f"Read {len(galaxies)} galaxies from volume {volume:.2f} (Mpc/h)³")
            snapshot_data_available = True
        except Exception as e:
            # Set empty list for snapshot plots
            snapshot_generated_plots = []
            snapshot_data_available = False
            if not args.quiet:
                if args.verbose:
                    # Show detailed error only in verbose mode
                    warn(f"Could not read snapshot data: {e}")
                warn("Skipping snapshot plots (no data available)")
            # Continue to evolution plots (if enabled)
            if not args.evolution_plots:
                # If only snapshot plots were requested and they failed, exit with error
                error("No plots could be generated.")
                sys.exit(1)
            else:
                # Continue to evolution plots section
                if not args.quiet:
                    print("")

        # Only generate snapshot plots if data was successfully loaded
        if snapshot_data_available:
            # Get available snapshot plot modules
            plot_modules = get_available_plot_modules("snapshot", args.verbose)

            if args.verbose:
                print(f"Available snapshot plots: {', '.join(plot_modules.keys())}")

            # Filter to selected plots if specified
            if selected_plots:
                plot_modules = {k: v for k, v in plot_modules.items() if k in selected_plots}

            # Filter plots based on available properties
            available_plots = {}
            skipped_plots = {}

            for plot_name, plot_func in plot_modules.items():
                required_props = PLOT_REQUIREMENTS.get(plot_name, [])
                if required_props:
                    # Check if required properties are available
                    props_available, missing_props = check_required_properties(
                        galaxies, required_props
                    )
                    if not props_available:
                        skipped_plots[plot_name] = missing_props
                        continue
                available_plots[plot_name] = plot_func

            # Report skipped plots
            if skipped_plots:
                print(f"\nSkipping {len(skipped_plots)} plot(s) due to missing properties:")
                for plot_name, missing in skipped_plots.items():
                    print(f"  - {plot_name}: missing {', '.join(missing)}")
                print(f"  (Enable physics modules to generate these plots)\n")

            # Generate each plot
            snapshot_generated_plots = []
            snapshot_skipped_validation = {}  # Track plots skipped due to data validation
            for plot_name, plot_func in available_plots.items():
                try:
                    if args.verbose:
                        print(f"Generating {plot_name}...")
                    result = plot_func(
                        galaxies=galaxies,
                        volume=volume,
                        metadata=metadata,
                        params=params.params,
                        output_dir=output_dir,
                        output_format=args.format,
                        verbose=args.verbose,
                    )

                    plot_path, skip_msg = result
                    if plot_path:
                        snapshot_generated_plots.append(plot_path)
                        if not args.quiet:
                            print(f"Created {plot_name} plot")
                    elif skip_msg:
                        snapshot_skipped_validation[plot_name] = skip_msg
                        if args.verbose:
                            print(f"Skipped {plot_name}: {skip_msg}")
                except Exception as e:
                    if not args.quiet:
                        print(f"Error generating {plot_name}: {e}")

            if args.verbose:
                print(f"Generated {len(snapshot_generated_plots)} snapshot plots.")

    # Generate evolution plots
    if args.evolution_plots:
        if not args.quiet:
            print_phase("EVOLUTION PLOTS")
        # Get available evolution plot modules
        plot_modules = get_available_plot_modules("evolution", args.verbose)

        if args.verbose:
            print(f"Available evolution plots: {', '.join(plot_modules.keys())}")

        # Filter to selected plots if specified
        if selected_plots:
            plot_modules = {k: v for k, v in plot_modules.items() if k in selected_plots}

        # Create a snapshot-to-redshift mapper
        # Add quiet and verbose flags to params for mapper
        mapper_params = params.params.copy()
        mapper_params["quiet"] = args.quiet
        mapper_params["verbose"] = args.verbose
        mapper = SnapshotRedshiftMapper(args.param_file, mapper_params, model_output_dir)
        if args.verbose:
            print(mapper.debug_info())

        # Create the mapper from parameter file (paths already resolved)
        mapper = SnapshotRedshiftMapper(args.param_file, mapper_params, params["OutputDir"])

        # Determine which snapshots to process
        if args.all_snapshots:
            # Process all available snapshots
            snapshots = mapper.get_all_snapshots()
            if args.verbose:
                print(f"Using all {len(snapshots)} available snapshots")
        elif args.snapshot:
            # Process only the specified snapshot
            # Verify this snapshot exists in our mapping
            if args.snapshot not in mapper.snapshots:
                error(f"Specified snapshot {args.snapshot} not found in redshift mapping")
                print(f"Available snapshots: {mapper.snapshots}")
                sys.exit(1)

            snapshots = [args.snapshot]
            if args.verbose:
                print(f"Using single snapshot: {args.snapshot}")
        else:
            # Use the evolution snapshots determined by the mapper
            # This will prioritize OutputSnapshots from parameter file
            snapshots = mapper.get_evolution_snapshots()

            # Check that we have at least 2 snapshots for a meaningful evolution plot
            if len(snapshots) < 2:
                error("At least 2 snapshots are required for evolution plots")
                print(f"Available snapshots: {snapshots}")
                sys.exit(1)

            # Check for diverse redshift coverage
            redshifts = [mapper.get_redshift(snap) for snap in snapshots]
            min_z = min(redshifts)
            max_z = max(redshifts)

            if args.verbose:
                print(f"Using {len(snapshots)} snapshots for evolution plots")
                print(f"Redshift range: z={min_z:.3f} to z={max_z:.3f}")

        if args.verbose:
            print(f"Selected snapshots for evolution plots: {snapshots}")
            print(
                f"Corresponding redshifts: {[mapper.get_redshift(snap) if snap >= 0 else 0.0 for snap in snapshots]}"
            )

        # Read galaxy data for each snapshot
        snapshot_data = {}
        # Only show progress bar when verbose is enabled
        snapshot_iterator = (
            tqdm(snapshots, desc="Loading snapshot data for evolution plots")
            if args.verbose
            else snapshots
        )
        for snap in snapshot_iterator:
            # Get redshift and model file path from mapper
            redshift = mapper.get_redshift(snap)
            model_file_base = mapper.get_model_file_path(snap, 0).rsplit("_", 1)[
                0
            ]  # Remove file number

            if args.verbose:
                print(f"Processing snapshot {snap} (z={redshift:.3f})")
                print(f"Using model file pattern: {model_file_base}")

            # Required parameters check
            if validate_required_params(
                params.params,
                ["FirstFile", "LastFile", "NumSimulationTreeFiles"],
                "evolution plots",
            ):
                sys.exit(1)

            # Get first and last file numbers, prioritizing command-line arguments
            if args.first_file is not None:
                first_file = args.first_file
                if args.verbose:
                    print(f"Using first_file={first_file} from command-line argument")
            else:
                first_file = params["FirstFile"]
                if args.verbose:
                    print(f"Using first_file={first_file} from parameter file")

            if args.last_file is not None:
                last_file = args.last_file
                if args.verbose:
                    print(f"Using last_file={last_file} from command-line argument")
            else:
                last_file = params["LastFile"]
                if args.verbose:
                    print(f"Using last_file={last_file} from parameter file")

            # Validate file range
            if first_file > last_file:
                error(f"FirstFile ({first_file}) is greater than LastFile ({last_file})")
                sys.exit(1)

            try:
                galaxies, volume, metadata = read_data(
                    model_path=model_file_base,
                    first_file=first_file,
                    last_file=last_file,
                    params=params.params,
                    verbose=args.verbose,
                    quiet=args.quiet,
                )
                # Add redshift to metadata
                metadata["redshift"] = redshift
                snapshot_data[snap] = (galaxies, volume, metadata)
                if args.verbose:
                    print(f"  Read {len(galaxies)} galaxies at z={redshift:.2f}")
            except Exception as e:
                if args.verbose:
                    warn(f"Could not read snapshot {snap}: {e}")
                # Continue to next snapshot - skipped snapshots won't be in the summary

        # Check if we have any snapshot data for evolution plots
        if not snapshot_data:
            evolution_generated_plots = []
            if not args.quiet:
                warn("Skipping evolution plots (no data available)")
        else:
            # Filter evolution plots based on available properties
            # Check properties in first available snapshot as representative sample
            available_plots = {}
            skipped_plots = {}

            # Get a sample galaxy dataset from any snapshot to check properties
            sample_galaxies = None
            if snapshot_data:
                sample_snap = next(iter(snapshot_data.values()))
                sample_galaxies = sample_snap[0]  # galaxies from (galaxies, volume, metadata) tuple

            for plot_name, plot_func in plot_modules.items():
                required_props = PLOT_REQUIREMENTS.get(plot_name, [])
                if required_props and sample_galaxies is not None:
                    # Check if required properties are available
                    props_available, missing_props = check_required_properties(
                        sample_galaxies, required_props
                    )
                    if not props_available:
                        skipped_plots[plot_name] = missing_props
                        continue
                available_plots[plot_name] = plot_func

            # Report skipped plots
            if skipped_plots:
                print(
                    f"\nSkipping {len(skipped_plots)} evolution plot(s) due to missing properties:"
                )
                for plot_name, missing in skipped_plots.items():
                    print(f"  - {plot_name}: missing {', '.join(missing)}")
                print(f"  (Enable physics modules to generate these plots)\n")

            # Generate each evolution plot
            evolution_generated_plots = []
            evolution_skipped_validation = {}  # Track plots skipped due to data validation
            for plot_name, plot_func in available_plots.items():
                try:
                    if args.verbose:
                        print(f"Generating {plot_name}...")
                    result = plot_func(
                        snapshots=snapshot_data,
                        params=params.params,
                        output_dir=output_dir,
                        output_format=args.format,
                        verbose=args.verbose,
                    )

                    plot_path, skip_msg = result
                    if plot_path:
                        evolution_generated_plots.append(plot_path)
                        if not args.quiet:
                            print(f"Created {plot_name} plot")
                    elif skip_msg:
                        evolution_skipped_validation[plot_name] = skip_msg
                        if args.verbose:
                            print(f"Skipped {plot_name}: {skip_msg}")
                except Exception as e:
                    if not args.quiet:
                        print(f"Error generating {plot_name}: {e}")

            if args.verbose:
                print(f"Generated {len(evolution_generated_plots)} evolution plots.")

    # Report validation-based skips if any (before COMPLETE section)
    # Only show in verbose mode
    total_skipped_validation = 0
    if args.snapshot_plots and "snapshot_skipped_validation" in locals():
        total_skipped_validation += len(snapshot_skipped_validation)
    if args.evolution_plots and "evolution_skipped_validation" in locals():
        total_skipped_validation += len(evolution_skipped_validation)

    if args.verbose and total_skipped_validation > 0:
        print_phase("SKIPPED PLOTS")
        print(f"Skipped {total_skipped_validation} plot(s) due to insufficient data")
        print()

        if (
            args.snapshot_plots
            and "snapshot_skipped_validation" in locals()
            and snapshot_skipped_validation
        ):
            print("Snapshot plots:")
            for plot_name, reason in snapshot_skipped_validation.items():
                print(f"  • {plot_name}")
                print(f"    {reason}")
            print()

        if (
            args.evolution_plots
            and "evolution_skipped_validation" in locals()
            and evolution_skipped_validation
        ):
            print("Evolution plots:")
            for plot_name, reason in evolution_skipped_validation.items():
                print(f"  • {plot_name}")
                print(f"    {reason}")
            print()

    # Show completion message for quiet mode
    if args.quiet:
        print("... finished\n")

    # Final completion summary
    if not args.quiet:
        print_phase("COMPLETE")

    total_plots = 0
    if args.snapshot_plots and "snapshot_generated_plots" in locals():
        snapshot_plot_count = len(snapshot_generated_plots)
        total_plots += snapshot_plot_count
        print(f"Snapshot plots  : {snapshot_plot_count}")

    if args.evolution_plots and "evolution_generated_plots" in locals():
        evolution_count = len(evolution_generated_plots)
        total_plots += evolution_count
        print(f"Evolution plots : {evolution_count}")

    print(f"Plots created   : {total_plots}")
    print(f"Skipped plots   : {total_skipped_validation} (run with --verbose flag for details)")
    print(f"Output location : {output_dir}")


if __name__ == "__main__":
    main()
