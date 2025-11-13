# Property Metadata Schema Specification

**Version**: 1.0
**Created**: 2025-11-07
**Status**: Implementation Specification
**Purpose**: Define the authoritative schema for property metadata in Mimic

---

## Quick Start

**Adding a new property? Here's what you need:**

1. **Edit the right file:**
   - Halo properties (core): `metadata/properties/halo_properties.yaml`
   - Galaxy properties (physics): `metadata/properties/galaxy_properties.yaml`

2. **Minimal property definition:**
```yaml
- name: MyNewProperty
  type: float
  units: "Msun"
  description: "My new galaxy property"
  output: true
  init_source: default
  init_value: 0.0f
```

3. **Regenerate code:**
```bash
make generate
```

4. **Use in your module:**
```c
float mass = get_MyNewProperty(galaxy);
set_MyNewProperty(galaxy, 1.5);
```

**Required fields:**
- `name` - C identifier (PascalCase recommended)
- `type` - Data type (`float`, `double`, `int`, `long`)
- `units` - Physical units (documentation)
- `description` - What this property represents
- `output` - Include in output files? (`true`/`false`)
- `init_source` - How to initialize (`default`, `copy_from_tree`, `calculate`)

**Common patterns:**
- Simple property: `init_source: default`, provide `init_value`
- From merger tree: `init_source: copy_from_tree`, provide tree field name
- Calculated: `init_source: calculate`, provide function name
- Output copy: Most properties use `output_source: copy_from_processing`

**Generated outputs:**
- `src/include/generated/property_defs.h` - Struct definitions (Halo, GalaxyData, HaloOutput)
- `src/include/generated/init_halo_properties.inc` - Halo initialization code
- `src/include/generated/init_galaxy_properties.inc` - Galaxy initialization code
- `src/include/generated/copy_to_output.inc` - Output copy logic
- `src/include/generated/hdf5_field_count.inc` - HDF5 field count
- `src/include/generated/hdf5_field_definitions.inc` - HDF5 field definitions
- `output/mimic-plot/generated/dtype.py` - Python NumPy dtypes
- `tests/generated/property_ranges.json` - Validation ranges

**Full schema details below.** This is a 990-line reference document - use Ctrl+F to find what you need.

---

## Overview

This document defines the YAML schema for property metadata in Mimic. Property metadata is the **single source of truth** for:
- Data structure definitions (struct Halo, struct GalaxyData, struct HaloOutput)
- Property initialization code (in `init_halo()`)
- Property output code (in `prepare_halo_for_output()`, HDF5 field definitions)
- Python data types (for plotting and analysis tools)
- Validation rules and test generation (scientific tests)

By defining properties once in metadata, we eliminate manual synchronization across 10+ files and enable rapid property addition (<2 minutes vs 30 minutes).

---

## Metadata File Organization

### Location

```
metadata/properties/
├── halo_properties.yaml      # Core halo tracking properties (24 fields)
└── galaxy_properties.yaml    # Baryonic physics properties (expandable)
```

### Rationale for Separation

- **halo_properties.yaml**: Core infrastructure, rarely changes, ~24 properties
- **galaxy_properties.yaml**: Physics modules, frequently extended, N properties

This separation allows physics developers to work in `galaxy_properties.yaml` without touching core infrastructure definitions.

---

## Schema Definition

### Property Structure

Each property is a YAML dictionary with the following fields:

