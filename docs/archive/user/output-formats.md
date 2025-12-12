# Output Format Guide

**Phase**: 3 (Runtime Module Configuration) + Format Synchronization
**Audience**: Users configuring Mimic output
**Prerequisites**: Basic understanding of parameter files

## Overview

Mimic supports two output formats for halo and galaxy data:
- **Binary**: Fast, compact format optimized for performance
- **HDF5**: Self-describing format for long-term archival and portability

Both formats are automatically synchronized via the property metadata system to ensure they contain identical data. Choose based on your workflow priorities.

## Format Selection

Set the output format in your parameter file:

```
OutputFormat  binary
```

or

```
OutputFormat  hdf5
```

### Binary Format

**Best for**: Production runs, performance-critical workflows, large simulations

**Characteristics**:
- **Performance**: ~3.5x faster than HDF5 for writing
- **Size**: More compact (baseline for size comparison)
- **Compatibility**: Requires matching reader code (provided in `output/mimic-plot/`)
- **Self-describing**: No (requires separate metadata or generated dtype)
- **Random access**: Limited (sequential read is most efficient)

**File naming**: `model_z{redshift}_{filenr}` (e.g., `model_z0.000_0`)

**When to use**:
- Running large simulations where I/O is a bottleneck
- When you control both writing and reading code
- When you need maximum performance

### HDF5 Format

**Best for**: Data sharing, long-term archival, exploratory analysis

**Characteristics**:
- **Performance**: Slower than binary (factor of ~3.5x)
- **Size**: Larger than binary (depends on compression settings)
- **Compatibility**: Standard format readable by many tools
- **Self-describing**: Yes (includes FieldMetadata dataset with units for all properties)
- **Random access**: Excellent (can read specific snapshots/properties efficiently)

**Compilation requirement**: Build with `make USE-HDF5=yes`

**File naming**: `model_{filenr}.hdf5` (e.g., `model_000.hdf5`)

**When to use**:
- Sharing data with collaborators
- Long-term data archival
- When you need self-documenting output
- Integration with HDF5-based workflows

### Compilation Requirements

**Binary format**: Always available (default build)

**HDF5 format**: Requires HDF5 libraries installed and compilation flag:

```bash
make clean
make USE-HDF5=yes
```

## Format Equivalence

Both formats contain **identical data** - all halo properties and galaxy properties from enabled modules. The formats are automatically synchronized via the property metadata system:

1. Properties defined in `metadata/*.yaml`
2. Auto-generated into C structs (HaloOutput)
3. Auto-generated into Python dtypes (for reading)
4. Both formats write the same fields

**Validation**: The integration test `tests/integration/test_output_formats.py` includes `test_format_equivalence()` which verifies that binary and HDF5 produce identical halo counts and property values.

## Property Availability

### Output Contents

Output files contain:
- **Core halo properties** (always present): Mvir, Rvir, Vmax, Spin, etc.
- **Galaxy properties** (when modules enabled): ColdGas, StellarMass, etc.

The specific properties included depend on which physics modules were enabled during the run.

### Checking Available Properties

To see what properties are available in an output file:

**Python (binary format)**:
```python
from generated_dtype import get_binary_dtype
import numpy as np

# Load data
dtype = get_binary_dtype()
data = np.fromfile('model_z0.000_0', dtype=dtype)

# Check available properties
print("Available properties:", data.dtype.names)

# Check if specific property exists
if 'StellarMass' in data.dtype.names:
    print("StellarMass available!")
```

**Python (HDF5 format)**:
```python
import h5py

with h5py.File('model_000.hdf5', 'r') as f:
    snap = f['Snap063']  # z=0 snapshot
    galaxies = snap['Galaxies']

    # Check available properties
    print("Available properties:", galaxies.dtype.names)

    # Read unit metadata from FieldMetadata dataset
    metadata = snap['FieldMetadata'][:]
    units_dict = {row['field_name'].decode(): row['units'].decode()
                  for row in metadata}

    # Show properties with units
    for prop in galaxies.dtype.names[:5]:  # First 5 properties
        print(f"  {prop}: {units_dict.get(prop, 'unknown')}")
```

### Module Dependencies

Properties are provided by modules. To get specific properties:

| Property | Required Module | Configuration |
|----------|----------------|---------------|
| Mvir, Rvir, Vmax, Spin | Core (always) | No modules needed |
| ColdGas | Cooling module | Enable in multi-phase pipeline |
| StellarMass | Star formation + cooling | Enable both modules in pipeline |

**Example**: To get both ColdGas and StellarMass in output:

```yaml
modules:
  phase_1:
    - sage_cooling: process_by_galaxy
    - sage_starformation_feedback: process_by_galaxy

  parameters:
    GlobalBaryonFraction: 0.17
    StarFormationEfficiency: 0.02
```

## Working with Unit Metadata

