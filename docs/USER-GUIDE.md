# Mimic User Guide

**Complete guide to installing, configuring, and running Mimic simulations**

---

## Table of Contents

1. [Installation](#installation)
2. [Running Simulations](#running-simulations)
3. [Configuration](#configuration)
4. [Output](#output)
5. [Plotting Results](#plotting-results)
6. [Troubleshooting](#troubleshooting)
7. [FAQ](#faq)

---

## Installation

### Quick Start (Recommended)

**Automated setup** - fastest way to get started:

```bash
# Clone repository
git clone [repository-url]
cd mimic

# One-command setup (creates directories, downloads data, sets up Python)
./scripts/first_run.sh

# Build
make

# Test
./mimic input/millennium.yaml
echo $?  # Should output: 0 (success)
```

The `first_run.sh` script:
- Creates required directories
- Downloads mini-Millennium test data (~50MB)
- Creates Python virtual environment (`mimic_venv`)
- Installs plotting dependencies (numpy, matplotlib, pyyaml, h5py)
- Updates paths in `input/millennium.yaml` to absolute paths

### Prerequisites

**Required**:
- C compiler (gcc 4.8+ or clang)
- GNU Make
- Python 3.6+ with pip

**Optional**:
- HDF5 libraries (for HDF5 output format, recommended)
- MPI libraries (for parallel processing)
- clang-format (for code formatting)
- black and isort (for Python code formatting)

### Manual Installation

If automated setup fails or you need custom configuration:

**1. Create directories**:
```bash
mkdir -p input/data/millennium output/results/millennium
```

**2. Download test data**:
```bash
cd input/data/millennium
wget "https://www.dropbox.com/s/l5ukpo7ar3rgxo4/mini-millennium-treefiles.tar?dl=0" \
     -O mini-millennium-treefiles.tar
tar -xf mini-millennium-treefiles.tar
rm mini-millennium-treefiles.tar
cd ../../..
```

**3. Set up Python environment** (for plotting):
```bash
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
deactivate
```

**4. Update configuration**:

Edit `input/millennium.yaml` and set **absolute paths** for:
- `output.output_directory`
- `input.simulation_dir`

**5. Build and test**:
```bash
make
./mimic input/millennium.yaml
echo $?  # Should output: 0 (success)
```

### Build Options

**Standard build** (HDF5 enabled by default):
```bash
make
```

**Faster compilation** (parallel build):
```bash
make -j$(nproc)  # Use all CPU cores
make -j4         # Use 4 cores
```

**Disable HDF5** (binary output only):
```bash
make USE-HDF5=no
```

**Enable MPI** (requires MPI libraries):
```bash
make USE-MPI=yes
```

**Clean builds**:
```bash
make clean  # Remove all build artifacts
make tidy   # Remove object files, keep executable
```

**Show build configuration**:
```bash
make info  # Shows compiler, libraries, features
```

---

## Running Simulations

### Basic Usage

```bash
./mimic <parameter_file.yaml>
```

Example:
```bash
./mimic input/millennium.yaml
```

### Command-Line Options

**Logging verbosity**:

| Flag | Alias | Output Level | Use For |
|------|-------|--------------|---------|
| (default) | - | INFO, WARNING, ERROR | Normal runs |
| `--verbose` | `-v` | + context (timestamp, file:line) | Detailed progress |
| `--debug` | `-d` | + DEBUG messages | Troubleshooting |
| `--quiet` | `-q` | WARNING, ERROR only | Production runs, scripts |

Examples:
```bash
./mimic input/millennium.yaml                # Standard output
./mimic --verbose input/millennium.yaml       # Verbose (with context)
./mimic --debug input/millennium.yaml         # Maximum detail
./mimic --quiet input/millennium.yaml         # Minimal output
```

**Skip existing output**:
```bash
./mimic --skip input/millennium.yaml   # Don't overwrite existing files
```

### Checking Success

Mimic returns exit code 0 on success:

```bash
./mimic input/millennium.yaml
echo $?  # 0 = success, non-zero = failure
```

**Successful execution**:
```
[INFO] Mimic v[version]
[INFO] Reading parameter file: input/millennium.yaml
[INFO] Initializing modules...
[INFO] Processing trees...
[INFO] Writing output...
[INFO] Mimic completed successfully
```

### Example Workflows

**Production run** (quiet, HDF5 output):
```bash
make clean
make -j$(nproc)                   # Fast parallel build
./mimic -q input/millennium.yaml  # Quiet mode for cleaner logs
```

**Debug run** (maximum verbosity, save logs):
```bash
./mimic --debug input/millennium.yaml 2>&1 | tee debug.log
```

**Quick test** (physics-free mode, fast halo tracking only):
```yaml
# Edit millennium.yaml to disable all physics modules
modules:
  pre_timestep: []
  phase_1: []
  phase_2: []
  post_timestep: []
  parameters: {}
```
```bash
./mimic input/millennium.yaml  # Fast execution
```

---

## Configuration

### YAML Configuration File

Mimic uses YAML format for runtime configuration. Main sections:

```yaml
# Output configuration
output:
  output_filename: model              # Base filename (no extension)
  output_directory: ./output/results/ # Output directory
  output_format: hdf5                 # 'binary' or 'hdf5'
  snapshot_list: [63, 37, 32, 27]     # Snapshots to process

# Input files
input:
  tree_type: lhalo_binary             # 'lhalo_binary' or 'genesis_lhalo_hdf5'
  simulation_dir: ./input/data/       # Merger tree directory
  first_file: 0                       # First file to process
  last_file: 7                        # Last file (inclusive)
  tree_name: trees_063                # Tree file base name
  snapshot_list_file: ./input/data/millennium.a_list
  last_snapshot: 63

# Simulation properties
simulation:
  cosmology:
    omega_matter: 0.25                # Ωm
    omega_lambda: 0.75                # ΩΛ
    hubble_h: 0.73                    # h (H0 = 100h km/s/Mpc)
  box_size: 62.5                      # Mpc/h
  particle_mass: 0.0860657            # 10^10 Msun/h

# Time sub-stepping (optional, default: 1)
SubSteps: 1                           # Number of substeps per snapshot

# Multi-phase pipeline (physics modules)
modules:
  pre_timestep: []
  phase_1: []
  phase_2: []
  post_timestep: []
  parameters: {}
```

See `input/millennium.yaml` for a complete working example.

### Configuring Physics Modules

Mimic uses a **multi-phase pipeline** architecture with four execution phases:

**Phase execution order**:
```
For each snapshot interval:
  1. pre_timestep  (runs once before substeps)
  2. Loop over SubSteps:
     a. phase_1    (runs each substep)
     b. phase_2    (runs each substep)
  3. post_timestep (runs once after substeps)
```

**Example configuration** (SAGE physics):

```yaml
SubSteps: 10  # Time sub-stepping (1 = no substeps, 10 = SAGE-like)

modules:
  # Setup phase (runs once)
  pre_timestep:
    - sage_reionization: process_full_halo
    - sage_calculate_infall: process_full_halo

  # Main physics (runs each substep)
  phase_1:
    - sage_add_infall: process_full_halo
    - sage_calculate_cooling: process_by_galaxy
    - sage_radio_mode_heating: process_by_galaxy
    - sage_add_cooling: process_by_galaxy
    - sage_starformation_feedback: process_by_galaxy

  # Secondary physics (runs each substep)
  phase_2:
    - sage_mergers: process_by_galaxy

  # Finalization (runs once)
  post_timestep: []

  # Physics parameters (required by enabled modules)
  parameters:
    GlobalBaryonFraction: 0.17
    RadioModeEfficiency: 0.01
    AGNrecipe: 1
    SfrEfficiency: 0.02
    FeedbackReheatingEpsilon: 3.0
    FeedbackEjectionEfficiency: 0.3
    RecycleFraction: 0.43
    Yield: 0.03
```

**Processing modes**:
- `process_full_halo`: Module processes entire galaxy array at once
- `process_by_galaxy`: Core loops over galaxies, module processes one at a time

**Time sub-stepping**:
- `SubSteps: 1` (default): No sub-stepping, fastest
- `SubSteps: 10`: Moderate stability, good balance
- `SubSteps: 20`: SAGE-like, most stable (slower)

Higher values improve numerical stability for time-dependent physics.

### Available Physics Modules

**SAGE physics modules** (complete SAGE implementation):

| Module | Phase | Description |
|--------|-------|-------------|
| `sage_reionization` | pre_timestep | Reionization suppression for low-mass halos |
| `sage_calculate_infall` | pre_timestep | Calculate cosmological gas infall budget |
| `sage_update_disk_radius` | pre_timestep | Update disk scale radius |
| `sage_add_infall` | phase_1 | Add infalling gas to central galaxies |
| `sage_reincorporation` | phase_1 | Reincorporation of ejected gas |
| `sage_satellite_stripping` | phase_1 | Environmental stripping for satellites |
| `sage_calculate_cooling` | phase_1 | Calculate cooling budget from hot halo |
| `sage_radio_mode_heating` | phase_1 | AGN radio-mode feedback suppresses cooling |
| `sage_add_cooling` | phase_1 | Transfer cooled gas to cold reservoir |
| `sage_calculate_star_formation` | phase_1 | Calculate star formation rate |
| `sage_calculate_supernova_feedback` | phase_1 | Calculate supernova feedback |
| `sage_update_star_formation_supernova` | phase_1 | Apply star formation and feedback |
| `sage_mergers` | phase_2 | Galaxy mergers and black hole growth |
| `sage_disk_instability` | phase_1 | Disk instability and bulge formation |

### Physics-Free Mode

To run halo tracking without galaxy physics:

```yaml
modules:
  pre_timestep: []
  phase_1: []
  phase_2: []
  post_timestep: []
  parameters: {}  # No parameters needed
```

Useful for:
- Testing infrastructure
- When only halo properties are needed
- Fastest execution

---

## Output

### Output Formats

Mimic supports two output formats with identical data:

**Binary** (fast, compact):
- 3.5× faster than HDF5
- Requires matching reader code
- File naming: `model_z{redshift}_{filenr}` (e.g., `model_z0.000_0`)
- **Use for**: production runs, performance-critical workflows

**HDF5** (self-documenting, portable):
- Standard format readable by many tools
- Self-describing with metadata
- Requires HDF5 libraries (enabled by default)
- File naming: `model_{filenr}.hdf5` (e.g., `model_000.hdf5`)
- **Use for**: data sharing, long-term archival, exploratory analysis

**Select format** in input YAML:
```yaml
output:
  output_format: binary  # or 'hdf5'
```

### Output Contents

**Always included** (core halo properties):
- Halo properties: Mvir, Rvir, Vmax, Spin, Position, Velocity
- Tree structure: TreeID, DescID, HaloIndex
- Identifiers: UniqueGalaxyID, MostBoundID

**Conditionally included** (galaxy properties from enabled modules):
- Baryonic masses: ColdGas, HotGas, StellarMass, BulgeMass, EjectedGas
- Metals: MetalsColdGas, MetalsHotGas, MetalsStellarMass, MetalsBulgeMass
- Star formation: Sfr, NewStarsMass
- Black holes: BlackHoleMass, QuasarModeBHaccretionMass
- Structure: DiskScaleRadius
- Merger history: TimeOfLastMajorMerger, TimeOfLastMinorMerger

Properties are output only if set by enabled physics modules.

### Reading Output

**Using mimic-plot** (recommended, auto-detects format):

```bash
source mimic_venv/bin/activate

# Generate all plots (auto-detects binary or HDF5)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml

# Generate specific plots
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --plots=halo_mass_function,stellar_mass_function

# Only snapshot plots (18 plots: 5 halo + 13 galaxy)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --snapshot-plots

# Only evolution plots (4 plots: 1 halo + 3 galaxy)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --evolution-plots

deactivate
```

**Custom Python code** (binary):

```python
from generated_dtype import get_binary_dtype, get_units
import numpy as np

# Load data
dtype = get_binary_dtype()
halos = np.fromfile('output/results/millennium/model_z0.000_0', dtype=dtype)

# Access properties
masses = halos['Mvir']  # Units: 1e10 Msun/h
radii = halos['Rvir']   # Units: Mpc/h
stellar = halos['StellarMass']  # Units: 1e10 Msun/h

# Get units
units = get_units()
print(f"Mvir units: {units['Mvir']}")  # "1e10 Msun/h"

# Convert to physical units
h = 0.73  # Hubble parameter
mass_physical_msun = masses * 1e10 / h  # Solar masses (no h)
```

**Custom Python code** (HDF5):

```python
import h5py
import numpy as np

with h5py.File('output/results/millennium/model_000.hdf5', 'r') as f:
    # Read z=0 snapshot
    halos = f['Snap063/Galaxies'][:]

    # Access properties
    masses = halos['Mvir']
    stellar = halos['StellarMass']

    # Read metadata
    metadata = f['Snap063/FieldMetadata'][:]
    for row in metadata[:5]:
        field = row['field_name'].decode()
        units = row['units'].decode()
        desc = row['description'].decode()
        print(f"{field}: {units} - {desc}")

    # Read run configuration
    enabled_modules = f['RunProperties/EnabledModules'][:]
    parameters = f['RunProperties/Parameters'][:]
    git_commit = f['RunProperties/Version'].attrs['git_commit']

    # Get redshifts
    redshifts = f['RunProperties/Redshifts'][:]
```

### HDF5 Output Structure

**Master file** (`model.hdf5`):
```
/RunProperties/
  ├── @BoxSize, @Hubble_h, @Omega (simulation parameters)
  ├── Version/ (@git_commit, @git_branch, @hdf5_format_version)
  ├── EnabledModules (dataset: module_name, phase, processing_mode)
  ├── Parameters (dataset: param_name, value pairs)
  └── Redshifts (dataset: z for each snapshot)

/Snap063/ (z=0)
  ├── FieldMetadata (dataset: field descriptions)
  └── File000/ → external link to model_000.hdf5
```

**Per-file output** (`model_000.hdf5`):
```
/RunProperties/ (same as master - self-contained)
/Snap063/
  ├── FieldMetadata (structured table: field_name, units, description)
  ├── Galaxies (compound dataset with all properties)
  │   ├── @Ntrees (attribute: number of trees in this file)
  │   └── @TotHalosPerSnap (attribute: total halos at this snapshot)
  └── TreeHalosPerSnap (dataset: halos per tree array)
```

**Benefits**:
- **Self-contained**: Each file has complete metadata
- **Reproducible**: Version info and parameters stored
- **Self-documenting**: FieldMetadata describes every field
- **No external dependencies**: Redshifts included (no need for .a_list file)

### Unit Conventions

| Quantity | Code Units | Example Properties |
|----------|-----------|-------------------|
| Mass | `1e10 Msun/h` | Mvir, StellarMass, ColdGas |
| Length | `Mpc/h` | Rvir, Pos, DiskScaleRadius |
| Velocity | `km/s` | Vvir, Vmax, Vel |
| Time | `Myr` (output), `Gyr/h` (internal) | dT, TimeOfLastMajorMerger |

**Converting to physical units**:
```python
h = 0.73  # From parameter file Hubble_h

# Remove h dependence
mass_physical_msun = halos['Mvir'] * 1e10 / h  # Solar masses (no h)
length_physical_mpc = halos['Rvir'] / h         # Mpc (no h)

# Velocity doesn't have h dependence
velocity_km_s = halos['Vvir']  # Already in km/s
```

---

## Plotting Results

### Quick Start

```bash
# Activate Python environment
source mimic_venv/bin/activate

# Generate all plots (18 snapshot + 4 evolution = 22 total)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml

# Deactivate when done
deactivate
```

Plots are saved to `output/results/millennium/plots/`.

### Available Plots

**Snapshot plots** (18 total, at z=0):

*Halo properties* (5 plots):
- Halo mass function
- Vmax distribution
- Spin distribution
- Concentration distribution
- Virial velocity distribution

*Galaxy physics* (13 plots):
- Stellar mass function
- Cold gas mass function
- Hot gas mass function
- Stellar metallicity distribution
- Cold gas metallicity distribution
- Star formation rate function
- Specific star formation rate
- Stellar mass vs halo mass
- Cold gas fraction vs stellar mass
- Bulge-to-total ratio vs stellar mass
- Black hole mass vs stellar mass
- Outflow rate vs stellar mass
- Disk scale radius vs stellar mass

**Evolution plots** (4 total, z=0 to z~3):

*Halo properties* (1 plot):
- Halo mass function evolution

*Galaxy physics* (3 plots):
- Stellar mass function evolution
- Star formation rate density evolution
- Cold gas mass density evolution

### Plotting Options

**Generate specific plots**:
```bash
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --plots=halo_mass_function,stellar_mass_function,sfrd_evolution
```

**Only snapshot plots** (18 plots):
```bash
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --snapshot-plots
```

**Only evolution plots** (4 plots):
```bash
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --evolution-plots
```

**Cross-directory execution** (works from anywhere):
```bash
# Can run from any directory
cd ~/analysis
python ~/mimic/output/mimic-plot/mimic-plot.py \
    --param-file=~/mimic/input/millennium.yaml
```

### Testing Plotting System

**Unit tests** (validation helpers):
```bash
source mimic_venv/bin/activate
cd output/mimic-plot/tests
python3 test_validation_helpers.py
cd ../../..
deactivate
```

**Integration tests** (full plotting pipeline):
```bash
source mimic_venv/bin/activate
cd output/mimic-plot/tests
./test_plotting.sh
cd ../../..
deactivate
```

---

## Troubleshooting

### Common Errors

**"Unknown output format: hdf5"**

Problem: Mimic not compiled with HDF5 support

Solution:
```bash
make clean
make                 # HDF5 is enabled by default
./mimic input/millennium.yaml
```

**"Required model parameter 'GlobalBaryonFraction' not found"**

Problem: Missing parameter needed by enabled module

Solution: Add parameter to `modules.parameters:` section in YAML:
```yaml
modules:
  parameters:
    GlobalBaryonFraction: 0.17
```

**"Module 'my_module' listed in phase_1 but not registered"**

Problem: Module name misspelled or not compiled

Solution:
1. Check spelling in YAML file
2. Verify module exists in `src/modules/`
3. Rebuild: `make clean && make`

**Exit code: non-zero**

Problem: Simulation failed

Solution:
1. Run with `--debug` for detailed output:
   ```bash
   ./mimic --debug input/millennium.yaml 2>&1 | tee debug.log
   ```
2. Check logs for ERROR messages
3. Verify input files exist and are readable
4. Ensure sufficient disk space for output

### Performance Issues

**Slow output writing**:
- Switch from HDF5 to binary format:
  ```yaml
  output:
    output_format: binary  # 3.5× faster than HDF5
  ```

**Memory issues**:
- Reduce number of snapshots:
  ```yaml
  output:
    snapshot_list: [63, 37]  # Process fewer snapshots
  ```
- Process fewer input files:
  ```yaml
  input:
    first_file: 0
    last_file: 3  # Instead of 7
  ```
- Check for memory leaks:
  ```bash
  ./mimic --debug input/millennium.yaml
  # Check final memory report
  ```

**Compilation issues**:
- Ensure compiler supports C99 or later
- Check HDF5/MPI library versions match compiler
- Try clean rebuild:
  ```bash
  make clean
  make
  ```

### Getting Help

**Check logs with maximum verbosity**:
```bash
./mimic --debug input/millennium.yaml 2>&1 | tee debug.log
```

**Verify installation**:
```bash
make info   # Shows compiler, libraries, build configuration
make tests  # Runs full test suite
```

**Check documentation**:
- This guide (USER-GUIDE.md)
- Architecture: [VISION.md](VISION.md)
- Development: [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md)

---

## FAQ

**Q: How do I add a new physics module to my run?**

A: Edit your YAML file's `modules` section:
1. Add module to appropriate phase (e.g., `phase_1`)
2. Specify processing mode (e.g., `process_by_galaxy`)
3. Add required parameters to `modules.parameters` section

Example:
```yaml
modules:
  phase_1:
    - sage_starformation_feedback: process_by_galaxy

  parameters:
    SfrEfficiency: 0.02
    FeedbackReheatingEpsilon: 3.0
```

**Q: Can I disable all physics and just track halos?**

A: Yes, use physics-free mode:
```yaml
modules:
  pre_timestep: []
  phase_1: []
  phase_2: []
  post_timestep: []
  parameters: {}
```

This is the fastest execution mode.

**Q: What units does Mimic use?**

A: Internal code units:
- Mass: `1e10 Msun/h`
- Length: `Mpc/h`
- Velocity: `km/s`
- Time: `Myr` (output), `Gyr/h` (internal)

The `/h` notation means values include Hubble parameter h. To convert to physical units, divide by h.

**Q: How do I generate both binary and HDF5 output?**

A: Run twice with different `output_format` settings:
```bash
# Run 1: Binary (fast)
./mimic input/millennium.yaml  # output_format: binary

# Run 2: HDF5 (portable)
# Edit millennium.yaml: output_format: hdf5
./mimic input/millennium.yaml
```

Both formats contain identical data.

**Q: Can I process only specific snapshots?**

A: Yes, set `snapshot_list` in YAML:
```yaml
output:
  snapshot_list: [63, 37, 32]  # Only z=0, z~2, z~3
```

**Q: How do I check which properties are in my output?**

A: For binary:
```python
from generated_dtype import get_binary_dtype
print(get_binary_dtype().names)
```

For HDF5:
```python
import h5py
with h5py.File('model_000.hdf5', 'r') as f:
    print(f['Snap063/Galaxies'].dtype.names)

    # Or read field metadata
    metadata = f['Snap063/FieldMetadata'][:]
    for row in metadata:
        print(f"{row['field_name'].decode()}: {row['units'].decode()}")
```

**Q: What's the difference between SubSteps values?**

A:
- `SubSteps: 1` (default): No sub-stepping, fastest, less numerically stable
- `SubSteps: 10`: Moderate stability, good balance
- `SubSteps: 20`: SAGE-like, slowest, most numerically stable

Use higher values for better time integration of differential equations.

**Q: How do I enable parallel processing with MPI?**

A: Build with MPI support:
```bash
make clean
make USE-MPI=yes
```

Then run with mpirun:
```bash
mpirun -np 4 ./mimic input/millennium.yaml
```

**Q: Can I run Mimic without HDF5 libraries?**

A: Yes, disable HDF5 support:
```bash
make clean
make USE-HDF5=no
```

Then use binary output format:
```yaml
output:
  output_format: binary
```

**Q: How do I cite Mimic in publications?**

A: Cite the SAGE papers (Mimic builds on SAGE):
- Croton et al. 2016 (ApJS, 222, 22)
- Croton et al. 2006 (MNRAS, 365, 11)

---

**For more information**:
- Architecture principles: [VISION.md](VISION.md)
- Module development: [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md)
- Running tests: `make tests`
