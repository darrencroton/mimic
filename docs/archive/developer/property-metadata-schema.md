# Property Metadata Schema Specification

**Version**: 1.0
**Created**: 2025-11-07
**Status**: Implementation Specification
**Purpose**: Define the authoritative schema for property metadata in Mimic

---

## Quick Start

**Adding a new property? Here's what you need:**

1. **Edit the right file:**
   - Halo properties (core): `src/core/halo_properties.yaml`
   - Galaxy properties (physics): `src/modules/model_properties.yaml`

2. **Minimal property definition:**
```yaml
- name: MyNewProperty
  type: float
  units: Msun
  description: My new galaxy property
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
- Output copy: Most properties use `output_source: copy_direct` or `galaxy_property`
- Unit conversion: Add `output_convert` for systematic unit conversion at I/O boundary

**Generated outputs:**
- `src/include/generated/property_defs.h` - Struct definitions (Halo, GalaxyData, HaloOutput)
- `src/include/generated/init_halo_properties.inc` - Halo initialization code
- `src/include/generated/init_galaxy_properties.inc` - Galaxy initialization code
- `src/include/generated/reset_galaxy_properties.inc` - Galaxy property reset code (for init_repeat: true)
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
src/
├── core/
│   └── halo_properties.yaml      # Core halo tracking properties (31 properties)
└── modules/
    └── model_properties.yaml    # Baryonic physics properties (24 properties)
```

### Rationale for Separation

- **halo_properties.yaml**: Core infrastructure, rarely changes, 31 properties
- **model_properties.yaml**: Physics modules, frequently extended, 24 properties (expandable)

This separation allows physics developers to work in `model_properties.yaml` without touching core infrastructure definitions.

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
  init_repeat: boolean      # Reset each snapshot? (galaxy properties only, default: false)

  # OUTPUT CONTROL (controls prepare_halo_for_output() generation)
  output_source: string     # How to copy to output (see Output Sources)
  output_tree_field: string # Tree field name (if output_source: copy_from_tree)
  output_function: string   # Function to call (if output_source: recalculate)
  output_function_arg: string # Argument expression (if output_source: recalculate)
  output_condition: string  # C expression (if output_source: conditional)
  output_true_value: string # Expression if condition true
  output_false_value: string # Expression if condition false
  output_convert: string    # Optional unit conversion expression (see Unit Conversion)

  # VALIDATION CONTROL (controls test generation)
  range: [min, max]         # Physical bounds for validation (inclusive)
  sentinels: [val1, ...]    # Special values excluded from validation

  # GALAXY PROPERTY EXTRAS
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
- Examples: `1e10 Msun/h`, `Mpc/h`, `km/s`, `dimensionless`
- Should match conventions used in parameter files
- Note: Quotes optional in YAML unless string contains special characters

**description** (string)
- Human-readable description
- Used in documentation generation
- Should explain physical meaning and calculation method
- Examples: `Virial mass (M200c)`, `Cold gas mass available for star formation`
- Note: Quotes optional in YAML unless string contains special characters

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
- Use for: Large indices (UniqueGalaxyID, particle IDs)

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

### init_repeat (Galaxy Properties Only)

**Purpose**: Controls whether a property should be reset to its `init_value` each snapshot after being copied from progenitors.

**Type**: Boolean (`true` or `false`)

**Default**: `false` (omit field if not needed)

**Only applies to**:
- Galaxy properties (not halo properties)
- Properties with `init_source: default`

**Use case**: Snapshot-scoped accumulator properties that track values during a single timestep rather than cumulative values across cosmic time.

**When to use `init_repeat: true`**:
- Energy accumulators (e.g., Cooling, Heating) - accumulated during snapshot, then normalized by timestep
- Snapshot trackers (e.g., QuasarModeBHaccretionMass) - tracks activity during current snapshot only
- Rate properties (e.g., OutflowRate) - recalculated each snapshot