All Mimic output properties include unit metadata for reproducible science. Units are provided differently for binary and HDF5 formats.

### Python Unit Dictionary (Binary and HDF5)

Both output formats can use the auto-generated Python unit dictionary:

```python
from generated.dtype import get_units

# Get units for all properties
units = get_units()
print(f"Mvir: {units['Mvir']}")        # "1e10 Msun/h"
print(f"Rvir: {units['Rvir']}")        # "Mpc/h"
print(f"dT: {units['dT']}")            # "Myr"

# Use with binary data
import numpy as np
halos = np.fromfile('model_z0.000_0', dtype=get_binary_dtype())
print(f"Mvir range: {halos['Mvir'].min():.2e} to {halos['Mvir'].max():.2e} {units['Mvir']}")

# Use with HDF5 data
import h5py
with h5py.File('model_000.hdf5', 'r') as f:
    halos = f['Snap063/Galaxies'][:]
    print(f"Rvir range: {halos['Rvir'].min():.3f} to {halos['Rvir'].max():.3f} {units['Rvir']}")
```

### HDF5 FieldMetadata Dataset

HDF5 files include a self-contained `FieldMetadata` dataset in each snapshot group:

```python
import h5py

with h5py.File('model_000.hdf5', 'r') as f:
    # Explore file structure
    print(f"Snapshot groups: {list(f.keys())}")
    print(f"Snap063 datasets: {list(f['Snap063'].keys())}")
    # Shows: ['Galaxies', 'FieldMetadata', 'TreeHalosPerSnap']

    # Read FieldMetadata dataset
    metadata = f['Snap063/FieldMetadata'][:]

    # Convert to dictionary
    units_dict = {row['field_name'].decode(): row['units'].decode()
                  for row in metadata}

    # Access units for specific fields
    print(f"Mvir units: {units_dict['Mvir']}")
    print(f"StellarMass units: {units_dict['StellarMass']}")

    # List all fields with units and descriptions
    for row in metadata[:10]:  # First 10 fields
        field = row['field_name'].decode()
        units = row['units'].decode()
        desc = row['description'].decode()
        print(f"{field:20s} {units:20s} {desc}")
```

**Structure**: The FieldMetadata dataset is a structured table with columns:
- `field_name` (string, 64 chars): Property name
- `units` (string, 128 chars): Unit string
- `description` (string, 256 chars): Human-readable property description

**Location**: Present in every snapshot group (e.g., `Snap000/FieldMetadata`, `Snap063/FieldMetadata`)

### Unit Conventions

Mimic uses a consistent internal code unit system:

| Quantity | Code Units | Example Property |
|----------|-----------|------------------|
| Mass | `1e10 Msun/h` | Mvir, StellarMass, ColdGas |
| Length | `Mpc/h` | Rvir, Pos, DiskScaleRadius |
| Velocity | `km/s` | Vvir, Vmax, Vel |
| Time | `Myr` (output), `sec` (internal) | dT, TimeOfLastMajorMerger |

**The `/h` notation**: Values include the Hubble parameter `h = H0 / (100 km/s/Mpc)`. To convert to physical units, divide by h:

```python
h = 0.73  # From parameter file Hubble_h
mass_physical_msun = halos['Mvir'] * 1e10 / h  # Solar masses (no h)
length_physical_mpc = halos['Rvir'] / h         # Mpc (no h)
```

For detailed information on the unit system and conversions, see:
- `docs/developer/unit-system-guide.md` - Complete unit system documentation
- `output/mimic-plot/README.md` - Python examples for working with units

## Reading Output Files

### Using mimic-plot

The `mimic-plot` tool automatically detects available properties and adapts:

```bash
# Auto-detects format (binary or HDF5) and available properties
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml

# Generate specific plots (skips plots requiring unavailable properties)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.yaml \
    --plots=halo_mass_function,stellar_mass_function
```

**Property detection**: mimic-plot checks which properties are available and:
- Generates halo plots (always possible)
- Generates galaxy plots only if required properties present (e.g., stellar_mass_function requires StellarMass)
- Prints clear messages about skipped plots due to missing properties

**Example output** when ColdGas/StellarMass unavailable:
```
Skipping 2 plot(s) due to missing properties:
  - stellar_mass_function: missing StellarMass
  - cold_gas_function: missing ColdGas
  (Enable physics modules to generate these plots)
```

### Custom Reading Code

Use the auto-generated dtypes for reading:

**Binary**:
```python
from generated_dtype import get_binary_dtype
import numpy as np

dtype = get_binary_dtype()
halos = np.fromfile('model_z0.000_0', dtype=dtype)

# Access properties
masses = halos['Mvir']
if 'StellarMass' in halos.dtype.names:
    stellar = halos['StellarMass']
```

