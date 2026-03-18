# Mimic User Guide

**Guide to installing, configuring, and running Mimic semi-analytic simulations**

---

## Table of Contents

1. [Installation](#installation)
2. [Running Simulations](#running-simulations)
3. [Configuration](#configuration)
4. [Output](#output)
5. [Troubleshooting](#troubleshooting)

---

## Installation

### Quick Start

```bash
git clone [repository-url]
cd mimic
./scripts/first_run.sh  # Creates directories, downloads data, sets up Python
make
./mimic input/millennium.yaml
```

The `first_run.sh` script downloads mini-Millennium test data (~50MB) and creates Python virtual environment for plotting.

### Prerequisites

**Required**: C compiler (gcc 4.8+/clang), GNU Make, Python 3.6+

**Optional**: HDF5 libraries (recommended), MPI libraries

### Build Options

```bash
make                  # Standard build (HDF5 enabled by default)
make -j$(nproc)       # Parallel build (faster)
make USE-HDF5=no      # Disable HDF5
make USE-MPI=yes      # Enable MPI
make info             # Show build configuration
```

### Manual Setup

If automated setup fails:

```bash
# Create directories
mkdir -p input/data/millennium output/results/millennium

# Download data
cd input/data/millennium
wget "https://www.dropbox.com/s/l5ukpo7ar3rgxo4/mini-millennium-treefiles.tar?dl=0" \
     -O mini-millennium-treefiles.tar
tar -xf mini-millennium-treefiles.tar && rm mini-millennium-treefiles.tar
cd ../../..

# Python environment (for plotting)
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
deactivate

# Build
make
./mimic input/millennium.yaml  # Should exit with code 0
```

Edit `input/millennium.yaml` to set absolute paths for `output.output_directory` and `input.simulation_dir`.

---

## Running Simulations

### Basic Usage

```bash
./mimic <parameter_file.yaml>
```

**Command-line options**:

| Flag | Output | Use Case |
|------|--------|----------|
| (default) | INFO, WARNING, ERROR | Normal runs |
| `--verbose` / `-v` | + timestamp, file:line context | Detailed logging |
| `--debug` / `-d` | + DEBUG messages | Troubleshooting |
| `--quiet` / `-q` | WARNING, ERROR only | Production runs |
| `--skip` | Skip existing output files | Resume interrupted runs |

**Example**:
```bash
./mimic --debug input/millennium.yaml 2>&1 | tee debug.log
```

Mimic returns exit code 0 on success.

---

## Configuration

### YAML Structure

```yaml
# Output
output:
  output_filename: model              # Base name (no extension)
  output_directory: ./output/results/
  output_format: hdf5                 # 'binary' or 'hdf5'
  snapshot_list: [63, 37, 32, 27]     # Snapshots to process

# Input
input:
  tree_type: lhalo_binary             # 'lhalo_binary' or 'genesis_lhalo_hdf5'
  simulation_dir: ./input/data/
  first_file: 0
  last_file: 7
  tree_name: trees_063
  snapshot_list_file: ./input/data/millennium.a_list
  last_snapshot: 63

# Simulation
simulation:
  cosmology:
    omega_matter: 0.25
    omega_lambda: 0.75
    hubble_h: 0.73
  box_size: 62.5                      # Mpc/h
  particle_mass: 0.0860657            # 10^10 Msun/h

# Sub-stepping (optional)
SubSteps: 1                           # 1=no substeps, 10=moderate, 20=SAGE-like

# Multi-phase pipeline
modules:
  pre_timestep: []                    # Runs once before substeps
  phase_1: []                         # Runs each substep
  phase_2: []                         # Runs each substep
  post_timestep: []                   # Runs once after substeps
  parameters: {}                      # Physics parameters
```

### Physics Modules

**Pipeline execution order**:
```
For each snapshot interval:
  1. pre_timestep  (once)
  2. Loop over SubSteps:
     - phase_1     (each substep)
     - phase_2     (each substep)
  3. post_timestep (once)
```

**Example configuration** (SAGE physics):

```yaml
SubSteps: 10

modules:
  pre_timestep:
    # Setup and snapshot budgets
    - sage_reionization:              process_full_halo
    - sage_prepare_infall_budget:     process_full_halo
    - sage_set_disk_scale_radius:     process_full_halo
    - sage_initialise_merger_clock:   process_full_halo

  phase_1:
    # Baryon supply — process_full_halo modules always execute before process_by_galaxy
    - sage_apply_infall:              process_full_halo
    - sage_reincorporation:           process_full_halo
    - sage_satellite_stripping:       process_full_halo

    # Cooling and AGN suppression (Calculate → Modify → Apply)
    - sage_calculate_cooling_budget:  process_by_galaxy
    - sage_radio_mode_heating:        process_by_galaxy
    - sage_apply_cooling:             process_by_galaxy

    # Star formation and supernova feedback
    # sage_star_formation and sage_supernova_feedback are independently optional.
    - sage_star_formation:            process_by_galaxy   # prescription: SF rate law
    - sage_supernova_feedback:        process_by_galaxy   # prescription: SN feedback model
    - sage_apply_star_formation_supernova: process_by_galaxy  # infrastructure: commit SF/SN results

    # Disk instability
    - sage_disk_instability:          process_by_galaxy
    - sage_quasar_mode:               process_by_galaxy
    - sage_starburst_feedback:        process_by_galaxy

  phase_2:
    # Mergers and disruption — runs after all galaxy physics for the current substep
    - sage_resolve_mergers_and_disruption: process_full_halo
    - sage_quasar_mode:               process_per_event   # merger event consumer
    - sage_starburst_feedback:        process_per_event   # merger event consumer

  post_timestep: []

  parameters:
    # Reionisation
    GlobalBaryonFraction: 0.17

    # Cooling & AGN Feedback
    AGNrecipe: 1
    RadioModeEfficiency: 0.01

    # Star Formation
    SfrEfficiency: 0.02

    # Supernova Feedback
    FeedbackReheatingEpsilon: 3.0
    FeedbackEjectionEfficiency: 0.3
    # Note: EtaSNcode and EnergySNcode are calculated from physical constants

    # Metals
    RecycleFraction: 0.43
    Yield: 0.03
    FracZleaveDisk: 0.3

    # Mergers & BH Growth
    ThresholdMajorMerger: 0.3
    ThresholdSatDisruption: 1.0
    BlackHoleGrowthRate: 0.01
    QuasarModeEfficiency: 0.001
    StarFormingDiskFactor: 3.0
```

**Processing modes**:
- `process_full_halo`: Module processes entire galaxy array
- `process_per_event`: Core dispatches each emitted event target with `ctx->active_event` set
- `process_by_galaxy`: Core loops over galaxies (better cache locality)

**Available SAGE modules**:

| Module | Phase | Description |
|--------|-------|-------------|
| `sage_reionization` | pre_timestep | Reionization suppression |
| `sage_prepare_infall_budget` | pre_timestep | Cosmological infall budget; consolidates satellite reservoirs |
| `sage_set_disk_scale_radius` | pre_timestep | Disk scale radius from halo spin |
| `sage_initialise_merger_clock` | pre_timestep | Merger timescales, Type 0/2 state management |
| `sage_apply_infall` | phase_1 | Distribute infall budget to hot reservoir over substeps |
| `sage_reincorporation` | phase_1 | Reincorporation of ejected gas |
| `sage_satellite_stripping` | phase_1 | Environmental stripping of satellite hot gas |
| `sage_calculate_cooling_budget` | phase_1 | Compute cooling rate and budget |
| `sage_radio_mode_heating` | phase_1 | AGN radio-mode suppression of cooling |
| `sage_apply_cooling` | phase_1 | Transfer cooled gas (hot → cold) |
| `sage_star_formation` | phase_1 | Star formation rate prescription (swappable) |
| `sage_supernova_feedback` | phase_1 | Supernova feedback prescription (swappable, optional) |
| `sage_apply_star_formation_supernova` | phase_1 | Infrastructure: commit SF/SN results to galaxy reservoirs |
| `sage_disk_instability` | phase_1 | Disk stability check; stellar transfer to bulge |
| `sage_quasar_mode` | phase_1, phase_2 | Disk-instability BH growth (phase_1); merger-event quasar winds (phase_2) |
| `sage_starburst_feedback` | phase_1, phase_2 | Disk-instability starbursts (phase_1); merger-event starbursts (phase_2) |
| `sage_resolve_mergers_and_disruption` | phase_2 | Merger coalescence and satellite disruption; emits merger events |

**Physics-free mode** (halo tracking only):
```yaml
modules:
  pre_timestep: []
  phase_1: []
  phase_2: []
  post_timestep: []
  parameters: {}
```

---

## Output

### Output Formats

**Binary**: Fast (3.5× faster than HDF5), compact, requires matching reader

**HDF5**: Self-documenting, portable, standard format

Select in YAML: `output.output_format: binary` or `hdf5`

### Unit Conventions

| Quantity | Units | Properties |
|----------|-------|------------|
| Mass | 1e10 Msun/h | Mvir, StellarMass, ColdGas |
| Length | Mpc/h | Rvir, Pos, DiskScaleRadius |
| Velocity | km/s | Vvir, Vmax, Vel |
| Time | Myr/h, Gyr/h (output) | dT, TimeOfLastMajorMerger |

To convert to physical units: divide by `h` (from `hubble_h` parameter).

### Output Properties

**Always included** (halo properties):
- Mvir, Rvir, Vmax, Spin, Position, Velocity
- TreeID, DescID, HaloIndex, UniqueGalaxyID

**Conditionally included** (galaxy properties, if physics enabled):
- Baryonic: ColdGas, HotGas, StellarMass, BulgeMass, EjectedGas
- Metals: MetalsColdGas, MetalsHotGas, MetalsStellarMass, MetalsBulgeMass
- Star formation: Sfr, NewStellarMass
- Black holes: BlackHoleMass, QuasarModeBHaccretionMass
- Structure: DiskScaleRadius
- Mergers: TimeOfLastMajorMerger, TimeOfLastMinorMerger

### Reading Output

**Binary** (Python):

```python
from generated_dtype import get_binary_dtype, get_units
import numpy as np

dtype = get_binary_dtype()
halos = np.fromfile('output/results/millennium/model_z0.000_0', dtype=dtype)

# Access properties
masses = halos['Mvir']      # 1e10 Msun/h
stellar = halos['StellarMass']

# Convert to physical units
h = 0.73
mass_msun = masses * 1e10 / h  # Solar masses (no h)
```

**HDF5** (Python):

```python
import h5py

with h5py.File('output/results/millennium/model_000.hdf5', 'r') as f:
    # Read galaxies
    halos = f['Snap063/Galaxies'][:]

    # Access properties
    masses = halos['Mvir']
    stellar = halos['StellarMass']

    # Read metadata
    metadata = f['Snap063/FieldMetadata'][:]
    for row in metadata[:5]:
        print(f"{row['field_name'].decode()}: {row['units'].decode()}")

    # Configuration
    git_commit = f['RunProperties/Version'].attrs['git_commit']
    redshifts = f['RunProperties/Redshifts'][:]
```

**HDF5 structure**:

Master file (`model.hdf5`):
```
/RunProperties/
  ├── @BoxSize, @Hubble_h, @Omega (simulation parameters)
  ├── Version/ (@git_commit, @git_branch, @hdf5_format_version)
  ├── EnabledModules (module_name, phase, processing_mode)
  ├── EventContracts (phase, consumer_module, producer_module, event_name, event_id)
  ├── Parameters (param_name, value pairs)
  └── Redshifts (z for each snapshot)

/Snap063/
  ├── FieldMetadata (field descriptions)
  └── File000/ → external link to model_000.hdf5
```

Per-file output (`model_000.hdf5`):
```
/RunProperties/ (same as master - self-contained)
/Snap063/
  ├── FieldMetadata (field_name, units, description)
  ├── Galaxies (compound dataset)
  │   ├── @Ntrees
  │   └── @TotHalosPerSnap
  └── TreeHalosPerSnap (halos per tree)
```

`EventContracts` records every active event subscription for this run — one row
per consumer/producer pair. If no modules use the event system, the dataset is
omitted. Read it to verify which modules are wired together:

```python
with h5py.File("model.hdf5", "r") as f:
    if "RunProperties/EventContracts" in f:
        contracts = f["RunProperties/EventContracts"][:]
        for row in contracts:
            print(f"{row['phase'].decode()}: {row['consumer_module'].decode()} "
                  f"← {row['producer_module'].decode()}/{row['event_name'].decode()} "
                  f"(id={row['event_id']})")
```

### Plotting

```bash
source mimic_venv/bin/activate

# All plots (22 total: 18 snapshot + 4 evolution)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml

# Specific plots
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --plots=halo_mass_function,stellar_mass_function

# Snapshot plots only (18 plots)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --snapshot-plots

# Evolution plots only (4 plots)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --evolution-plots

deactivate
```

**Available plots**:

*Snapshot (z=0)*: Halo mass function, Vmax/spin/concentration distributions, stellar/cold gas/hot gas mass functions, metallicity distributions, SFR function, sSFR, stellar mass vs halo mass, cold gas fraction, bulge-to-total ratio, BH mass vs stellar mass, outflow rate, disk scale radius

*Evolution*: Halo/stellar mass function evolution, SFRD evolution, cold gas density evolution

Plots saved to `output/results/millennium/plots/`

---

## Troubleshooting

### Build Issues

**HDF5 not found**: If `make` reports missing HDF5 headers, either install HDF5 development libraries (`libhdf5-dev` on Debian/Ubuntu, `hdf5` via Homebrew on macOS) or build without HDF5:
```bash
make USE-HDF5=no
```

**Compiler errors after code changes**: If you've edited property YAML files or module metadata, regenerate before building:
```bash
make generate
make clean && make
```

**`make info` shows unexpected configuration**: Use this to verify which libraries were detected and which features are enabled.

### Runtime Issues

**Non-zero exit code**: Mimic returns exit code 0 on success. Check stderr output for error messages. Run with `--debug` for full diagnostic output:
```bash
./mimic --debug input/millennium.yaml 2>&1 | tee debug.log
```

**"Cannot open file" errors**: Check that paths in your YAML configuration are correct. Both `output.output_directory` and `input.simulation_dir` must exist. Use absolute paths if relative paths cause issues.

**Missing test data**: Run the setup script to download mini-Millennium data:
```bash
./scripts/first_run.sh
```

### Plotting Issues

**`ModuleNotFoundError`**: Activate the Python virtual environment first:
```bash
source mimic_venv/bin/activate
```

**No plots generated / all plots skipped**: If physics modules are not enabled, only halo property plots will be generated. Galaxy physics plots require physics modules in the pipeline. Run with `--verbose` to see skip reasons:
```bash
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml --verbose
```

**Virtual environment missing**: Recreate it:
```bash
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
```

---

**For architectural details**: See [VISION.md](VISION.md)

**For module development**: See [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md)

**Citations**: Croton et al. 2016 (ApJS, 222, 22); Croton et al. 2006 (MNRAS, 365, 11)