**When NOT to use** (default `false` or omit):
- Mass components (ColdGas, StellarMass, etc.) - cumulative across all time
- Historical properties (TimeOfLastMajorMerger, etc.) - preserve from progenitors
- Structural properties (DiskScaleRadius, etc.) - evolve over time

**Implementation**:
- During halo copy from progenitors, all galaxy properties are copied via `memcpy()`
- For central halos only, properties with `init_repeat: true` are then reset to `init_value`
- Generated code in: `src/include/generated/reset_galaxy_properties.inc`
- Applied in: `src/core/build_model.c` within `copy_halos_from_progenitors()`

**Example - Snapshot-scoped accumulator**:
```yaml
- name: Cooling
  type: double
  units: (km/s)^2 * 1e10 Msun/h
  description: Cumulative cooling energy (0.5 * m * V_vir^2)
  output: true
  init_source: default
  init_value: 0.0
  init_repeat: true  # Reset each snapshot
  output_source: galaxy_property
```

**Example - Cumulative property (no reset)**:
```yaml
- name: StellarMass
  type: float
  units: 1e10 Msun/h
  description: Total stellar mass
  output: true
  init_source: default
  init_value: 0.0
  # init_repeat: false (default - omit field)
  output_source: galaxy_property
```

**Current properties using `init_repeat: true`** (as of 2025-12-03):
1. `Cooling` - Snapshot cooling energy accumulator
2. `Heating` - Snapshot AGN heating energy accumulator
3. `QuasarModeBHaccretionMass` - Snapshot BH accretion tracker
4. `OutflowRate` - Current snapshot outflow rate

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

**conditional** (DEPRECATED - use `recalculate` instead)
- **Status**: Deprecated as of 2025-11-29
- **Replacement**: Use `recalculate` with helper functions from `src/modules/shared/output_helpers.h`
- **Migration**: See migration guide below

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
- **Why deprecated**:
  - Requires 3 fields instead of 2 (`recalculate` pattern)
  - Logic not testable (embedded in metadata)
  - Not reusable across properties
  - Helper functions more transparent and maintainable
- Example (old pattern - DO NOT USE):
  ```yaml
  - name: infallMvir
    type: float
    output_source: conditional
    output_condition: "g->Type != 0"
    output_true_value: "g->infallMvir"
    output_false_value: "0.0"
  ```
- **Preferred alternative** (use `recalculate`):
  ```yaml
  - name: infallMvir
    type: float
    output_source: recalculate
    output_function: output_infall_property_or_zero
    output_function_arg: "g, g->infallMvir"
  ```

**custom**
- Hand-written code (emit comment in generated code)
- Generated code: `/* CUSTOM: PropertyName - see prepare_halo_for_output() */`
- Use for: Complex logic that doesn't fit patterns (UniqueGalaxyID encoding)
- Example:
  ```yaml
  - name: UniqueGalaxyID
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

## Unit Conversion (output_convert)

### Overview

The `output_convert` field enables **systematic unit conversion at the I/O boundary** while maintaining Mimic's "code units everywhere" philosophy internally.

**Purpose**: Convert properties from internal code units to human-readable output units when writing files.

**Key Principle**: All physics calculations use consistent code units internally. Unit conversions happen ONLY at I/O boundaries (reading input, writing output).

### When to Use

**Common use cases**:
- Time conversions: seconds (code) → Myr (output)
- Energy conversions: code units → physical units
- Any property where output units differ from internal units

**When NOT to use**:
- Properties already in desired output units (most properties)
- Internal-only properties (output: false)
- Properties requiring complex logic (use `output_source: recalculate` instead)

### Field Specification

**Type**: String (C expression)

**Optional**: Yes (omit if no conversion needed)

**Requirements**:
- Must be valid C expression
- Can reference `ctx->params->Unit*_in_cgs` (UnitTime_in_s, UnitMass_in_g, etc.)
- Can reference constants from `constants.h` (SEC_PER_MEGAYEAR, etc.)
- Applied as multiplication: `value *= expression`

### Automatic Sentinel Handling

**Critical feature**: Sentinel values are automatically preserved unchanged.

If property has `sentinels` field:
- Generator creates conditional conversion code
- Each sentinel value excluded from conversion
- Ensures special values (like -1.0 for "not set") remain intact

### Examples

**Example 1: Time Conversion (seconds → Myr)**

```yaml
- name: dT
  type: float
  units: Myr
  description: Time since last snapshot
  output: true
  init_source: calculate
  init_function: get_time_difference
  output_source: copy_direct
  output_convert: "UnitTime_in_s / SEC_PER_MEGAYEAR"
  sentinels: [-1.0]  # Orphan halos have dT = -1