**HDF5**:
```python
import h5py

with h5py.File('model_000.hdf5', 'r') as f:
    halos = f['Snap063/Galaxies'][:]

    # Access properties
    masses = halos['Mvir']
    if 'StellarMass' in halos.dtype.names:
        stellar = halos['StellarMass']
```

## Format Migration

### Converting Between Formats

Currently no automated conversion tool. To generate both formats from the same run:

1. Run once with binary output (fast)
2. Rerun with HDF5 output (same inputs)
3. Use `test_format_equivalence()` to verify equivalence

**Note**: Since both formats write identical data, the choice affects only I/O performance and downstream compatibility, not scientific results.

### Updating to New Property Schema

When property definitions change (new modules, modified properties):

1. Run `make generate` to update generated code
2. Regenerate baseline data if needed
3. Both formats automatically include new properties

The test framework (`tests/framework/data_loader.py`) and plotting tools automatically use the current generated dtype.

## Performance Considerations

### Binary Format Performance

**Advantages**:
- Fast sequential writes (~3.5x faster than HDF5)
- Compact file size
- Minimal overhead

**Best practices**:
- Use for production runs with large datasets
- Read sequentially when possible
- Use memory-mapped access for very large files

### HDF5 Format Performance

**Advantages**:
- Excellent random access
- Self-documenting
- Built-in compression (optional)

**Configuration** (in HDF5 writer, if exposed):
- Chunk size affects random access performance
- Compression trades CPU for storage (disabled by default for performance)

**Best practices**:
- Use when you need random access to subsets
- Consider compression for long-term archival
- Use h5py's caching for repeated access patterns

## File Organization

### Binary Output Structure

```
OutputDir/
├── model_z0.000_0      # Snapshot 63, file 0
├── model_z0.000_1      # Snapshot 63, file 1
├── model_z1.000_0      # Snapshot X, file 0
...
```

Each file contains processed halos from subset of merger trees.

### HDF5 Output Structure

```
OutputDir/
├── model_000.hdf5      # File 0 (all snapshots)
├── model_001.hdf5      # File 1 (all snapshots)
...
```

Each HDF5 file contains:
```
model_000.hdf5
├── /Snap063/            # z=0 snapshot
│   ├── Galaxies         # Halo/galaxy data (structured array with HDF5 attributes)
│   ├── FieldMetadata    # Field names and units (structured table)
│   └── TreeHalosPerSnap # Halos per tree
├── /Snap062/            # z=0.27 snapshot
│   └── ...
...
```

The master file (`model.hdf5`) contains complete metadata for reproducibility:
```
model.hdf5
├── /RunProperties/                  # Run configuration and metadata
│   ├── @BoxSize, @Hubble_h, @Omega  # Simulation parameters (attributes)
│   ├── /Version/                    # Version information (subgroup)
│   │   ├── @git_commit              # Code version (SHA)
│   │   ├── @git_branch, @git_date   # Git metadata
│   │   └── @hdf5_format_version     # Schema version
│   ├── EnabledModules               # Pipeline configuration (compound dataset)
│   │                                # Fields: module_name, phase, processing_mode
│   ├── Parameters                   # Runtime parameters (dataset: param_name, value)
│   └── Redshifts                    # Snapshot redshifts z[64] (dataset)
├── /Snap063/            # Snapshot groups with external links to data files
│   ├── FieldMetadata    # Field descriptions (for self-documentation)
│   ├── File000/         # Links to model_000.hdf5
│   ├── File001/         # Links to model_001.hdf5
│   └── ...
...
```

Each per-file output (e.g., `model_000.hdf5`) contains the same `RunProperties` metadata, making files self-contained and analyzable independently.

## Troubleshooting

### "Unknown output format: hdf5"

**Problem**: Mimic not compiled with HDF5 support

**Solution**:
```bash
make clean
make USE-HDF5=yes
./mimic input/millennium.yaml
```

### "Property not found in output"

**Problem**: Trying to access a property that wasn't included in output

**Solution**: Check which modules were enabled during the run. Properties are only output if:
1. Defined with `output: true` in `metadata/*.yaml`
2. Set by a module during the run
3. Non-zero values written

### Reader dtype mismatch

**Problem**: Reading old output with updated code

**Solution**:
1. Regenerate output with current version
2. Or regenerate baseline if running tests
3. Both formats use same property schema - regenerating works for both

### Performance issues with large output

**Problem**: HDF5 writing is slow

**Solution**:
1. Switch to binary format for performance-critical runs
2. Or optimize HDF5 chunking/compression settings (if exposed)
3. Consider parallelizing output (future feature)

## See Also

- `docs/user/module-configuration.md` - Configuring physics modules
- `docs/developer/property-metadata-schema.md` - Property definition system
- `docs/developer/testing.md` - Output format testing
- `output/mimic-plot/` - Plotting tools with auto-detection
- `tests/integration/test_output_formats.py` - Format equivalence tests