```yaml
property_name:
  # REQUIRED FIELDS (all properties)
  name: string              # C identifier, becomes struct member name
  type: string              # Data type (see Supported Types)
  units: string             # Physical units (for documentation)
  description: string       # Human-readable description
  output: boolean           # Include in HaloOutput struct?

  # INITIALIZATION CONTROL (controls init_halo() generation)
  init_source: string       # How to initialize (see Init Sources)
  init_value: number|string # Default value (if init_source: default)
  init_function: string     # Function name (if init_source: calculate)

  # OUTPUT CONTROL (controls prepare_halo_for_output() generation)
  output_source: string     # How to copy to output (see Output Sources)
  output_tree_field: string # Tree field name (if output_source: copy_from_tree)
  output_function: string   # Function to call (if output_source: recalculate)
  output_function_arg: string # Argument expression (if output_source: recalculate)
  output_condition: string  # C expression (if output_source: conditional)
  output_true_value: string # Expression if condition true
  output_false_value: string # Expression if condition false

  # VALIDATION CONTROL (controls test generation)
  range: [min, max]         # Physical bounds for validation (inclusive)
  sentinels: [val1, ...]    # Special values excluded from validation

  # GALAXY PROPERTY EXTRAS
  created_by: string        # Module name (documentation, validation)
```

---

## Required Fields

### All Properties Must Have

**name** (string)
- Valid C identifier (alphanumeric + underscore, no spaces)
- Becomes struct member name
- Must be unique within property category
- Convention: PascalCase for readability (e.g., `StellarMass`, `Mvir`, `ColdGas`)

**type** (string)
- Data type for C struct
- Must be one of the supported types (see Supported Types section)
- Determines memory layout, alignment, and output format

**units** (string)
- Physical units as string
- Used for documentation and output file metadata
- Examples: `"1e10 Msun/h"`, `"Mpc/h"`, `"km/s"`, `"dimensionless"`
- Should match conventions used in parameter files

**description** (string)
- Human-readable description
- Used in documentation generation
- Should explain physical meaning and calculation method
- Examples: `"Virial mass (M200c)"`, `"Cold gas mass available for star formation"`

**output** (boolean)
- `true`: Include in struct HaloOutput (written to files)
- `false`: Internal processing only (not in output files)
- Most properties should be `true` for scientific analysis

---

## Supported Types

### Scalar Types

**int**
- C type: `int`
- NumPy type: `np.int32`
- HDF5 type: `H5T_NATIVE_INT`
- Use for: Counts, indices, flags

**float**
- C type: `float`
- NumPy type: `np.float32`
- HDF5 type: `H5T_NATIVE_FLOAT`
- Use for: Most physical quantities (mass, position, velocity)

**double**
- C type: `double`
- NumPy type: `np.float64`
- HDF5 type: `H5T_NATIVE_DOUBLE`
- Use for: High-precision calculations (rare, float usually sufficient)

**long long**
- C type: `long long`
- NumPy type: `np.int64`
- HDF5 type: `H5T_NATIVE_LLONG`
- Use for: Large indices (HaloIndex, particle IDs)

### Array Types

**vec3_float**
- C type: `float[3]`
- NumPy type: `(np.float32, 3)`
- HDF5 type: Array of 3 floats
- Use for: 3D vectors (position, velocity, spin)

**vec3_int**
- C type: `int[3]`
- NumPy type: `(np.int32, 3)`
- HDF5 type: Array of 3 ints
- Use for: Integer 3D data (rare)

### Type Mapping Table

| YAML Type    | C Type      | NumPy Type      | HDF5 Type         |
|--------------|-------------|-----------------|-------------------|
| int          | int         | np.int32        | H5T_NATIVE_INT    |
| float        | float       | np.float32      | H5T_NATIVE_FLOAT  |
| double       | double      | np.float64      | H5T_NATIVE_DOUBLE |
| long long    | long long   | np.int64        | H5T_NATIVE_LLONG  |
| vec3_float   | float[3]    | (np.float32, 3) | Array3f           |
| vec3_int     | int[3]      | (np.int32, 3)   | Array3i           |

---

## Initialization Control

Defines how property is initialized in `init_halo(int p, int halonr)`.

### init_source Options

**default**
- Use `init_value` as default
- Generated code: `FoFWorkspace[p].PropertyName = init_value;`
- Use for: Properties set by modules, merge flags, sentinel values
- Example:
  ```yaml
  - name: ColdGas
    type: float
    init_source: default
    init_value: 0.0
  ```