```

Generated code:
```c
// Copy value
o->dT = g->dT;

// Apply conversion (skip sentinels)
if (o->dT != -1.0f) {
    o->dT *= UnitTime_in_s / SEC_PER_MEGAYEAR;
}
```

**Example 2: Multiple Sentinels**

```yaml
- name: SomeProperty
  type: float
  units: physical_units
  output: true
  output_source: copy_direct
  output_convert: "UnitMass_in_g / SOLAR_MASS"
  sentinels: [0.0, -1.0, -999.0]
```

Generated code:
```c
o->SomeProperty = g->SomeProperty;
if (o->SomeProperty != 0.0f && o->SomeProperty != -1.0f && o->SomeProperty != -999.0f) {
    o->SomeProperty *= UnitMass_in_g / SOLAR_MASS;
}
```

**Example 3: No Sentinels**

```yaml
- name: ConvertedValue
  type: float
  units: converted_units
  output: true
  output_source: copy_direct
  output_convert: "some_conversion_factor"
```

Generated code:
```c
o->ConvertedValue = g->ConvertedValue;
o->ConvertedValue *= some_conversion_factor;
```

### Unit Conversion Best Practices

**1. Code Units Everywhere**
- All internal calculations use consistent code units
- Mass: 10^10 Msun/h
- Length: Mpc/h
- Velocity: km/s
- Time: derived from length/velocity

**2. Convert Only at Boundaries**
- Input: Convert from file units → code units (in tree readers)
- Processing: Use code units exclusively
- Output: Convert from code units → output units (via `output_convert`)

**3. Document Both Units**
- `units` field should reflect OUTPUT units (what users see in files)
- Code comments should note internal code units if different
- Example: `units: Myr  # Output; stored internally as seconds in code units`

**4. Use Named Constants**
- Prefer `SEC_PER_MEGAYEAR` over `3.15576e13`
- Prefer `UnitTime_in_s` over hardcoded values
- Makes conversions self-documenting and maintainable

**5. Validate Conversions**
- Check converted values in scientific tests
- Verify ranges match expected output units
- Test sentinel preservation

### Combining with Other Features

**With recalculate**:
```yaml
- name: ComputedProperty
  type: float
  units: output_units
  output_source: recalculate
  output_function: compute_property
  output_function_arg: "g"
  output_convert: "conversion_factor"
  sentinels: [-1.0]
```

Order of operations:
1. Call `compute_property(g)` → get value
2. Assign to `o->ComputedProperty`
3. If not sentinel, apply conversion

**With conditional**:
```yaml
- name: ConditionalProperty
  type: float
  units: output_units
  output_source: conditional
  output_condition: "g->Type != 0"
  output_true_value: "g->SomeValue"
  output_false_value: "0.0"
  output_convert: "conversion_factor"
  sentinels: [0.0]
```

Order of operations:
1. Evaluate condition → choose value
2. Assign to `o->ConditionalProperty`
3. If not sentinel (0.0), apply conversion

**Not compatible with**:
- `output_source: custom` - custom code handles its own conversions
- `output_source: galaxy_property` - galaxy properties already in code units

### Migrating from Conditional to Recalculate

**Background**: The `conditional` output source is deprecated in favor of `recalculate` with helper functions.

**Why migrate**:
- Reduces metadata fields (3 → 2)
- Makes logic testable (helper functions can have unit tests)
- Enables reuse across multiple properties
- More transparent and maintainable

