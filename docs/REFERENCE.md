# Mimic Technical Reference

**Technical specifications for modules, properties, formats, and APIs**

---

## Table of Contents

1. [Module Metadata Schema](#module-metadata-schema)
2. [Property Metadata Schema](#property-metadata-schema)
3. [Output Format Specification](#output-format-specification)
4. [Configuration File Reference](#configuration-file-reference)
5. [API Reference](#api-reference)

---

## Module Metadata Schema

### File Location

`src/modules/your_module/module_info.yaml`

### Required Fields

```yaml
module:
  # Core identification
  name: my_module                    # REQUIRED: lowercase_with_underscores
  display_name: "My Module"          # REQUIRED: Human-readable name
  description: "Brief physics desc"  # REQUIRED: 1-2 sentence summary
  version: "1.0.0"                   # REQUIRED: Semantic versioning
  author: "Your Name"                # REQUIRED: Attribution

  # Source files
  sources:                           # REQUIRED: List of .c files
    - my_module.c

  # headers:                          # OPTIONAL: Not needed (generator uses forward declarations)
  #   - my_module.h

  register_function: my_module_register  # REQUIRED: {module_name}_register

  # Processing modes
  processing_modes:                  # REQUIRED: List of supported modes
    - process_by_galaxy              # or process_full_halo

  # Dependencies
  dependencies:
    properties:                      # List of properties used (read or write)
      - ColdGas
      - StellarMass

    parameters:                      # List of parameters from input file
      - name: MyEfficiency           # Parameter name (as in YAML input file)
        type: double                 # double, int, long, string
        default: null                # null (required) or default value
        valid_range: [0.0, 1.0]      # [min, max] for validation (optional)
        units: "dimensionless"       # Physical units
        description: "Brief desc"    # What this parameter controls

  # Testing (optional but recommended)
  tests:
    unit: test_unit_my_module.c
    integration: test_integration_my_module.py
    scientific: test_scientific_my_module_validation.py

  # Compilation requirements (optional)
  compilation_requires: []           # e.g., ["hdf5", "mpi"]
```

### Processing Modes

**process_by_galaxy**:
- Core loops over galaxies, module processes one at a time
- Best for: Per-galaxy physics, time integration
- Better cache locality

**process_full_halo**:
- Module receives entire galaxy array
- Best for: Snapshot-level operations, vectorized calculations
- Better for array operations

### Parameter Types

| Type | C Type | Example |
|------|--------|---------|
| `double` | `double` | `0.17`, `1.5e-3` |
| `int` | `int` | `1`, `20` |
| `long` | `long` | `1000000` |
| `string` | `const char*` | `"input/CoolFunctions"` |

### Auto-Generated Files

After `make generate`:
- `src/modules/_system/generated/module_init.c` - Registration code
- `tests/unit/generated/module_sources.mk` - Test build configuration
- `tests/generated/module_metadata.json` - Test metadata

### Example: Complete Module

```yaml
module:
  name: sage_cooling
  display_name: "SAGE Cooling"
  description: "Radiative cooling from hot halo to cold disk with AGN feedback"
  version: "1.0.0"
  author: "Mimic Team (ported from SAGE)"

  sources:
    - sage_cooling.c

  register_function: sage_cooling_register

  processing_modes:
    - process_by_galaxy

  dependencies:
    properties:
      - HotGas
      - MetalsHotGas
      - ColdGas
      - MetalsColdGas
      - BlackHoleMass

    parameters:
      - name: RadioModeEfficiency
        type: double
        default: null
        valid_range: [0.0, 1.0]
        units: "dimensionless"
        description: "Radio-mode AGN heating efficiency"

      - name: AGNrecipeOn
        type: int
        default: null
        valid_range: [0, 2]
        units: "flag"
        description: "AGN feedback mode (0=off, 1=radio, 2=quasar+radio)"

      - name: CoolFunctionsDir
        type: string
        default: null
        units: "path"
        description: "Directory containing cooling function tables"

  tests:
    unit: test_unit_sage_cooling.c
    integration: test_integration_sage_cooling.py

  compilation_requires: []
```

---

## Property Metadata Schema

### File Locations

- **Halo properties** (core): `src/core/halo_properties.yaml`
- **Galaxy properties** (physics): `src/modules/model_properties.yaml`

### Required Fields

```yaml
properties:
  - name: MyProperty             # REQUIRED: PascalCase identifier
    type: float                  # REQUIRED: float, double, int, long
    units: "1e10 Msun/h"         # REQUIRED: Physical units (for documentation)
    description: "Brief desc"    # REQUIRED: What this property represents
    output: true                 # REQUIRED: Include in output files?
    init_source: default         # REQUIRED: How to initialize (see below)
    init_value: 0.0f             # CONDITIONAL: Required if init_source: default
```

### Initialization Sources

**`init_source: default`** - Initialize to constant value
```yaml
- name: ColdGas
  init_source: default
  init_value: 0.0f               # REQUIRED: Initialization value
```

**`init_source: copy_from_tree`** - Copy from merger tree input
```yaml
- name: Mvir
  init_source: copy_from_tree
  init_value: InputHalo.Mvir     # REQUIRED: Tree field name
```

**`init_source: calculate`** - Call function to calculate
```yaml
- name: Vvir
  init_source: calculate
  init_value: calc_vvir          # REQUIRED: Function name
```

### Output Configuration

**`output: true/false`** - Include in output files?

**`output_source`** (optional, default: `copy_direct`):
- `copy_direct`: Copy from halo property
- `galaxy_property`: Copy from galaxy property
- `copy_with_conversion`: Copy with unit conversion
- `calculate`: Call function to calculate output value

**`output_convert`** (optional) - Unit conversion at output boundary:
```yaml
- name: dT
  output: true
  output_convert:
    factor: 3.1536e13          # Conversion factor (sec to Myr)
    description: "Convert dT from sec to Myr"
```

### Type Reference

| YAML Type | C Type | Size | Range |
|-----------|--------|------|-------|
| `float` | `float` | 4 bytes | ±3.4e38 |
| `double` | `double` | 8 bytes | ±1.7e308 |
| `int` | `int` | 4 bytes | ±2.1e9 |
| `long` | `long long` | 8 bytes | ±9.2e18 |

### Auto-Generated Files

After `make generate`:
- `src/include/generated/property_defs.h` - Struct definitions
- `src/include/generated/init_halo_properties.inc` - Halo init code
- `src/include/generated/init_galaxy_properties.inc` - Galaxy init code
- `src/include/generated/copy_to_output.inc` - Output copy logic
- `src/include/generated/hdf5_field_*.inc` - HDF5 field definitions
- `output/mimic-plot/generated/dtype.py` - Python dtypes

### Example: Galaxy Property

```yaml
- name: ColdGas
  type: float
  units: "1e10 Msun/h"
  description: "Cold gas mass in disk"
  output: true
  init_source: default
  init_value: 0.0f
```

### Example: Halo Property with Conversion

```yaml
- name: dT
  type: float
  units: "Myr"
  description: "Time since previous snapshot"
  output: true
  init_source: calculate
  init_value: calc_dt
  output_convert:
    factor: 3.1536e13
    description: "Convert from sec to Myr"
```

---

## Output Format Specification

### Binary Format

**File naming**: `model_z{redshift}_{filenr}`

Example: `model_z0.000_0` (snapshot 63, file 0)

**Structure**: Raw binary, sequential write of struct HaloOutput

**Reading** (Python):
```python
from generated_dtype import get_binary_dtype
import numpy as np

dtype = get_binary_dtype()
halos = np.fromfile('model_z0.000_0', dtype=dtype)
```

**Endianness**: Native (platform-dependent)

### HDF5 Format

**File naming**: `model_{filenr}.hdf5`

Example: `model_000.hdf5` (file 0, all snapshots)

**Master file structure** (`model.hdf5`):
```
/RunProperties/
  ├── @BoxSize (attribute: float)
  ├── @Hubble_h (attribute: float)
  ├── @Omega_m (attribute: float)
  ├── @Omega_lambda (attribute: float)
  ├── @PartMass (attribute: float)
  ├── Version/
  │   ├── @git_commit (attribute: string)
  │   ├── @git_branch (attribute: string)
  │   ├── @git_date (attribute: string)
  │   ├── @build_date (attribute: string)
  │   └── @hdf5_format_version (attribute: string, e.g., "1.0")
  ├── EnabledModules (compound dataset)
  │   └── Fields: module_name (str64), phase (str64), processing_mode (str64)
  ├── Parameters (dataset)
  │   └── Fields: param_name (str128), value (str256)
  └── Redshifts (dataset: float[64])

/Snap063/ (z=0)
  ├── FieldMetadata (dataset)
  └── File000/ → external link to model_000.hdf5
```

**Per-file structure** (`model_000.hdf5`):
```
/RunProperties/ (same as master - self-contained)

/Snap063/
  ├── FieldMetadata (structured table)
  │   └── Fields: field_name (str64), units (str128), description (str256)
  ├── Galaxies (compound dataset, variable-length)
  │   ├── @Ntrees (attribute: int)
  │   ├── @TotHalosPerSnap (attribute: int)
  │   └── Fields: [all output properties]
  └── TreeHalosPerSnap (dataset: int[Ntrees])

/Snap062/ (next snapshot)
  └── ...
```

**Reading** (Python):
```python
import h5py

with h5py.File('model_000.hdf5', 'r') as f:
    # Read data
    halos = f['Snap063/Galaxies'][:]

    # Read metadata
    metadata = f['Snap063/FieldMetadata'][:]
    units_dict = {row['field_name'].decode(): row['units'].decode()
                  for row in metadata}

    # Read run configuration
    enabled_modules = f['RunProperties/EnabledModules'][:]
    parameters = f['RunProperties/Parameters'][:]
    git_commit = f['RunProperties/Version'].attrs['git_commit']
```

### Field Metadata

**FieldMetadata dataset** (in each snapshot group):

| Field | Type | Description |
|-------|------|-------------|
| `field_name` | string(64) | Property name |
| `units` | string(128) | Physical units |
| `description` | string(256) | Human-readable description |

Example:
```
field_name        units              description
─────────────────────────────────────────────────────────────────
Mvir              1e10 Msun/h        Virial mass of halo
Rvir              Mpc/h              Virial radius of halo
ColdGas           1e10 Msun/h        Cold gas mass in disk
StellarMass       1e10 Msun/h        Total stellar mass
```

### Unit Conventions

| Quantity | Code Units | Example Properties |
|----------|-----------|-------------------|
| Mass | `1e10 Msun/h` | Mvir, StellarMass, ColdGas |
| Length | `Mpc/h` | Rvir, Pos, DiskScaleRadius |
| Velocity | `km/s` | Vvir, Vmax, Vel |
| Time | `Myr` (output), `sec` (internal) | dT, TimeOfLastMajorMerger |

**Converting to physical units**:
```python
h = 0.73  # From parameter file Hubble_h
mass_physical_msun = halos['Mvir'] * 1e10 / h  # Solar masses (no h)
length_physical_mpc = halos['Rvir'] / h         # Mpc (no h)
```

---

## Configuration File Reference

### Top-Level Sections

```yaml
# Output configuration
output:
  output_filename: model              # Base filename (without extension)
  output_directory: ./output/         # Output directory
  output_format: hdf5                 # 'binary' or 'hdf5'
  snapshot_list: [63, 37, 32, 27]     # Snapshots to process

# Input files
input:
  tree_type: lhalo_binary             # 'lhalo_binary' or 'genesis_lhalo_hdf5'
  simulation_dir: ./input/data/       # Merger tree directory
  first_file: 0                       # First file to process
  last_file: 7                        # Last file to process (inclusive)

# Simulation properties
simulation:
  cosmology:
    omega_matter: 0.25
    omega_lambda: 0.75
    hubble_h: 0.73
  box_size: 62.5                      # Mpc/h
  particle_mass: 0.0860657            # 10^10 Msun/h

# Time sub-stepping
SubSteps: 1                           # Number of substeps per snapshot

# Multi-phase pipeline
modules:
  pre_timestep: []                    # Setup phase (runs once)
  phase_1: []                         # Main physics (runs each substep)
  phase_2: []                         # Secondary physics (runs each substep)
  post_timestep: []                   # Finalization (runs once)
  parameters: {}                      # Physics parameters
```

### Module Phase Configuration

**Format**:
```yaml
modules:
  phase_name:
    - module_name: processing_mode
```

**Example**:
```yaml
modules:
  pre_timestep:
    - sage_reionization: process_full_halo
    - sage_calculate_infall: process_full_halo

  phase_1:
    - sage_cooling: process_by_galaxy
    - sage_starformation_feedback: process_by_galaxy

  phase_2:
    - sage_mergers: process_by_galaxy

  post_timestep: []

  parameters:
    GlobalBaryonFraction: 0.17
    RadioModeEfficiency: 0.01
    # ... (module-specific parameters)
```

### SAGE Module Parameters

| Parameter | Type | Units | Valid Range | Description |
|-----------|------|-------|-------------|-------------|
| `GlobalBaryonFraction` | double | dimensionless | [0.0, 1.0] | Cosmic baryon fraction |
| `RadioModeEfficiency` | double | dimensionless | [0.0, 1.0] | Radio-mode AGN heating efficiency |
| `AGNrecipeOn` | int | flag | [0, 2] | AGN feedback mode |
| `CoolFunctionsDir` | string | path | - | Cooling function table directory |
| `SFprescription` | int | flag | [0, 1] | Star formation prescription |
| `SfrEfficiency` | double | dimensionless | [0.0, 1.0] | Star formation efficiency |
| `EnergySNcode` | double | code units | [0.0, 10.0] | SN energy normalization |
| `EtaSNcode` | double | dimensionless | [0.0, 10.0] | SN mass loading factor |
| `SupernovaRecipeOn` | int | flag | [0, 1] | Enable SN feedback |
| `FeedbackReheatingEpsilon` | double | dimensionless | [0.0, 10.0] | Reheating efficiency |
| `FeedbackEjectionEfficiency` | double | dimensionless | [0.0, 1.0] | Ejection efficiency |
| `RecycleFraction` | double | dimensionless | [0.0, 1.0] | Stellar recycling fraction |
| `Yield` | double | dimensionless | [0.0, 0.1] | Metal yield |
| `FracZleaveDisk` | double | dimensionless | [0.0, 1.0] | Fraction of metals leaving disk |
| `ReIncorporationFactor` | double | dimensionless | [0.0, 10.0] | Reincorporation efficiency |
| `BlackHoleGrowthRate` | double | dimensionless | [0.0, 1.0] | BH accretion rate fraction |
| `QuasarModeEfficiency` | double | dimensionless | [0.0, 1.0] | Quasar-mode AGN efficiency |
| `ThreshMajorMerger` | double | dimensionless | [0.0, 1.0] | Major merger mass ratio threshold |
| `DiskInstabilityOn` | int | flag | [0, 1] | Enable disk instability |
| `DiskRadiusFactor` | double | dimensionless | [0.0, 10.0] | Disk size normalization |

See individual module `module_info.yaml` files for detailed parameter descriptions and physics context.

---

## API Reference

### Module Interface Functions

**Required lifecycle functions**:

```c
/* Initialize module (called once at startup) */
int module_init(void);

/* Process halos (called for each FOF group) */
int module_process(struct ModuleContext *ctx, struct Halo *halos, int ngal);

/* Cleanup module (called once at shutdown) */
int module_cleanup(void);
```

**Module Context**:
```c
struct ModuleContext {
  double redshift;     /* Current redshift */
  double dt;           /* Timestep (sec) */
  float a_scale;       /* Scale factor */
  int snapshot;        /* Snapshot number */
};
```

### Model Parameter Access

**Load parameters in `module_init()`**:

```c
#include "core/model_parameters.h"

/* Double precision */
LOAD_PARAM_DOUBLE("MyParam", my_param);

/* Integer */
LOAD_PARAM_INT("MyFlag", my_flag);

/* Long integer */
LOAD_PARAM_LONG("MyCount", my_count);

/* String */
LOAD_PARAM_STRING("MyPath", my_path);
```

These macros handle error checking and logging automatically.

### Memory Management

```c
#include "util/memory.h"

/* Allocate with category tracking */
float *data = mymalloc_cat(size * sizeof(float), MEM_PHYSICS);

/* Free */
myfree(data);

/* Check allocated memory */
print_allocated_by_category();
```

**Categories**: `MEM_HALOS`, `MEM_TREES`, `MEM_IO`, `MEM_UTILITY`, `MEM_PHYSICS`

### Logging

```c
#include "util/error.h"

/* Logging levels */
DEBUG_LOG("Detailed info");          /* Only with --debug */
VERBOSE_LOG("Config info");          /* Only with --verbose or --debug */
INFO_LOG("General info");            /* Default level */
WARNING_LOG("Warning message");       /* Always shown */
ERROR_LOG("Error message");          /* Always shown */
FATAL_ERROR("Fatal error");          /* Terminates program */
```

### Numerical Utilities

```c
#include "util/numeric.h"

/* Safe division (avoids division by zero) */
float result = safe_div(numerator, denominator);
```

---

**Need more detail?**
- User guide: [USER-GUIDE.md](USER-GUIDE.md)
- Developer guide: [DEVELOPER-GUIDE.md](DEVELOPER-GUIDE.md)
- Architecture: [VISION.md](VISION.md)