**copy_from_tree**
- Copy from `InputTreeHalos[halonr].{name}`
- Generated code: `FoFWorkspace[p].PropertyName = InputTreeHalos[halonr].PropertyName;`
- Use for: Properties directly from simulation (Pos, Vel, Len, Vmax)
- Example:
  ```yaml
  - name: Vmax
    type: float
    init_source: copy_from_tree
  ```

**copy_from_tree_array**
- Copy array from tree (special case for vec3_float, vec3_int)
- Generated code: `for (int j = 0; j < 3; j++) FoFWorkspace[p].Pos[j] = InputTreeHalos[halonr].Pos[j];`
- Use for: 3D vectors from simulation
- Example:
  ```yaml
  - name: Pos
    type: vec3_float
    init_source: copy_from_tree_array
  ```

**calculate**
- Call `init_function` to compute value
- Generated code: `FoFWorkspace[p].PropertyName = init_function(halonr);`
- Use for: Derived properties (Mvir, Rvir, Vvir calculated from tree data)
- Requires `init_function` field
- Example:
  ```yaml
  - name: Mvir
    type: float
    init_source: calculate
    init_function: get_virial_mass
  ```

**skip**
- Do not initialize (property not in struct Halo)
- Use for: Properties that only exist in HaloOutput (e.g., Spin, VelDisp)
- Example:
  ```yaml
  - name: Spin
    type: vec3_float
    init_source: skip  # Only in HaloOutput, not struct Halo
    output_source: copy_from_tree_array
  ```

---

## Output Control

Defines how property is copied from `struct Halo` to `struct HaloOutput` in `prepare_halo_for_output()`.

### output_source Options

**copy_direct**
- Direct copy: `o->PropertyName = g->PropertyName;`
- Use for: Most simple properties
- Example:
  ```yaml
  - name: Mvir
    type: float
    output_source: copy_direct
  ```

**copy_direct_array**
- Copy array with loop
- Generated code: `for (int j = 0; j < 3; j++) o->Pos[j] = g->Pos[j];`
- Use for: 3D vectors in struct Halo
- Example:
  ```yaml
  - name: Pos
    type: vec3_float
    output_source: copy_direct_array
  ```

**copy_from_tree**
- Copy from `InputTreeHalos[g->HaloNr].{output_tree_field}`
- Generated code: `o->PropertyName = InputTreeHalos[g->HaloNr].TreeField;`
- Use for: Properties in tree but not in struct Halo (Spin, VelDisp)
- Requires `output_tree_field` field
- Example:
  ```yaml
  - name: VelDisp
    type: float
    output_source: copy_from_tree
    output_tree_field: VelDisp
  ```

**copy_from_tree_array**
- Copy array from tree
- Generated code: `for (int j = 0; j < 3; j++) o->Spin[j] = InputTreeHalos[g->HaloNr].Spin[j];`
- Use for: 3D vectors in tree but not struct Halo
- Requires `output_tree_field` field
- Example:
  ```yaml
  - name: Spin
    type: vec3_float
    output_source: copy_from_tree_array
    output_tree_field: Spin
  ```

**recalculate**
- Call function to compute value at output time
- Generated code: `o->PropertyName = output_function(output_function_arg);`
- Use for: Properties that need recalculation (Rvir, Vvir, CentralMvir)
- Requires `output_function` and `output_function_arg` fields
- Example:
  ```yaml
  - name: Rvir
    type: float
    output_source: recalculate
    output_function: get_virial_radius
    output_function_arg: "g->HaloNr"
  ```

**conditional**
- Use condition to choose value
- Generated code:
  ```c
  if (output_condition) {
      o->PropertyName = output_true_value;
  } else {
      o->PropertyName = output_false_value;
  }
  ```