**Migration steps**:

1. **Identify the pattern** in your conditional:
   ```yaml
   # Old pattern
   output_source: conditional
   output_condition: "g->Type != 0"
   output_true_value: "g->infallMvir"
   output_false_value: "0.0"
   ```

2. **Check for existing helper** in `src/modules/shared/output_helpers.h`:
   - Infall properties: Use `output_infall_property_or_zero`
   - Other patterns: Check if helper exists

3. **If helper exists**, update metadata:
   ```yaml
   # New pattern
   output_source: recalculate
   output_function: output_infall_property_or_zero
   output_function_arg: "g, g->infallMvir"
   ```

4. **If helper doesn't exist**, create one:
   ```c
   // In src/modules/shared/output_helpers.h
   static inline float output_your_pattern(const struct Halo *g, ...) {
       // Your conditional logic here
       return (condition) ? true_value : false_value;
   }
   ```

5. **Update property metadata** to use new helper

6. **Test** with `make generate && make clean && make tests`

**Complete example - Infall properties**:

Before (3 properties × 3 fields each = 9 fields):
```yaml
- name: infallMvir
  output_source: conditional
  output_condition: "g->Type != 0"
  output_true_value: "g->infallMvir"
  output_false_value: "0.0"

- name: infallVvir
  output_source: conditional
  output_condition: "g->Type != 0"
  output_true_value: "g->infallVvir"
  output_false_value: "0.0"

- name: infallVmax
  output_source: conditional
  output_condition: "g->Type != 0"
  output_true_value: "g->infallVmax"
  output_false_value: "0.0"
```

After (3 properties × 2 fields each = 6 fields + 1 reusable helper):
```yaml
- name: infallMvir
  output_source: recalculate
  output_function: output_infall_property_or_zero
  output_function_arg: "g, g->infallMvir"

- name: infallVvir
  output_source: recalculate
  output_function: output_infall_property_or_zero
  output_function_arg: "g, g->infallVvir"

- name: infallVmax
  output_source: recalculate
  output_function: output_infall_property_or_zero
  output_function_arg: "g, g->infallVmax"
```

Helper function (reusable, testable):
```c
// src/modules/shared/output_helpers.h
static inline float output_infall_property_or_zero(const struct Halo *g, float value) {
    return (g->Type != 0) ? value : 0.0f;
}
```

**Benefits**:
- 33% reduction in metadata fields (9 → 6)
- Single testable function instead of 3 embedded conditionals
- Clear intent: "infall property or zero" vs conditional logic
- Easy to add new infall properties (just reference same helper)

### Architecture Notes

**Maintains Core-Physics Separation**:
- Conversion expressions defined in metadata (YAML)
- Code auto-generated by `scripts/generate_properties.py`
- Core has no hardcoded unit conversions (stays physics-agnostic)

**HDF5 Integration**:
- Per-field unit attributes written to HDF5 (from `units` field)
- Users can verify output units programmatically
- Python tools receive unit metadata automatically

**Binary Format**:
- Unit header written to binary files (global unit system)
- Individual conversions applied per-property
- Maintains backward compatibility

---

## Galaxy Property Extras

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
  units: 1e10 Msun/h
  range: [1.0e-5, 1.0e4]  # 10^5 Msun/h to 10^14 Msun/h
  description: Virial mass

# Velocity property
- name: Vvir
  type: float
  units: km/s
  range: [10.0, 5000.0]  # Physical halo velocities
  description: Virial velocity

# Position in simulation box
- name: Pos
  type: vec3_float
  units: Mpc/h
  range: [0.0, 62.5]  # BoxSize for test data
  description: 3D position (comoving)

# Particle count
- name: Len
  type: int
  units: particles
  range: [20, 1.0e9]  # Resolution limit to max particles
  description: Number of particles in halo
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
  units: 1e10 Msun/h
  range: [1.0e-5, 1.0e4]
  sentinels: [0.0, -1.0]  # 0.0 for centrals, -1.0 for unset
  description: Virial mass at infall time (satellites only)

