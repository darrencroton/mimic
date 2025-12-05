# Mimic Plotting Tool

A centralized plotting tool for the Mimic physics-agnostic galaxy evolution framework.

## Overview

This tool provides a single, comprehensive entry point for generating plots from Mimic halo tracking outputs. It features:

- A centralized `mimic-plot.py` script that handles command-line arguments, parameter parsing, and plot management
- Self-contained figure modules in the `figures/` directory, each implementing a specific plot type
- Support for both snapshot plots (single snapshots) and evolution plots (across multiple snapshots)
- Integration with Mimic parameter files for consistent configuration
- Customizable output formatting and figure selection
- Consistent styling and formatting across all plot types
- Robust error handling and fallback mechanisms

## Usage

### Basic Usage

**Important**: Always activate the Python environment before running plots:

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
--output-dir=<dir>     Output directory for plots [default: ./plots]
--format=<format>      Output format (.png, .pdf) [default: .png]
--plots=<list>         Comma-separated list of plots to generate [default: all]
--use-tex              Use LaTeX for text rendering (not recommended)
--verbose              Show detailed output
--help                 Show this help message
```

**Note:** By default, both snapshot and evolution plots are generated if neither `--evolution-plots` nor `--snapshot-plots` is specified.

## Available Plots

### Snapshot Plots (Single Redshift)

- `halo_mass_function`: Halo mass function showing the abundance of halos vs. mass
- `halo_occupation`: Halo occupation distribution
- `spin_distribution`: Distribution of halo spin parameters
- `velocity_distribution`: Distribution of halo velocities
- `spatial_distribution`: Spatial distribution of halos in the simulation volume

### Evolution Plots (Multiple Redshifts)

- `hmf_evolution`: Evolution of the halo mass function across cosmic time

## Working with Units

All Mimic output properties include unit metadata for reproducible science. Units are stored in code units internally (see `docs/developer/unit-system-guide.md`), with metadata available for proper interpretation.

### Accessing Unit Information

**From Python** (works with both binary and HDF5 outputs):
```python
from generated.dtype import get_units

# Get units dictionary
units = get_units()
print(f"Mvir is in: {units['Mvir']}")  # "1e10 Msun/h"
print(f"dT is in: {units['dT']}")      # "Myr"
print(f"Rvir is in: {units['Rvir']}")  # "Mpc/h"

# Use in your analysis
import numpy as np
halos = np.fromfile('model_z0.000_0', dtype=get_binary_dtype())
masses_code_units = halos['Mvir']  # In 10^10 Msun/h
print(f"Mvir range: {masses_code_units.min():.2e} to {masses_code_units.max():.2e} {units['Mvir']}")
```

**From HDF5 files directly** (self-documenting output):
```python
import h5py

with h5py.File('model_000.hdf5', 'r') as f:
    # Read FieldMetadata dataset (recommended)
    metadata = f['Snap016/FieldMetadata'][:]

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

Mimic uses a consistent code unit system:
- **Mass**: `1e10 Msun/h` (10^10 solar masses with Hubble parameter)
- **Length**: `Mpc/h` (megaparsecs comoving with Hubble parameter)
- **Velocity**: `km/s` (kilometers per second)
- **Time**: `Myr` or `Gyr` (megayears or gigayears for output)

The `/h` notation means the value includes the Hubble parameter (h = H0 / 100 km/s/Mpc). To convert to physical units, divide by `h`. For example:
```python
h = 0.73  # From parameter file
mass_physical_msun = halos['Mvir'] * 1e10 / h  # Convert to physical solar masses
```

For detailed information on unit conversions and the internal unit system, see `docs/developer/unit-system-guide.md`.

## Adding New Plot Types

To add a new plot type, follow these steps:

1. **Create a new Python module** in the `figures/` directory with a descriptive name (e.g., `new_plot_type.py`)

