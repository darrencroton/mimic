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
- **Self-describing**: Yes (includes metadata, units, descriptions)
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

1. Properties defined in `metadata/properties/*.yaml`
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

    # Properties are self-documented
    for prop in galaxies.dtype.names:
        if prop in snap.attrs:
            print(f"  {prop}: {snap.attrs[prop]}")
```

### Module Dependencies

Properties are provided by modules. To get specific properties:

| Property | Required Module | Configuration |
|----------|----------------|---------------|
| Mvir, Rvir, Vmax, Spin | Core (always) | No modules needed |
| ColdGas | simple_cooling | `EnabledModules simple_cooling` |
| StellarMass | simple_sfr + cooling | `EnabledModules simple_cooling,simple_sfr` |

**Example**: To get both ColdGas and StellarMass in output:

```
EnabledModules  simple_cooling,simple_sfr

SimpleCooling_BaryonFraction  0.15
SimpleSFR_Efficiency  0.02
```

## Reading Output Files

### Using mimic-plot

The `mimic-plot` tool automatically detects available properties and adapts:

```bash
# Auto-detects format (binary or HDF5) and available properties
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.par

# Generate specific plots (skips plots requiring unavailable properties)
python output/mimic-plot/mimic-plot.py --param-file=input/millennium.par \
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
├── /Snap063/           # z=0 snapshot
│   ├── Galaxies        # Halo/galaxy data (structured array)
│   ├── TreeHalosPerSnap  # Halos per tree
│   └── Attributes      # Metadata (redshift, hubble_h, etc.)
├── /Snap062/           # z=0.27 snapshot
│   └── ...
...
```

## Troubleshooting

### "Unknown output format: hdf5"

**Problem**: Mimic not compiled with HDF5 support

**Solution**:
```bash
make clean
make USE-HDF5=yes
./mimic input/millennium.par
```

### "Property not found in output"

**Problem**: Trying to access a property that wasn't included in output

**Solution**: Check which modules were enabled during the run. Properties are only output if:
1. Defined with `output: true` in `metadata/properties/*.yaml`
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
- `docs/architecture/property-metadata-schema.md` - Property definition system
- `docs/developer/testing.md` - Output format testing
- `output/mimic-plot/` - Plotting tools with auto-detection
- `tests/integration/test_output_formats.py` - Format equivalence tests
