# Mimic Plotting Tool

A centralized plotting tool for the Mimic physics-agnostic galaxy evolution framework. This is the detailed plotting manual; for the basic plotting workflow in context, see the [User Guide](../../docs/USER-GUIDE.md#plotting).

## Table of Contents

1. [Overview](#overview)
2. [Setup and Requirements](#setup-and-requirements)
3. [Usage](#usage)
4. [Available Plots](#available-plots)
5. [Working with Units](#working-with-units)
6. [Adding New Plot Types](#adding-new-plot-types)
7. [Testing](#testing)
8. [Architecture](#architecture)
9. [Documentation Directory](#documentation-directory)
10. [License](#license)

## Overview

This tool provides a single, comprehensive entry point for generating plots from Mimic halo tracking outputs. It features:

- A centralized `mimic-plot.py` script that handles command-line arguments, parameter parsing, and plot management
- Self-contained figure modules in the active model package, such as `models/<MODEL>/plots/figures/`, each implementing a specific plot type
- Support for both snapshot plots (single snapshots) and evolution plots (across multiple snapshots)
- Integration with Mimic parameter files for consistent configuration
- Customizable output formatting and figure selection
- Consistent styling and formatting across all plot types
- Robust error handling and fallback mechanisms

## Setup and Requirements

### Quick Setup

If you used the main Mimic setup script (`../../scripts/first_run.sh`), the Python environment is already configured. Activate it before running plots:

```bash
# From the main Mimic directory
source mimic_venv/bin/activate
cd plot/mimic-plot
python mimic-plot.py --param-file=../../models/sage16/input/sage16_mini-millennium.yaml
```

### Manual Setup

Requirements:

- Python 3.9+
- NumPy
- Matplotlib (>=3.0.0)
- tqdm (for progress bars)
- h5py for HDF5 output
- PyYAML for run files and plot profiles
- Optional: LaTeX installation for text rendering in plots

Recommended setup from the main Mimic requirements:

```bash
# From the main Mimic directory
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
cd plot/mimic-plot
```

Direct package installation also works for standalone plotting environments:

```bash
python3 -m venv plotting-env
source plotting-env/bin/activate
pip install "numpy>=1.20.0" "matplotlib>=3.0.0" "tqdm>=4.0.0" "h5py>=3.0.0" "PyYAML>=5.0.0"
```

Verify the environment:

```bash
python -c "import numpy, matplotlib, tqdm, h5py, yaml; print('All plotting packages available')"
```

## Usage

### Basic Usage

Build Mimic with the same model set as the parameter file before plotting. The available plot registry is model-specific, so a SHAM run should be built with `make MODEL=sham`, the shipped SAGE run with `make MODEL=sage16`, and halo-catalogue-only runs with `make MODEL=halos-only`. Binary outputs must be kept with their run `metadata/` directory because `mimic-plot` reads `metadata/output_schema.json`.

```bash
# Activate virtual environment (if using one)
source ../../mimic_venv/bin/activate  # or source plotting-env/bin/activate

# Generate both snapshot and evolution plots (default behavior)
python mimic-plot.py --param-file=/path/to/mimic_params.yaml

# Generate specific plots from both types
python mimic-plot.py --param-file=/path/to/mimic_params.yaml --plots=halo_mass_function,hmf_evolution

# Generate only snapshot plots
python mimic-plot.py --param-file=/path/to/mimic_params.yaml --snapshot-plots

# Generate only evolution plots
python mimic-plot.py --param-file=/path/to/mimic_params.yaml --evolution-plots

# Specify file range and output options
python mimic-plot.py --param-file=/path/to/mimic_params.yaml --first-file=0 --last-file=7 --output-dir=my_plots --format=.pdf

# Deactivate virtual environment when done
deactivate
```

### Command-Line Options

```
--param-file=<file>    Mimic parameter file (required)
--first-file=<num>     First file to read [default: 0]
--last-file=<num>      Last file to read [default: use LastFile from param file]
--snapshot=<num>       Process only this snapshot number
--all-snapshots        Process all available snapshots
--evolution-plots      Generate evolution plots only
--snapshot-plots       Generate snapshot plots only
--output-dir=<dir>     Output directory for plots [default: <OutputDir>/plots]
--format=<format>      Output format (.png, .pdf) [default: .png]
--plots=<list>         Comma-separated list of plots to generate [default: all]
--use-tex              Use LaTeX for text rendering (not recommended)
--verbose, -v          Show detailed output including skipped plots
--quiet, -q            Show minimal output (only summary)
--input-format=<fmt>   Input data format: 'mimic' (default) or 'sage-hdf5'.
                       See "Plotting native SAGE output" below.
--help                 Show this help message
```

### Plotting native SAGE output

For debugging the Mimic SAGE module port it is useful to run the same
plots over native [sage-model](https://github.com/sage-home/sage-model)
HDF5 output. Pass `--input-format=sage-hdf5` and point `output_directory`
in your Mimic YAML parameter file at the SAGE output directory:

```bash
python mimic-plot.py --param-file=sage.yaml --input-format=sage-hdf5
```

The SAGE-native reader (`sage_native_hdf5.py`) is an isolated add-on. It
maps SAGE field names to Mimic field names (e.g. `IntraClusterStars` →
`ICS`, `EjectedMass` → `EjectedGas`), reconstructs `Pos`/`Vel`/`Spin`
from the per-component datasets SAGE writes, and combines
`SfrDisk + SfrBulge` into Mimic's `StarFormationRate`. Mimic-only fields
that SAGE does not export remain zero-filled and the standard validation
helpers automatically skip plots that need them. To remove SAGE-native
support, delete `sage_native_hdf5.py` and the `>>> SAGE-NATIVE-HDF5 >>>`
... `<<< SAGE-NATIVE-HDF5 <<<` blocks in `mimic-plot.py`.

**Note:** By default, both snapshot and evolution plots are generated if neither `--evolution-plots` nor `--snapshot-plots` is specified.

### Output Verbosity

The tool supports three verbosity levels:

- **Normal** (default): Shows standard progress messages and created plots
- **Verbose** (`--verbose` or `-v`): Shows detailed information including data loading progress, validation details, and a SKIPPED PLOTS section with reasons for any skipped plots
- **Quiet** (`--quiet` or `-q`): Shows only the final summary with plot counts and output location

## Available Plots

The plot registry is model-specific: it lives in the active model package at `models/<MODEL>/plots/figures/__init__.py`, which is the source of truth for registered plot names. The lists below summarise the shipped sage16 registry (18 snapshot plots and 4 evolution plots); the sham package registers a smaller diagnostic set.

### Snapshot Plots (Single Redshift)

**Halo Property Plots** (always available):
- `halo_mass_function`: Halo mass function showing abundance vs. mass
- `halo_occupation`: Halo occupation distribution
- `spin_distribution`: Distribution of halo spin parameters
- `velocity_distribution`: Distribution of halo velocities
- `spatial_distribution`: Spatial distribution of halos in simulation volume

**Galaxy Physics Plots** (require physics modules):
- `stellar_mass_function`: Stellar mass function
- `cold_gas_function`: Cold gas mass function
- `baryon_fraction`: Baryon fraction as function of halo mass
- `baryonic_mass_function`: Baryonic mass function
- `gas_mass_function`: Gas mass function
- `baryonic_tully_fisher`: Baryonic Tully-Fisher relation
- `specific_sfr`: Specific star formation rate distribution
- `black_hole_bulge_relation`: Black hole mass vs. bulge mass relation
- `gas_fraction`: Gas fraction as function of stellar mass
- `metallicity`: Metallicity distribution
- `bulge_mass_fraction`: Bulge-to-total mass ratio
- `quiescent_fraction`: Quiescent galaxy fraction
- `mass_reservoir_scatter`: Scatter in mass reservoirs

### Evolution Plots (Multiple Redshifts)

**Halo Property Evolution** (always available):
- `hmf_evolution`: Evolution of halo mass function across cosmic time

**Galaxy Physics Evolution** (require physics modules):
- `smf_evolution`: Stellar mass function evolution
- `sfr_density_evolution`: Star formation rate density evolution
- `stellar_mass_density_evolution`: Stellar mass density evolution

## Working with Units

All Mimic output properties include unit metadata for reproducible science. Units are stored in code units internally, with output unit labels generated from property metadata and exposed through HDF5 `FieldMetadata` or the run-local `metadata/output_schema.json`.

### Accessing Unit Information

**From binary output schema**:
```python
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, "/path/to/mimic/plot/mimic-plot")
from output_schema import descriptions_from_schema, dtype_from_schema, units_from_schema

output_file = Path("model_z0.000_0")
schema = json.loads((output_file.parent / "metadata" / "output_schema.json").read_text())
units = units_from_schema(schema)
descriptions = descriptions_from_schema(schema)  # {field: human-readable description}
print(f"Mvir is in: {units['Mvir']}")  # "1e10 Msun/h"
print(f"dT is in: {units['dT']}")      # "Myr/h"
print(f"Rvir is in: {units['Rvir']}")  # "Mpc/h"
print(f"Mvir: {descriptions['Mvir']}")  # human-readable field description

with output_file.open("rb") as handle:
    ntrees = np.fromfile(handle, dtype=np.int32, count=1)[0]
    ngalaxies = np.fromfile(handle, dtype=np.int32, count=1)[0]
    np.fromfile(handle, dtype=np.int32, count=ntrees)  # halos per tree
    halos = np.fromfile(handle, dtype=dtype_from_schema(schema, binary=True), count=ngalaxies)
masses_code_units = halos['Mvir']  # In 10^10 Msun/h
print(f"Mvir range: {masses_code_units.min():.2e} to {masses_code_units.max():.2e} {units['Mvir']}")
```

**From HDF5 files directly** (self-documenting output):
```python
import h5py

with h5py.File('model_000.hdf5', 'r') as f:
    # Read FieldMetadata dataset (recommended) - written once per file
    metadata = f['RunProperties/FieldMetadata'][:]

    # Convert to dictionary for easy lookup
    units_dict = {row['field_name'].decode(): row['units'].decode()
                  for row in metadata}

    print(f"Mvir units: {units_dict['Mvir']}")

    # FieldMetadata also includes descriptions for each property
    for row in metadata[:5]:  # First 5 properties
        field = row['field_name'].decode()
        units = row['units'].decode()
        desc = row['description'].decode()
        print(f"{field}: {units} - {desc}")

    # Load data with unit awareness
    halos = f['Snap016/Galaxies'][:]
    print(f"Loaded {len(halos)} halos")
    print(f"Mvir is in {units_dict['Mvir']}")
```

### Unit Conventions

Mimic derives runtime code units from `simulation.units` in the active simulation package and writes output unit labels from property metadata. The shipped Millennium-family examples use these common conventions:

- **Mass**: `1e10 Msun/h` (10^10 solar masses with Hubble parameter)
- **Length**: `Mpc/h` (megaparsecs comoving with Hubble parameter)
- **Velocity**: `km/s` (kilometers per second)
- **Time**: `Myr/h` or `Gyr/h` (megayears or gigayears for output)

The `/h` notation means the value includes the Hubble parameter (h = H0 / 100 km/s/Mpc). To convert to physical units, divide by `h`. For example:
```python
h = 0.73  # From parameter file
mass_physical_msun = halos['Mvir'] * 1e10 / h  # Convert to physical solar masses
```

Do not assume these labels are universal for every simulation or model package. For run-level unit configuration, see the `simulation.units` section described in [docs/USER-GUIDE.md](../../docs/USER-GUIDE.md), and treat HDF5 `FieldMetadata` or `metadata/output_schema.json` as the source of truth for a completed run.

## Adding New Plot Types

To add a new plot type, follow these steps:

1. **Create a new Python module** in the active model package's `plots/figures/` directory (e.g. `models/sage16/plots/figures/new_plot_type.py`) with a descriptive name

2. **Implement the plot function** with the appropriate signature and automatic validation:

   For snapshot plots:
   ```python
   from output_utils import (
       check_field_has_values,
       check_required_fields,
       save_and_close_figure,
       setup_figure,
       validate_filtered_data,
       warn,
   )

   def plot(galaxies, volume, metadata, params, output_dir="plots", output_format=".png", verbose=False):
       """
       Create your new plot type.

       Args:
           galaxies: Galaxy data as a numpy recarray
           volume: Simulation volume in (Mpc/h)^3
           metadata: Dictionary with additional metadata
           params: Dictionary with Mimic parameters
           output_dir: Output directory for the plot
           output_format: File format for the output
           verbose: Whether to print verbose output

       Returns:
           Tuple of (plot_path, skip_message):
               - plot_path (str or None): Path to saved plot file if successful
               - skip_message (str or None): Reason for skipping if validation failed
       """
       # 1. Check required fields
       success, optional, msg = check_required_fields(
           galaxies,
           required_fields=['SomeField'],
           plot_name='Your Plot Name'
       )
       if not success:
           return None, f"Required fields missing: {msg}"

       # 2. Field-level validation (automatic check for all-zero fields)
       has_values, count, msg = check_field_has_values(
           galaxies.SomeField, 'SomeField', threshold=0.0
       )
       if not has_values:
           return None, f"Field validation failed: {msg}"

       # 3. Filter data
       w = np.where(galaxies.SomeField > 0.0)[0]

       # 4. Filter-level validation (automatic check for empty results)
       is_valid, skip_msg = validate_filtered_data(w, "Your Plot Name", verbose)
       if not is_valid:
           return None, skip_msg

       # 5. NOW create the figure (only after validation passes)
       fig, ax = setup_figure()

       # 6. Your plotting code here...

       # 7. Save and return tuple
       plot_path = save_and_close_figure(fig, output_dir, "PlotFileName", output_format, verbose)
       return plot_path, None
   ```

   For evolution plots:
   ```python
   from output_utils import (
       check_field_has_values,
       check_required_fields,
       save_and_close_figure,
       setup_figure,
       validate_evolution_snapshot,
       warn,
   )

   def plot(snapshots, params, output_dir="plots", output_format=".png", verbose=False):
       """
       Create your new evolution plot type.

       Args:
           snapshots: Dictionary mapping snapshot numbers to tuples of (galaxies, volume, metadata)
           params: Dictionary with Mimic parameters
           output_dir: Output directory for the plot
           output_format: File format for the output
           verbose: Whether to print verbose output

       Returns:
           Tuple of (plot_path, skip_message):
               - plot_path (str or None): Path to saved plot file if successful
               - skip_message (str or None): Reason for skipping if validation failed
       """
       # 1. Check if we have snapshots
       if len(snapshots) == 0:
           return None, "No snapshot data available"

       # 2. Validate using first snapshot as sample
       first_snap = next(iter(snapshots.values()))
       galaxies_sample = first_snap[0]

       success, optional, msg = check_required_fields(
           galaxies_sample,
           required_fields=['SomeField'],
           plot_name='Your Evolution Plot'
       )
       if not success:
           return None, f"Required fields missing: {msg}"

       # 3. Field-level validation
       has_values, count, msg = check_field_has_values(
           galaxies_sample.SomeField, 'SomeField', threshold=0.0
       )
       if not has_values:
           return None, f"Field validation failed: {msg}"

       # 4. Create figure AFTER validation
       fig, ax = setup_figure()

       # 5. Loop through snapshots
       for snap, (galaxies, volume, metadata) in snapshots.items():
           redshift = metadata['redshift']

           # Filter data
           w = np.where(galaxies.SomeField > 0.0)[0]

           # Validate this snapshot (skip if no data)
           is_valid, skip_msg = validate_evolution_snapshot(w, redshift, "Your Plot", verbose)
           if not is_valid:
               continue  # Skip this snapshot, continue with others

           # Plot this snapshot...

       # 6. Save and return tuple
       plot_path = save_and_close_figure(fig, output_dir, "PlotFileName", output_format, verbose)
       return plot_path, None
   ```

3. **Use consistent styling** by importing and using helper functions from the `figures` package:
   ```python
   from figures import setup_plot_fonts, setup_legend, AXIS_LABEL_SIZE
   ```

4. **Automatic validation** - The validation helpers provide three levels of automatic checking:
   - **Property existence**: `check_required_fields()` verifies fields are present
   - **Field-level validation**: `check_field_has_values()` checks for all-zero fields
   - **Filter-level validation**: `validate_filtered_data()` / `validate_evolution_snapshot()` check for empty results

   When validation fails, plots are automatically skipped and reported to the user with clear reasons.

5. **Update the model package's `plots/figures/__init__.py`** to include your new module:
   ```python
   # Add import
   from . import new_plot_type
   
   # Add to the appropriate list
   SNAPSHOT_PLOTS.append('new_plot_type')
   # or
   EVOLUTION_PLOTS.append('new_plot_type')
   
   # Add to the mapping
   PLOT_FUNCS['new_plot_type'] = new_plot_type.plot
   ```

6. **Test your plot** with the central script:
   ```bash
   # Activate environment first
   source ../../mimic_venv/bin/activate
   python mimic-plot.py --param-file=params.yaml --plots=new_plot_type --verbose
   ```

### Example Implementation

Here's a complete example of a new plot module with automatic validation:

```python
#!/usr/bin/env python

"""
Mimic Example Plot

This module generates an example plot from Mimic halo data.
"""

import numpy as np
from figures import setup_plot_fonts, setup_legend, AXIS_LABEL_SIZE
from output_utils import (
    check_field_has_values,
    check_required_fields,
    save_and_close_figure,
    setup_figure,
    validate_filtered_data,
    warn,
)

def plot(galaxies, volume, metadata, params, output_dir="plots", output_format=".png", verbose=False):
    """
    Create an example plot.

    Args:
        galaxies: Galaxy data as a numpy recarray
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output
        verbose: Whether to print verbose output

    Returns:
        Tuple of (plot_path, skip_message):
            - plot_path (str or None): Path to saved plot file if successful
            - skip_message (str or None): Reason for skipping if validation failed
    """
    # Check required fields
    success, optional, msg = check_required_fields(
        galaxies,
        required_fields=['Mvir'],
        plot_name='Example Plot'
    )
    if not success:
        return None, f"Required fields missing: {msg}"

    # Field-level validation
    has_mvir, count, msg = check_field_has_values(
        galaxies.Mvir, 'Mvir', threshold=0.0
    )
    if not has_mvir:
        return None, f"Field validation failed: {msg}"

    # Filter data
    w = np.where((galaxies.Type == 0) & (galaxies.Mvir > 0.0))[0]

    # Filter-level validation
    is_valid, skip_msg = validate_filtered_data(w, "Example Plot", verbose)
    if not is_valid:
        return None, skip_msg

    # NOW create the figure (only after validation passes)
    fig, ax = setup_figure()

    # Apply consistent font settings
    setup_plot_fonts(ax)

    # Your plotting code here...
    mass = np.log10(galaxies.Mvir[w] * 1.0e10)
    ax.hist(mass, bins=50)
    ax.set_xlabel("log$_{10}$(M$_{vir}$ / M$_{\odot}$)", fontsize=AXIS_LABEL_SIZE)
    ax.set_ylabel("Count", fontsize=AXIS_LABEL_SIZE)

    # Save and return tuple
    plot_path = save_and_close_figure(fig, output_dir, "ExamplePlot", output_format, verbose)
    return plot_path, None
```

## Testing

The plotting system includes comprehensive tests to ensure reliability and correctness.

### Running Tests

```bash
# Activate Python environment first
source ../../mimic_venv/bin/activate

# Run validation helper unit tests
cd tests
python3 test_validation_helpers.py

# Run plotting system integration tests
./test_plotting.sh

# Both from mimic root
cd ../..
python3 plot/mimic-plot/tests/test_validation_helpers.py
plot/mimic-plot/tests/test_plotting.sh
```

### Test Suite

- **`test_validation_helpers.py`**: Unit tests for plot validation functions (13 tests covering edge cases, thresholds, and error handling)
- **`test_plotting.sh`**: Integration tests for the plotting pipeline (5 tests covering different command-line options and plot types)

## Architecture

The plotting system is organized around these key components:

1. **Master Script (`mimic-plot.py`)**: The central entry point that handles:
   - Command-line argument parsing
   - Parameter file reading
   - Data loading
   - Figure generation coordination
   - Plot profile loading, with inherited profiles resolved relative to the profile file that declares them

2. **Snapshot-Redshift Mapper**: Handles mapping between snapshot numbers and redshifts

3. **Figure Modules**: Self-contained modules in the `figures/` directory:
   - Each implements a specific plot type
   - All have consistent interfaces
   - Can be easily extended with new types

## Documentation Directory

- [README.md](../../README.md): project overview and shortest path to a first result
- [docs/VISION.md](../../docs/VISION.md): architectural principles and design boundaries
- [docs/USER-GUIDE.md](../../docs/USER-GUIDE.md): installation, run configuration, output analysis, plotting, and troubleshooting
- [docs/DEVELOPER-GUIDE.md](../../docs/DEVELOPER-GUIDE.md): extending models, modules, simulations, properties, tests, and generated metadata
- [tests/README.md](../../tests/README.md): test-suite quick reference
- `models/<model>/README.md`: model-package science scope, module pipeline, parameters, plots, and references
- `simulations/<simulation>/README.md`: simulation-package data, units, snapshot lists, and maintenance notes

## License

This tool is part of the Mimic physics-agnostic galaxy evolution framework. Please see the main Mimic license for details.
