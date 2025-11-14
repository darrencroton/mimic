# Metadata Directory

**Purpose:** YAML definitions for properties, parameters, and modules that drive code generation.

**Status:** ✅ **Active** - Property Metadata System (Phase 1) implemented

## Current Implementation

### Property Definitions

The property metadata system eliminates manual synchronization across 8+ files by defining all halo and galaxy properties in YAML. Code generation automatically creates C structures, initialization code, output code, and Python dtypes.

**Files:**
- `halo_properties.yaml` - 30 core halo tracking properties (physics-agnostic)
- `galaxy_properties.yaml` - 2 validated galaxy properties (ColdGas, StellarMass)

**Generated Output:**
- `src/include/generated/property_defs.h` - Struct definitions
- `src/include/generated/init_halo_properties.inc` - Initialization code
- `src/include/generated/init_galaxy_properties.inc` - Galaxy initialization
- `src/include/generated/copy_to_output.inc` - Output copy logic
- `src/include/generated/hdf5_field_count.inc` - HDF5 field count
- `src/include/generated/hdf5_field_definitions.inc` - HDF5 field definitions
- `output/mimic-plot/generated/dtype.py` - NumPy dtypes for plotting
- `output/mimic-plot/generated/__init__.py` - Python package init
- `tests/generated/property_ranges.json` - Validation ranges for tests

**Usage:**
```bash
# Regenerate code after modifying YAML files
make generate

# Verify generated code is up-to-date (CI check)
make check-generated
```

**Benefits:**
- Property addition: 30 minutes → <2 minutes (60x speedup)
- Code reduction: 222 lines eliminated
- Single source of truth: YAML metadata
- Type safety: Generated structs and accessors

**Documentation:**
See `docs/architecture/property-metadata-schema.md` for complete specification.

## Future Expansion

**Planned:**
- `parameters/` - Configuration parameter definitions with automatic validation
- `modules/` - Physics module registry for runtime configuration

See `docs/architecture/vision.md` for architectural principles and future plans.