- Use for: Type-dependent properties (infall properties for satellites only)
- Requires `output_condition`, `output_true_value`, `output_false_value` fields
- Example:
  ```yaml
  - name: infallMvir
    type: float
    output_source: conditional
    output_condition: "g->Type != 0"
    output_true_value: "g->infallMvir"
    output_false_value: "0.0"
  ```

**custom**
- Hand-written code (emit comment in generated code)
- Generated code: `/* CUSTOM: PropertyName - see prepare_halo_for_output() */`
- Use for: Complex logic that doesn't fit patterns (HaloIndex encoding)
- Example:
  ```yaml
  - name: HaloIndex
    type: long long
    output_source: custom  # Complex encoding logic hand-written
  ```

**galaxy_property**
- Copy from galaxy struct
- Generated code: `o->PropertyName = g->galaxy->PropertyName;`
- Use for: All galaxy properties (ColdGas, StellarMass, etc.)
- Example:
  ```yaml
  - name: ColdGas
    type: float
    output_source: galaxy_property
  ```

---

## Galaxy Property Extras

### created_by (optional)

- Module name that creates/modifies this property
- Used for documentation and validation
- Helps track property provenance
- Example:
  ```yaml
  - name: ColdGas
    type: float
    created_by: simple_cooling  # This module creates the property
  ```

---

## Validation Control

Validation fields control automated testing and quality checks. These fields are used by `tests/scientific/test_scientific.py` to validate output data.

### range (optional)

**Format**: `[min, max]` (two-element list)

**Purpose**: Define physically reasonable bounds for property values (inclusive)

**When to use**:
- All physical quantities (masses, velocities, positions, etc.)
- Properties where you can define reasonable scientific bounds
- Helps catch bugs, numerical issues, and unphysical results

**Requirements**:
- Must be a list of exactly 2 numbers
- First value (min) must be ≤ second value (max)
- Range is **inclusive** on both ends
- Only applies to properties with `output: true`

**Examples**:
```yaml
# Mass property (internal units: 10^10 Msun/h)
- name: Mvir
  type: float
  units: "1e10 Msun/h"
  range: [1.0e-5, 1.0e4]  # 10^5 Msun/h to 10^14 Msun/h
  description: "Virial mass"

# Velocity property
- name: Vvir
  type: float
  units: "km/s"
  range: [10.0, 5000.0]  # Physical halo velocities
  description: "Virial velocity"

# Position in simulation box
- name: Pos
  type: vec3_float
  units: "Mpc/h"
  range: [0.0, 62.5]  # BoxSize for test data
  description: "3D position (comoving)"

# Particle count
- name: Len
  type: int
  units: "particles"
  range: [20, 1.0e9]  # Resolution limit to max particles
  description: "Number of particles in halo"
```

**Vector properties**: Each component is checked against the same range independently.

### sentinels (optional)

**Format**: `[value1, value2, ...]` (list of numbers)

**Purpose**: Define special values that should be **excluded** from range and zero checks

