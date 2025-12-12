# Mimic User Guide

**Complete guide to installing, configuring, and running Mimic simulations**

---

## Table of Contents

1. [Installation](#installation)
2. [Running Simulations](#running-simulations)
3. [Configuration](#configuration)
4. [Output](#output)
5. [Troubleshooting](#troubleshooting)
6. [FAQ](#faq)

---

## Installation

### Prerequisites

**Required**:
- C compiler (gcc 4.8+ or clang)
- GNU Make
- Python 3.6+ with pip

**Optional**:
- HDF5 libraries (for HDF5 output format)
- MPI libraries (for parallel processing)
- clang-format (for code formatting)
- black and isort (for Python code formatting)

### Automated Setup

The fastest way to get started:

```bash
# Clone repository
git clone [repository-url]
cd mimic

# Run first-time setup (creates directories, downloads data, sets up Python environment)
./scripts/first_run.sh

# Build
make

# Test
./mimic input/millennium.yaml
```

The `first_run.sh` script:
1. Creates required directories (`input/data/millennium`, `output/results/millennium`)
2. Downloads mini-Millennium test data (~50MB)
3. Creates Python virtual environment (`mimic_venv`)
4. Installs Python dependencies (numpy, matplotlib, pyyaml, h5py)
5. Updates paths in `input/millennium.yaml` to absolute paths

### Manual Setup

If automated setup fails or you need custom configuration:

**1. Create Directories**:
```bash
mkdir -p input/data/millennium output/results/millennium
```

**2. Download Test Data**:
```bash
cd input/data/millennium
wget "https://www.dropbox.com/s/l5ukpo7ar3rgxo4/mini-millennium-treefiles.tar?dl=0" -O mini-millennium-treefiles.tar
tar -xf mini-millennium-treefiles.tar
rm mini-millennium-treefiles.tar
cd ../../..
```

**3. Set Up Python Environment**:
```bash
python3 -m venv mimic_venv
source mimic_venv/bin/activate
pip install -r requirements.txt
deactivate
```

**4. Update Configuration**:
Edit `input/millennium.yaml` and set absolute paths for:
- `output.output_directory`
- `input.simulation_dir`
- `modules.parameters.CoolFunctionsDir`

**5. Build and Test**:
```bash
make
./mimic input/millennium.yaml
echo $?  # Should output: 0 (success)
```

### Build Options

**Standard build**:
```bash
make
```

**Disable HDF5 support** (binary output-only build):
```bash
make USE-HDF5=no
```

**With MPI parallelization** (requires MPI libraries installed):
```bash
make USE-MPI=yes
```

**Parallel compilation** (faster builds):
```bash
make -j$(nproc)  # Use all CPU cores
make -j4         # Use 4 cores
```

**Clean builds**:
```bash
make clean  # Remove all build artifacts
make tidy   # Remove object files, keep executable
```

**Regenerate code from metadata** (after editing YAML schemas):
```bash
make generate
```

---

## Running Simulations

### Basic Usage

```bash
./mimic <parameter_file>
```

Example:
```bash
./mimic input/millennium.yaml
```

### Command-Line Options

**Logging verbosity**:
```bash
./mimic input/millennium.yaml                  # Standard output (INFO, WARNING, ERROR)
./mimic --verbose input/millennium.yaml        # Verbose (adds context: timestamp, file:line)
./mimic --debug input/millennium.yaml          # Debug (maximum detail, includes DEBUG_LOG)
./mimic --quiet input/millennium.yaml          # Quiet (only WARNING and ERROR)
```

**Aliases** (`-v` for `--verbose`, `-d` for `--debug`, `-q` for `--quiet`):
```bash
./mimic -v input/millennium.yaml
./mimic -d input/millennium.yaml
./mimic -q input/millennium.yaml
```

**Skip existing output files**:
```bash
./mimic --skip input/millennium.yaml   # Don't overwrite existing output files
```

### Checking Success

Mimic returns exit code 0 on success:
```bash
./mimic input/millennium.yaml
echo $?  # 0 = success, non-zero = failure
```

Successful execution shows:
```
[INFO] Mimic v[version]
[INFO] Reading parameter file: input/millennium.yaml
[INFO] Initializing modules...
[INFO] Processing trees...
[INFO] Writing output...
[INFO] Mimic completed successfully
```

### Example Workflows

**Production run with HDF5 output**:
```bash
make clean
make -j$(nproc)                   # HDF5 is enabled by default
./mimic -q input/millennium.yaml  # Quiet mode for cleaner logs
```

**Debug run with maximum verbosity**:
```bash
./mimic --debug input/millennium.yaml 2>&1 | tee debug.log
```

**Quick test run** (physics-free mode):
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
./mimic input/millennium.yaml  # Fast halo tracking only
```

---

## Configuration

### YAML Configuration File

Mimic uses YAML format for runtime configuration. Main sections:

```yaml
# Output configuration
output:
  output_filename: model
  output_directory: ./output/results/millennium/
  output_format: hdf5                    # or 'binary'
  snapshot_list: [63, 37, 32, 27]        # Snapshots to process

# Input files
input:
  tree_type: lhalo_binary                # or 'genesis_lhalo_hdf5'
  simulation_dir: ./input/data/millennium/
  first_file: 0
  last_file: 7

# Simulation properties
simulation:
  cosmology:
    omega_matter: 0.25
    omega_lambda: 0.75
    hubble_h: 0.73
  box_size: 62.5                         # Mpc/h
  particle_mass: 0.0860657               # 10^10 Msun/h

# Time sub-stepping (optional, default: 1)
SubSteps: 1                              # Number of substeps per snapshot

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
1. **pre_timestep**: Setup calculations (runs once before substeps)
2. **phase_1**: Main physics (runs each substep)
3. **phase_2**: Secondary physics (runs each substep)
4. **post_timestep**: Finalization (runs once after substeps)

**Example configuration** (SAGE physics):
```yaml
SubSteps: 1  # Time sub-stepping (1 = no substeps, 20 = SAGE-like)

modules:
  # Setup phase (runs once)
  pre_timestep:
    - sage_reionization: process_full_halo
    - sage_calculate_infall: process_full_halo

  # Main physics (runs each substep)
  phase_1:
    - sage_cooling: process_by_galaxy
    - sage_starformation_feedback: process_by_galaxy
    - sage_reincorporation: process_by_galaxy
    - sage_disk_instability: process_by_galaxy

  # Secondary physics (runs each substep)
  phase_2:
    - sage_mergers: process_by_galaxy

  # Finalization (runs once)
  post_timestep: []

  # Physics parameters (required by enabled modules)
  parameters:
    GlobalBaryonFraction: 0.17
    RadioModeEfficiency: 0.01
    AGNrecipeOn: 1
    CoolFunctionsDir: "input/CoolFunctions"
    # ... (see REFERENCE.md for complete parameter list)
```

**Processing modes**:
- `process_full_halo`: Module processes entire galaxy array at once (better for vectorized operations)
- `process_by_galaxy`: Core loops over galaxies, module processes one at a time (better cache locality)

**Time sub-stepping**:
- `SubSteps: 1` (default): No sub-stepping, phase_1/phase_2 run once per snapshot
- `SubSteps: 20`: SAGE-like behavior, phase_1/phase_2 run 20 times with smaller timesteps

### Available Physics Modules

**SAGE physics modules** (complete SAGE implementation):
- `sage_reionization`: Reionization suppression for low-mass halos
- `sage_calculate_infall`: Gas infall onto central galaxies
- `sage_satellite_stripping`: Environmental stripping for satellites
- `sage_cooling`: Radiative cooling with AGN feedback
- `sage_starformation_feedback`: Star formation and supernova feedback
- `sage_reincorporation`: Reincorporation of ejected gas
- `sage_mergers`: Galaxy mergers and black hole growth
- `sage_disk_instability`: Disk instability and bulge formation

See [REFERENCE.md](REFERENCE.md) for detailed module descriptions and parameter specifications.

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

Useful for testing infrastructure or when only halo properties are needed.

---

## Output

### Output Formats

Mimic supports two output formats with identical data:

**Binary** (fast, compact):
- 3.5× faster than HDF5
- Requires matching reader code
- File naming: `model_z{redshift}_{filenr}` (e.g., `model_z0.000_0`)
- Use for: production runs, performance-critical workflows

**HDF5** (self-documenting, portable):
- Standard format readable by many tools
- Self-describing with metadata
- Requires HDF5 libraries (enabled by default; disable with `USE-HDF5=no`)
- File naming: `model_{filenr}.hdf5` (e.g., `model_000.hdf5`)
- Use for: data sharing, long-term archival, exploratory analysis

**Select format in YAML**:
```yaml
output:
  output_format: binary  # or 'hdf5'
```

### Output Contents

**Always included** (core halo properties):
- Halo properties: Mvir, Rvir, Vmax, Spin, Position, Velocity, etc.
- Tree structure: TreeID, DescID, HaloIndex

**Conditionally included** (galaxy properties from enabled modules):
- Baryonic masses: ColdGas, HotGas, StellarMass, BulgeMass
- Metals: MetalsColdGas, MetalsHotGas, MetalsStellarMass
- Star formation: Sfr, ICS (intracluster stars)
- Black holes: BlackHoleMass, QuasarModeBHaccretionMass

Properties are only output if:
1. Defined with `output: true` in property metadata
2. Set by an enabled physics module
3. Have non-zero values

### Reading Output

**Using mimic-plot** (recommended):
```bash
source mimic_venv/bin/activate

# Auto-detects format (binary or HDF5) and available properties
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml

# Generate specific plots
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --plots=halo_mass_function,spin_distribution

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

# Get units
units = get_units()
print(f"Mvir units: {units['Mvir']}")  # "1e10 Msun/h"
```

**Custom Python code** (HDF5):
```python
import h5py

with h5py.File('output/results/millennium/model_000.hdf5', 'r') as f:
    # Read z=0 snapshot
    halos = f['Snap063/Galaxies'][:]

    # Access properties
    masses = halos['Mvir']

    # Read metadata
    metadata = f['Snap063/FieldMetadata'][:]
    for row in metadata[:5]:
        field = row['field_name'].decode()
        units = row['units'].decode()
        desc = row['description'].decode()
        print(f"{field}: {units} - {desc}")
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
  └── TreeHalosPerSnap (dataset: halos per tree)
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

Solution: Add parameter to `modules.parameters:` section in YAML file:
```yaml
modules:
  parameters:
    GlobalBaryonFraction: 0.17
```

**"Module 'sage_cooling' listed in phase_1 but not registered"**

Problem: Module name misspelled or not compiled

Solution: Check spelling, verify module exists in `src/modules/`, run `make clean && make`

**Exit code: non-zero**

Problem: Simulation failed

Solution:
1. Run with `--debug` for detailed output
2. Check logs for ERROR messages
3. Verify input files exist and are readable
4. Ensure sufficient disk space for output

### Performance Issues

**Slow output writing**:
- Switch from HDF5 to binary format (`output_format: binary`)
- Binary is ~3.5× faster for writes

**Memory issues**:
- Reduce number of snapshots in `snapshot_list`
- Process fewer input files (`first_file` / `last_file`)
- Check for memory leaks: run with `--debug` and check final memory report

**Compilation issues**:
- Ensure compiler supports C99 or later
- Check HDF5/MPI library versions match compiler
- Try `make clean` before rebuilding

### Getting Help

**Check logs**:
```bash
./mimic --debug input/millennium.yaml 2>&1 | tee debug.log
```

**Verify installation**:
```bash
make info  # Shows compiler, libraries, build configuration
make tests # Runs test suite
```

**Check documentation**:
- This guide (USER-GUIDE.md)
- [REFERENCE.md](REFERENCE.md) for parameter specifications
- [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md) for internals

---

## FAQ

**Q: How do I add a new physics module to my run?**

A: Edit your YAML file's `modules` section:
1. Add module to appropriate phase (e.g., `phase_1`)
2. Specify processing mode (e.g., `process_by_galaxy`)
3. Add required parameters to `modules.parameters` section

See [REFERENCE.md](REFERENCE.md) for module parameter requirements.

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

**Q: What units does Mimic use?**

A: Internal code units:
- Mass: `1e10 Msun/h`
- Length: `Mpc/h`
- Velocity: `km/s`
- Time: `Myr` (output), `sec` (internal calculations)

The `/h` notation means values include Hubble parameter h. To convert to physical units, divide by h.

**Q: How do I generate both binary and HDF5 output?**

A: Run twice with different `output_format` settings (both formats contain identical data).

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
```

**Q: What's the difference between SubSteps values?**

A:
- `SubSteps: 1` (default): No sub-stepping, faster, less numerically stable
- `SubSteps: 20`: SAGE-like, slower, more numerically stable
- Use higher values for better time integration of differential equations

**Q: How do I cite Mimic in publications?**

A: Cite the SAGE papers (Mimic builds on SAGE):
- Croton et al. 2016 (ApJS, 222, 22)
- Croton et al. 2006 (MNRAS, 365, 11)

---

**Need more detail?**
- Architecture: [VISION.md](VISION.md)
- Development: [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md)
- Reference: [REFERENCE.md](REFERENCE.md)
