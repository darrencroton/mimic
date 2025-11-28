# Model Parameters Metadata Schema Specification

**Version**: 1.0
**Created**: 2025-11-28
**Status**: Implementation Specification
**Purpose**: Define the authoritative schema for model parameter metadata in Mimic

---

## Quick Start

**Adding a new model parameter? Here's what you need:**

1. **Edit the metadata file:**
   - `src/modules/model_parameters.yaml`

2. **Minimal parameter definition:**
```yaml
- name: MyNewParameter
  type: double
  range: [0.0, 1.0]
  units: dimensionless
  description: My new physics parameter
  recommended: 0.5
  references: Smith et al. 2020
  scientific_notes: |
    Brief scientific background on this parameter.
    What physics does it control?
```

3. **Regenerate code:**
```bash
make generate
```

4. **Use in your module:**
```c
// In module_info.yaml, declare dependency:
dependencies:
  requires:
    parameters: [MyNewParameter]

// In your module code:
double value = get_model_param_double("MyNewParameter");
```

5. **Add to input files:**
```yaml
# input/millennium.yaml
model_parameters:
  MyNewParameter: 0.5
```

**Required fields:**
- `name` - C identifier (e.g., BaryonFrac, SfrEfficiency)
- `type` - Data type (`int`, `double`, `float`, `string`)
- `range` - Valid range `[min, max]` for numeric types, `null` for strings
- `units` - Physical units (for documentation and validation)
- `description` - Brief description of parameter
- `recommended` - Recommended/default value (documentation only - NOT used as default!)
- `references` - Scientific papers or models
- `scientific_notes` - Scientific background (multi-line YAML string)

**Generated outputs:**
- `src/include/generated/model_parameters.h` - Parameter metadata struct, validation functions
- `src/include/generated/model_parameters.c` - Validation implementation, smart lookup

**Important:** All parameters are **REQUIRED** in input files - no defaults are used. This ensures reproducible science.

**Full schema details below.**

---

## Overview

This document defines the YAML schema for model parameter metadata in Mimic. Model parameter metadata is the **single source of truth** for:
- Parameter structure (name, type, valid range, units)
- Validation rules (range checking, type checking)
- Module dependencies (which parameters each module requires)
- Smart parameter validation (only require parameters for enabled modules)
- Scientific documentation (references, recommended values, background)

By defining parameters once in metadata, we eliminate manual synchronization and enable type-safe, validated parameter access.

---

## Architecture Principles

The model parameter system implements **Vision Principles #3, #4, and #8**:

### Principle #3: Metadata-Driven Architecture
- YAML defines parameter structure
- Code generation creates type-safe validation
- Single source of truth for all parameter information

### Principle #4: Single Source of Truth
- `model_parameters.yaml` defines parameter STRUCTURE (name, type, range)
- `module_info.yaml` files declare parameter DEPENDENCIES (which modules need which parameters)
- Input YAML file provides parameter VALUES (user configuration)
- NO hardcoded defaults in source code

### Principle #8: Type Safety and Validation
- Auto-generated validation functions: `validate_model_param_double()`, `validate_model_param_int()`
- Range checking from metadata
- Clear error messages with parameter name and valid range
- Compile-time type safety through generated accessors

---

## No Defaults Philosophy

**IMPORTANT**: Mimic does NOT use default parameter values. All parameters must be explicitly specified in the input YAML file.

**Why?**
- **Reproducible Science**: Parameter files fully specify the model - no hidden assumptions
- **Explicit Models**: Researchers know exactly what physics is configured
- **Comparable Runs**: Different runs can be compared by comparing parameter files
- **No Surprises**: No "magic" defaults that change between versions

**Smart Validation**: Only parameters needed by enabled modules are required. If you disable a module, you don't need to specify its parameters.

---

## File Structure

### Location
```
src/modules/model_parameters.yaml
```

### Format
```yaml
model_parameters:
  - name: ParameterName
    type: double|int|float|string
    range: [min, max] | null
    units: dimensionless|Msun|Mpc|etc
    description: Brief description
    recommended: value
    references: Citation
    scientific_notes: |
      Multi-line scientific background
```

### Generated Files

| File | Purpose | Content |
|------|---------|---------|
| `src/include/generated/model_parameters.h` | Header | Metadata struct, validation function declarations, constants |
| `src/include/generated/model_parameters.c` | Implementation | Validation functions, metadata arrays, smart module lookup |

---

## Field Reference

### Required Fields

#### `name` (string, required)
**Purpose**: Parameter identifier used in code and configuration files.

**Requirements**:
- Must be valid C identifier (alphanumeric + underscore)
- No spaces, special characters
- Case-sensitive
- Must be unique across all parameters