**When to use**:
- Sentinel values indicating "not set" (e.g., `-1.0`)
- Placeholder values for invalid/inapplicable states (e.g., `0.0` for central halos where infall properties don't apply)
- Properties that legitimately can be zero in valid scientific contexts

**Requirements**:
- Must be a list of numbers (can be empty)
- Values must match the property's type (int for int properties, float for float properties)
- Sentinel values are excluded from ALL validation checks (range, zero warnings)

**Examples**:
```yaml
# Infall property (only valid for satellites)
- name: infallMvir
  type: float
  units: "1e10 Msun/h"
  range: [1.0e-5, 1.0e4]
  sentinels: [0.0, -1.0]  # 0.0 for centrals, -1.0 for unset
  description: "Virial mass at infall time (satellites only)"

# Time between snapshots (can be -1 for orphans)
- name: dT
  type: float
  units: "Myr"
  range: [0.0, 2000.0]
  sentinels: [-1.0]  # Orphan halos have dT = -1
  description: "Time since last snapshot"

# Galaxy property (can legitimately be zero)
- name: StellarMass
  type: float
  units: "1e10 Msun/h"
  range: [0.0, 1.0e5]
  sentinels: [0.0]  # Halos with no stars yet
  description: "Total stellar mass"
```

**Vector properties**: Sentinel checking applies component-wise (each component can independently match a sentinel value).

### Validation Manifest Generation

When you run `make generate`, the validation fields are extracted into `tests/generated/property_ranges.json`:

```json
{
  "schema_version": 1,
  "generated_at": "2025-11-11T17:28:27",
  "properties": {
    "Mvir": {
      "name": "Mvir",
      "type": "float",
      "units": "1e10 Msun/h",
      "is_vector": false,
      "range": [1.0e-5, 10000.0]
    },
    "infallMvir": {
      "name": "infallMvir",
      "type": "float",
      "units": "1e10 Msun/h",
      "is_vector": false,
      "range": [1.0e-5, 10000.0],
      "sentinels": [0.0, -1.0]
    }
  }
}
```

This manifest is consumed by `tests/scientific/test_scientific.py` to validate ALL output properties dynamically.

### Automated Quality Checks

For ALL properties with `output: true`, the scientific test automatically performs:

1. **NaN/Inf Detection** (always checked, always fails if found)
   - No configuration needed
   - Applied to all output properties
   - Critical errors that always fail tests

2. **Zero Value Warnings** (always checked, warns but doesn't fail)
   - No configuration needed
   - Applied to all numeric output properties
   - Warnings help identify potential issues but don't fail tests
   - Use `sentinels: [0.0]` to suppress warnings for properties where zero is expected

3. **Range Validation** (only if `range` specified)
   - Fails if values outside `[min, max]` (inclusive)
   - Sentinel values are excluded before checking
   - Vector components checked independently

### Validation Best Practices

**Always add ranges for**:
- Physical quantities (masses, radii, velocities)
- Positions (bounded by simulation box)
- Counts (particle numbers, snapshot indices)

**Use sentinels for**:
- Type-specific properties (e.g., infall properties only valid for satellites)
- Time properties that can be -1 for special cases
- Properties that legitimately can be zero

**Don't add ranges for**:
- Internal tracking indices (HaloNr, UniqueHaloID)
- Encoded composite values (HaloIndex)
- Properties with no clear physical bounds

**Choosing range values**:
- Be generous but realistic (account for numerical precision)
- Use simulation parameters (e.g., BoxSize for positions)
- Consider resolution limits (e.g., minimum particle count)
- Check actual data ranges from test runs

---

## Complete Examples

### Example 1: Simple Property (Direct Copy from Tree)

```yaml
- name: SnapNum
  type: int
  units: "dimensionless"
  description: "Snapshot number"
  output: true
  init_source: copy_from_tree
  output_source: copy_direct
```

Generates:
```c
// Initialization:
FoFWorkspace[p].SnapNum = InputTreeHalos[halonr].SnapNum;

// Output:
o->SnapNum = g->SnapNum;
```

### Example 2: Calculated Property

```yaml
- name: Mvir
  type: float
  units: "1e10 Msun/h"
  description: "Virial mass (M200c)"
  output: true
  init_source: calculate
  init_function: get_virial_mass
  output_source: copy_direct
```

Generates:
```c
// Initialization:
FoFWorkspace[p].Mvir = get_virial_mass(halonr);

// Output:
o->Mvir = g->Mvir;
```

### Example 3: Galaxy Property (Physics Module)

```yaml
- name: ColdGas
  type: float
  units: "1e10 Msun/h"
  description: "Cold gas mass available for star formation"
  output: true
  created_by: simple_cooling
  init_source: default
  init_value: 0.0
  output_source: galaxy_property
```

Generates:
```c
// Initialization (after galaxy allocation):
FoFWorkspace[p].galaxy->ColdGas = 0.0;

// Output:
o->ColdGas = g->galaxy->ColdGas;
```

**Additional examples** (arrays, conditionals, recalculation, custom logic) are available in:
- `metadata/properties/halo_properties.yaml` - Core halo properties
- `metadata/properties/galaxy_properties.yaml` - Baryonic physics properties


## Property Categories

### Halo Properties (Core Infrastructure)

**Metadata** (7):
- SnapNum, Type, HaloNr, UniqueHaloID, CentralHalo, MostBoundID, HaloIndex

**Merge Tracking** (4):
- MergeStatus, mergeIntoID, mergeIntoSnapNum, dT

**Physical Properties** (10):
- Pos[3], Vel[3], Spin[3], Len, Mvir, CentralMvir, Rvir, Vvir, Vmax, VelDisp

**Infall Properties** (3):
- infallMvir, infallVvir, infallVmax

**Total**: 24 properties (21 in struct Halo, 24 in struct HaloOutput)

### Galaxy Properties (Baryonic Physics)

**Current** (2):
- ColdGas, StellarMass

**Planned** (expandable):
- HotGas, StellarMassFromMergers, MetalsCold, MetalsHot, BlackHoleMass, etc.

---

## Generated Code Structure

### struct Definitions (property_defs.h)

```c
/* AUTO-GENERATED - DO NOT EDIT */

/* Halo properties (internal processing) */
struct Halo {
    /* Metadata */
    int SnapNum;
    int Type;
    /* ... all halo properties ... */

    /* Galaxy pointer (physics-agnostic) */
    struct GalaxyData *galaxy;
};

/* Galaxy properties (baryonic physics) */
struct GalaxyData {
    float ColdGas;
    float StellarMass;
    /* ... expandable ... */
};

/* Output structure (file writing) */
struct HaloOutput {
    /* Halo properties */
    int SnapNum;
    /* ... all output=true halo properties ... */

    /* Galaxy properties */
    float ColdGas;
    /* ... all output=true galaxy properties ... */
};
```

### Initialization Code (init_halo_properties.inc, init_galaxy_properties.inc)

```c
/* AUTO-GENERATED - DO NOT EDIT */

/* Initialize halo properties */
FoFWorkspace[p].SnapNum = InputTreeHalos[halonr].SnapNum;
FoFWorkspace[p].Mvir = get_virial_mass(halonr);
for (int j = 0; j < 3; j++) {
    FoFWorkspace[p].Pos[j] = InputTreeHalos[halonr].Pos[j];
}
/* ... all properties ... */

/* Initialize galaxy properties */
FoFWorkspace[p].galaxy->ColdGas = 0.0;
FoFWorkspace[p].galaxy->StellarMass = 0.0;
/* ... all galaxy properties ... */
```

### Output Copy Code (copy_to_output.inc)

```c
/* AUTO-GENERATED - DO NOT EDIT */

/* Copy halo properties */
o->SnapNum = g->SnapNum;
o->Mvir = g->Mvir;
for (int j = 0; j < 3; j++) {
    o->Pos[j] = g->Pos[j];
}
if (g->Type != 0) {
    o->infallMvir = g->infallMvir;
} else {
    o->infallMvir = 0.0;
}
/* ... all properties ... */

/* Copy galaxy properties */
o->ColdGas = g->galaxy->ColdGas;
o->StellarMass = g->galaxy->StellarMass;
/* ... all galaxy properties ... */
```

### HDF5 Field Definitions (hdf5_field_count.inc, hdf5_field_definitions.inc)

hdf5_field_count.inc:
```c
/* AUTO-GENERATED - DO NOT EDIT */
HDF5_n_props = 26;  /* 24 halo + 2 galaxy */
int i = 0;
```

hdf5_field_definitions.inc:
```c
/* AUTO-GENERATED - DO NOT EDIT */
struct HaloOutput galout;

/* SnapNum */
HDF5_dst_offsets[i] = HOFFSET(struct HaloOutput, SnapNum);
HDF5_dst_sizes[i] = sizeof(galout.SnapNum);
HDF5_field_names[i] = "SnapNum";
HDF5_field_types[i++] = H5T_NATIVE_INT;

/* Mvir */
HDF5_dst_offsets[i] = HOFFSET(struct HaloOutput, Mvir);
HDF5_dst_sizes[i] = sizeof(galout.Mvir);
HDF5_field_names[i] = "Mvir";
HDF5_field_types[i++] = H5T_NATIVE_FLOAT;

/* ... all properties ... */
```

### Python Dtype (generated_dtype.py)

```python
# AUTO-GENERATED - DO NOT EDIT
import numpy as np

def get_binary_dtype():
    """Return NumPy dtype for binary output format."""
    return np.dtype([
        ("SnapNum", np.int32),
        ("Type", np.int32),
        # ... all properties ...
        ("ColdGas", np.float32),
        ("StellarMass", np.float32),
    ], align=True)

def get_hdf5_dtype():
    """Return NumPy dtype for HDF5 output format."""
    return np.dtype([
        ("SnapNum", np.int32),
        ("Type", np.int32),
        # ... all properties ...
        ("ColdGas", np.float32),
        ("StellarMass", np.float32),
    ])
```

---

## Validation Rules

The code generator (`generate_properties.py`) must validate:

### Required Field Checks
- All required fields present (name, type, units, description, output)
- init_source specified for all halo properties
- output_source specified if output=true

### Type Validation
- `type` is one of: int, float, double, long long, vec3_float, vec3_int
- `init_source` is one of: default, copy_from_tree, copy_from_tree_array, calculate, skip
- `output_source` is one of: copy_direct, copy_direct_array, copy_from_tree, copy_from_tree_array, recalculate, conditional, custom, galaxy_property

### Field Dependency Checks
- If `init_source: default`, require `init_value`
- If `init_source: calculate`, require `init_function`
- If `output_source: copy_from_tree`, require `output_tree_field`
- If `output_source: recalculate`, require `output_function` and `output_function_arg`
- If `output_source: conditional`, require `output_condition`, `output_true_value`, `output_false_value`

### Name Validation
- Property names must be valid C identifiers
- No duplicate names within a category
- No C reserved keywords (int, float, return, struct, etc.)

### Consistency Checks
- Array types (vec3_float, vec3_int) must use array init/output sources
- Scalar types cannot use array sources
- If output=false, output_source can be omitted

---

## Migration Strategy

### Phase 1: Halo Properties (Week 2)
1. Write all 24 halo properties to halo_properties.yaml
2. Generate code and verify bit-identical struct layout
3. Validate Millennium output matches baseline exactly

### Phase 2: Galaxy Properties (Week 3)
1. Write StellarMass and ColdGas to galaxy_properties.yaml
2. Update PoC modules to use generated properties
3. Validate PoC output matches baseline

### Phase 3: Expansion (Ongoing)
1. Add new galaxy properties as modules are developed
2. Each property: edit YAML, run `make generate`, done in <2 minutes

---

## Best Practices

### Documentation
- Write clear, specific descriptions
- Include units explicitly
- Document calculation method if non-obvious
- Reference papers for complex physics

### Organization
- Group related properties in YAML (metadata, tracking, physics)
- Use blank lines to separate logical groups
- Add comments explaining complex properties

### Naming
- Use PascalCase for readability (StellarMass, not stellar_mass)
- Be specific (infallMvir, not Mvir2)
- Avoid abbreviations unless standard (Mvir, Vmax are OK)

### Defaults
- Use -1.0 for unset float properties (distinguishes from 0.0)
- Use -1 for unset int properties
- Use 0.0/0 for physical quantities that start at zero

---

## Version History

- **v1.0** (2025-11-07): Initial specification based on PoC analysis

---

**This specification is authoritative for property metadata in Mimic. All property definitions must conform to this schema.**