2. **Implement the plot function** with the appropriate signature:
   
   For snapshot plots:
   ```python
   def plot(halos, volume, metadata, params, output_dir="plots", output_format=".png"):
       """
       Create your new plot type.

       Args:
           halos: Halo data as a numpy recarray
           volume: Simulation volume in (Mpc/h)^3
           metadata: Dictionary with additional metadata
           params: Dictionary with Mimic parameters
           output_dir: Output directory for the plot
           output_format: File format for the output

       Returns:
           Path to the saved plot file
       """
       # Your implementation here
   ```

   For evolution plots:
   ```python
   def plot(snapshots, params, output_dir="plots", output_format=".png"):
       """
       Create your new evolution plot type.

       Args:
           snapshots: Dictionary mapping snapshot numbers to tuples of (halos, volume, metadata)
           params: Dictionary with Mimic parameters
           output_dir: Output directory for the plot
           output_format: File format for the output

       Returns:
           Path to the saved plot file
       """
       # Your implementation here
   ```

3. **Use consistent styling** by importing and using helper functions from the `figures` package:
   ```python
   from figures import setup_plot_fonts, setup_legend, AXIS_LABEL_SIZE
   ```

4. **Add robust error handling** for empty selections, missing data, and division by zero.

5. **Update `figures/__init__.py`** to include your new module:
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

Here's a minimal example of a new plot module:

```python
#!/usr/bin/env python

"""
Mimic Example Plot

This module generates an example plot from Mimic halo data.
"""

import os
import numpy as np
import matplotlib.pyplot as plt
from figures import setup_plot_fonts, setup_legend, AXIS_LABEL_SIZE

def plot(halos, volume, metadata, params, output_dir="plots", output_format=".png"):
    """
    Create an example plot.

    Args:
        halos: Halo data as a numpy recarray
        volume: Simulation volume in (Mpc/h)^3
        metadata: Dictionary with additional metadata
        params: Dictionary with Mimic parameters
        output_dir: Output directory for the plot
        output_format: File format for the output

    Returns:
        Path to the saved plot file
    """
    # Set up the figure
    fig, ax = plt.subplots(figsize=(8, 6))

    # Apply consistent font settings
    setup_plot_fonts(ax)

    # Your plotting code here...

    # Save the figure
    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, f"ExamplePlot{output_format}")
    plt.savefig(output_path)
    plt.close()

    return output_path
```

## Architecture

The plotting system is organized around these key components:

1. **Master Script (`mimic-plot.py`)**: The central entry point that handles:
   - Command-line argument parsing
   - Parameter file reading
   - Data loading
   - Figure generation coordination
   
2. **Snapshot-Redshift Mapper**: Handles mapping between snapshot numbers and redshifts

3. **Figure Modules**: Self-contained modules in the `figures/` directory:
   - Each implements a specific plot type
   - All have consistent interfaces
   - Can be easily extended with new types

## Setup and Requirements

### Quick Setup

If you used the main Mimic setup script (`../../first_run.sh`), the Python environment is already configured. Simply activate it:

```bash
# From the main Mimic directory
source mimic_venv/bin/activate
cd output/mimic-plot
python mimic-plot.py --param-file=../../input/millennium.yaml
```

### Manual Setup

If you need to set up the plotting environment manually:

#### Requirements
- Python 3.x
- NumPy
- Matplotlib (>=3.0.0)
- tqdm (for progress bars)
- (Optional) LaTeX installation for high-quality text rendering in plots

#### Installation Options

**Option 1 (Recommended): Use the main Mimic requirements.txt:**
```bash
# From the main Mimic directory
cd ../..
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
cd output/mimic-plot
```

**Option 2: Install packages directly:**
```bash
# Using virtual environment (recommended)
python3 -m venv plotting-env
source plotting-env/bin/activate
pip install "numpy>=1.20.0" "matplotlib>=3.0.0" "tqdm>=4.0.0"

# Or using --user flag
pip3 install --user "numpy>=1.20.0" "matplotlib>=3.0.0" "tqdm>=4.0.0"

# Or using system package manager (macOS with Homebrew)
brew install python-numpy python-matplotlib python-tqdm
```

#### Verification

Test your setup:
```bash
python -c "import numpy, matplotlib, tqdm; print('All packages available!')"
```

## License

This tool is part of the Mimic physics-agnostic galaxy evolution framework. Please see the main Mimic license for details.