**Convention**:
- PascalCase for multi-word names: `BaryonFrac`, `SfrEfficiency`, `ThreshMajorMerger`
- Descriptive and self-documenting
- Avoid abbreviations unless standard (e.g., `AGN`, `SF`)

**Examples**:
```yaml
name: BaryonFrac           # ✓ Good
name: SfrEfficiency        # ✓ Good
name: baryon-fraction      # ✗ Bad (invalid C identifier)
name: SF_EFF               # ⚠ Discouraged (unclear abbreviation)
```

---

#### `type` (string, required)
**Purpose**: Data type for parameter value.

**Valid values**:
- `int` - Integer values
- `double` - Double-precision floating point
- `float` - Single-precision floating point
- `string` - String values (file paths, names)

**Mapping to C types**:
```yaml
type: int     →  int
type: double  →  double
type: float   →  float
type: string  →  char*
```

**Examples**:
```yaml
type: double     # For most physics parameters (0.17, 0.01, etc.)
type: int        # For flags, modes, counts (0, 1, 2)
type: string     # For paths, file names
```

---

#### `range` (array or null, required for numeric types)
**Purpose**: Define valid value range for validation.

**Format**:
- Numeric types: `[min_value, max_value]`
- String types: `null` (no range validation)

**Validation**:
- Values outside range trigger ERROR-level validation failure
- Both min and max are **inclusive**
- Special cases:
  - Use `[0, 0]` for "only zero allowed" (e.g., placeholder parameters)
  - Use `[0.0, 1.0]` for fractions/efficiencies
  - Use `[0.0, INFINITY]` conceptually (though YAML doesn't support infinity literals)

**Examples**:
```yaml
range: [0.0, 1.0]          # Fraction (0-100%)
range: [0, 3]              # Mode selector (4 options: 0,1,2,3)
range: [0.0, 100.0]        # Large physical range
range: null                # String parameter (no numeric range)
```

---

#### `units` (string, required)
**Purpose**: Document physical units for scientific clarity and validation.

**Common values**:
- `dimensionless` - Ratios, fractions, pure numbers
- `Msun` - Solar masses
- `Msun/h` - Solar masses with h-scaling
- `Mpc` or `Mpc/h` - Megaparsecs
- `km/s` - Kilometers per second
- `Gyr` - Gigayears
- `code_units` - Internal simulation units
- `path` - File system path

**Purpose**:
- Documentation for users
- Validates dimensional consistency
- Helps catch configuration errors

**Examples**:
```yaml
units: dimensionless       # BaryonFrac (Omega_b / Omega_m)
units: code_units          # EnergySNcode (10^51 erg in code units)
units: path                # CoolFunctionsDir (directory path)
```

---

#### `description` (string, required)
**Purpose**: Brief, user-facing description of the parameter.

**Requirements**:
- Single line (no newlines)
- Concise (< 100 characters ideal)
- Self-contained (readable without references)
- Explains what parameter controls, not why or how

**Examples**:
```yaml
description: Cosmic baryon fraction (Omega_b / Omega_m)
description: Star formation efficiency (epsilon_SF in SFR equation)
description: Mass ratio threshold for major mergers
```

---

#### `recommended` (value, required)
**Purpose**: Recommended value from published models (SAGE defaults, literature values).

**Important**: This is **documentation only** - NOT used as a default! All parameters must be specified in input file.

**Format**:
- Type must match parameter `type`
- `int`: integer value
- `double`/`float`: numeric value
- `string`: quoted string

**Examples**:
```yaml
recommended: 0.17          # double
recommended: 1             # int
recommended: "input/CoolFunctions"  # string
```

---

#### `references` (string, required)
**Purpose**: Scientific citation for recommended value or physics prescription.

**Format**:
- Author et al. YEAR for papers
- Model name for established models
- Method name for standard techniques

**Examples**:
```yaml
references: Planck Collaboration 2018 (Planck 2018 results. VI.)
references: Croton et al. 2006 (MNRAS, 365, 11)
references: Kennicutt 1998 (ApJ, 498, 541)
references: Standard IMF-averaged supernova rate
```

---

#### `scientific_notes` (string, required)
**Purpose**: Multi-line scientific background, context, and usage guidance.

**Format**: YAML multi-line string (pipe `|` or `>`)

**Content should include**:
- Physical meaning and context
- Typical value ranges from literature
- How it affects galaxy formation physics
- Calibration information
- Interactions with other parameters
- Warnings or caveats

**Example**:
```yaml
scientific_notes: |
  The ratio of baryonic matter density to total matter density.
  Planck 2018: Omega_b = 0.0486, Omega_m = 0.315, ratio ~0.154-0.17

  This parameter affects:
  - Total gas available for galaxy formation
  - Normalization of cooling and star formation

  Typical range: 0.15 - 0.18 (consistent with CMB observations)
```

---

## Complete Example

```yaml
model_parameters:
  # =============================================================================
  # COSMOLOGY
  # =============================================================================

  - name: BaryonFrac
    type: double
    range: [0.0, 1.0]
    units: dimensionless
    description: Cosmic baryon fraction (Omega_b / Omega_m)
    recommended: 0.17
    references: Planck Collaboration 2018 (Planck 2018 results. VI. Cosmological parameters)
    scientific_notes: |
      The ratio of baryonic matter density to total matter density.
      Planck 2018: Omega_b = 0.0486, Omega_m = 0.315, ratio ~0.154-0.17

      Critical parameter affecting:
      - Total gas budget for galaxy formation
      - Normalization of all baryonic processes
      - Cosmic baryon conversion efficiency

      Observational constraints: 0.15 - 0.18 (CMB + BBN)

  # =============================================================================
  # STAR FORMATION
  # =============================================================================

  - name: SfrEfficiency
    type: double
    range: [0.0, 1.0]
    units: dimensionless
    description: Star formation efficiency (epsilon_SF in SFR = epsilon_SF * M_cold / t_dyn)
    recommended: 0.02
    references: Kennicutt 1998; Springel & Hernquist 2003
    scientific_notes: |
      Controls how efficiently cold gas forms stars per dynamical time.

      Star formation rate: SFR = epsilon_SF * M_cold / t_dyn
      where t_dyn is the disk dynamical time.

      Typical values: 0.01 - 0.05
      - Lower values: slower SF, more gas-rich galaxies
      - Higher values: faster SF, more stars, less gas

      Calibrated to match:
      - Kennicutt-Schmidt relation slope and normalization
      - Observed stellar mass functions at z=0
      - Gas depletion timescales in local galaxies
```

---

## Module Dependencies

Modules declare parameter dependencies in their `module_info.yaml`:

```yaml
module:
  name: sage_cooling
  dependencies:
    requires:
      parameters:
        - BaryonFrac
        - RadioModeEfficiency
        - AGNrecipeOn
        - CoolFunctionsDir
```

The system uses these declarations for **smart validation**:
- Only parameters needed by enabled modules are required
- Validation function: `get_required_params_for_modules()` (auto-generated)
- Physics-free mode (no modules) requires NO parameters

---

## Code Generation

### Generated Structures

**`src/include/generated/model_parameters.h`:**
```c
/* Parameter metadata structure */
struct ModelParameterMetadata {
    const char *name;          /* Parameter name */
    const char *type;          /* Type: int, double, string */
    const char *description;   /* Human-readable description */
    double range_min;          /* Minimum valid value */
    double range_max;          /* Maximum valid value */
    int has_range;             /* 1 if range applies, 0 otherwise */
};

#define NUM_REQUIRED_MODEL_PARAMETERS 22

extern const char *REQUIRED_MODEL_PARAMETERS[NUM_REQUIRED_MODEL_PARAMETERS];
extern const struct ModelParameterMetadata MODEL_PARAMETER_METADATA[NUM_REQUIRED_MODEL_PARAMETERS];
```

### Generated Validation

**`src/include/generated/model_parameters.c`:**
```c
/* Validate parameter value against metadata range */
int validate_model_param_double(const char *param_name, double value) {
    const struct ModelParameterMetadata *meta = get_model_param_metadata(param_name);
    if (meta == NULL) {
        ERROR_LOG("Model parameter '%s' not found in metadata", param_name);
        return -1;
    }

    if (!meta->has_range) return 0;

    if (value < meta->range_min || value > meta->range_max) {
        ERROR_LOG("Parameter %s = %g out of valid range [%g, %g]",
                  param_name, value, meta->range_min, meta->range_max);
        return -1;
    }

    return 0;
}

/* Smart module-based parameter lookup */
int get_required_params_for_modules(
    const char **enabled_modules,
    int num_enabled,
    const char **required_params_out,
    int *num_required_out
);
```

---

## Usage in Modules

### Declaring Dependencies

In `module_info.yaml`:
```yaml
dependencies:
  requires:
    parameters:
      - BaryonFrac
      - SfrEfficiency
      - RecycleFraction
```

### Accessing Parameters

In module code:
```c
#include "generated/model_parameters.h"
#include "core/module_registry.h"

void sage_starformation_init(void) {
    /* Get parameters (validated by core before module init) */
    double sfr_eff = get_model_param_double("SfrEfficiency");
    double recycle = get_model_param_double("RecycleFraction");
    int sf_mode = get_model_param_int("SFprescription");

    /* Parameters guaranteed to be:
     * 1. Present (required by this module)
     * 2. Valid (range-checked during parameter loading)
     * 3. Correct type (enforced by accessor functions)
     */
}
```

Helper functions (in `src/core/module_registry.c`):
```c
double get_model_param_double(const char *param_name);
int get_model_param_int(const char *param_name);
const char *get_model_param_string(const char *param_name);
```

---

## Validation Workflow

1. **Parameter File Parsed** (`read_parameter_file()`)
   - Loads `model_parameters:` section from YAML
   - Stores as string name-value pairs in `MimicConfig.ModelParams[]`

2. **Module Configuration** (`configure_modules()`)
   - Determines enabled modules from `modules.enabled` list
   - Calls `get_required_params_for_modules()` to find required parameters

3. **Parameter Validation** (before module initialization)
   - Checks all required parameters are present
   - Validates values against metadata ranges using `validate_model_param_*()`
   - ERROR-level failures halt execution with clear messages

4. **Module Access** (during module execution)
   - Modules call `get_model_param_*()` to retrieve validated values
   - Values guaranteed valid by this point

---

## Best Practices

### Parameter Naming
✓ **DO**:
- Use PascalCase: `BaryonFrac`, `SfrEfficiency`
- Be descriptive: `ThreshMajorMerger` (not `TMM`)
- Match scientific literature conventions
- Group related parameters with prefixes: `Feedback*`, `AGN*`

✗ **DON'T**:
- Use abbreviations: `BF`, `SFE` (unclear)
- Use underscores unnecessarily: `baryon_frac` (use PascalCase)
- Use generic names: `Parameter1`, `Value`

### Range Definition
✓ **DO**:
- Use physical constraints: `[0.0, 1.0]` for fractions
- Document why range is chosen in `scientific_notes`
- Use inclusive ranges (both min and max are valid)

✗ **DON'T**:
- Use arbitrarily large ranges: `[0.0, 1e100]` (be specific)
- Leave numeric types without ranges (required!)
- Use ranges that exclude physically valid values

### Documentation
✓ **DO**:
- Provide complete `scientific_notes` with context
- Cite specific papers with journal and year
- Explain parameter's role in physics
- Note interactions with other parameters
- Document typical value ranges from literature

✗ **DON'T**:
- Copy-paste generic descriptions
- Omit scientific context
- Use vague references: "standard model"
- Forget to explain physical meaning

---

## Common Patterns

### Physics Efficiency Parameters
```yaml
- name: SomeEfficiency
  type: double
  range: [0.0, 1.0]        # Efficiency bounded 0-100%
  units: dimensionless
  recommended: 0.1         # Typical ~10%
  scientific_notes: |
    Controls efficiency of [physical process].
    Higher values = more efficient [outcome].
    Typical range: 0.05 - 0.2
```

### Mode Selectors
```yaml
- name: SomeRecipeOn
  type: int
  range: [0, 3]           # 4 modes: 0,1,2,3
  units: dimensionless
  description: Mode selector (0=off, 1=mode1, 2=mode2, 3=mode3)
  recommended: 1
  scientific_notes: |
    Mode 0: Disabled
    Mode 1: [prescription 1]
    Mode 2: [prescription 2]
    Mode 3: [prescription 3]
```

### File Paths
```yaml
- name: SomeDirectory
  type: string
  range: null             # No numeric range for strings
  units: path
  description: Directory containing [data files]
  recommended: "input/DataDir"
  scientific_notes: |
    Path to directory with required data files.
    Must contain: [list of required files].
```

---

## Troubleshooting

### "Parameter X not found in metadata"
**Cause**: Typo in parameter name, or parameter not defined in `model_parameters.yaml`
**Fix**: Check spelling, ensure parameter is defined, run `make generate`

### "Parameter X out of valid range"
**Cause**: Value in input file exceeds range specified in metadata
**Fix**: Check `range:` in `model_parameters.yaml`, adjust input value or range

### "Required parameter X missing"
**Cause**: Parameter needed by enabled module not specified in input file
**Fix**: Add parameter to `model_parameters:` section of input YAML

### "Too many model parameters"
**Cause**: More than 64 parameters specified (array size limit)
**Fix**: Increase array size in `src/include/types.h` (currently 64, supports up to 64 parameters)

---

## See Also

- **[Property Metadata Schema](property-metadata-schema.md)** - Schema for galaxy/halo properties
- **[Module Metadata Schema](../developer/module-metadata-schema.md)** - Schema for physics modules
- **[Module Configuration Guide](../user/module-configuration.md)** - User guide for configuring parameters
- **[Vision Document](vision.md)** - Architectural principles (especially #3, #4, #8)

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2025-11-28 | Initial specification |

---

**Document Status**: ✅ Complete and current
**Maintenance**: Update when parameter schema changes or new features added