# Time between snapshots (can be -1 for orphans)
- name: dT
  type: float
  units: Myr
  range: [0.0, 2000.0]
  sentinels: [-1.0]  # Orphan halos have dT = -1
  description: Time since last snapshot

# Galaxy property (can legitimately be zero)
- name: StellarMass
  type: float
  units: 1e10 Msun/h
  range: [0.0, 1.0e5]
  sentinels: [0.0]  # Halos with no stars yet
  description: Total stellar mass
```

**Vector properties**: Sentinel checking applies component-wise (each component can independently match a sentinel value).

### Validation Manifest Generation

When you run `make generate`, the validation fields are extracted into `tests/generated/property_ranges.json`. The manifest starts with a `_metadata` block that mirrors the auto-generated headers used elsewhere in Mimic, followed by the schema payload:

```json
{
  "_metadata": {
    "auto_generated": true,
    "generated_by": "scripts/generate_properties.py",
    "source_files": [
      "src/core/halo_properties.yaml",
      "src/modules/model_properties.yaml"
    ],
    "source_md5": "c3a35676282c5fd9d1c2e52716d3a80c",
    "regenerate": "make generate"
  },
  "schema_version": 1,
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
  },
  "notes": "Auto-generated from src/core/halo_properties.yaml and src/modules/model_properties.yaml. Range is inclusive; sentinels are exempt."
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
- Internal tracking indices (HaloNr)
- Encoded composite values (UniqueGalaxyID)
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
  units: dimensionless
  description: Snapshot number
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
  units: 1e10 Msun/h
  description: Virial mass (M200c)
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
  units: 1e10 Msun/h
  description: Cold gas mass available for star formation
  output: true
  init_source: default
  init_value: 0.0
  output_source: galaxy_property
  range: [0.0, 100000.0]
  sentinels: [0.0]
```

Generates:
```c
// Initialization (after galaxy allocation):
FoFWorkspace[p].galaxy->ColdGas = 0.0;

// Output:
o->ColdGas = g->galaxy->ColdGas;
```

**Additional examples** (arrays, conditionals, recalculation, custom logic) are available in:
- `src/core/halo_properties.yaml` - Core halo properties
- `src/modules/model_properties.yaml` - Baryonic physics properties

## Property Categories

### Halo Properties (Core Infrastructure)

**Total**: 31 properties

Organized by category:
- Metadata (SnapNum, Type, HaloNr, etc.)
- Merge tracking (MergeStatus, mergeIntoID, etc.)
- Physical properties (Pos, Vel, Spin, Mvir, Rvir, Vvir, etc.)
- Infall properties (infallMvir, infallVvir, infallVmax)

See `src/core/halo_properties.yaml` for complete definitions.

### Galaxy Properties (Baryonic Physics)

**Total**: 24 properties (expandable)

Organized by component:
- Gas components (ColdGas, HotGas, EjectedMass)
- Stellar components (StellarMass, BulgeMass, ICS)
- Chemical composition (MetalsColdGas, MetalsHotGas, MetalsStellarMass, etc.)
- Black holes (BlackHoleMass, QuasarModeBHaccretionMass)
- Energy & feedback (Cooling, Heating, r_heat, OutflowRate)
- Structure (DiskScaleRadius)
- Merger history (TimeOfLastMajorMerger, TimeOfLastMinorMerger)
- Testing (TestDummyProperty)

See `src/modules/model_properties.yaml` for complete definitions.

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
HDF5_n_props = 55;  /* 31 halo + 24 galaxy */
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
- `output_convert` is optional and can be used with any `output_source` (except `custom`)

### Name Validation
- Property names must be valid C identifiers
- No duplicate names within a category
- No C reserved keywords (int, float, return, struct, etc.)

### Consistency Checks
- Array types (vec3_float, vec3_int) must use array init/output sources
- Scalar types cannot use array sources
- If output=false, output_source can be omitted

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
