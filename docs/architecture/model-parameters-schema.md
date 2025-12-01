# Model Parameters Metadata Schema Specification

**Version**: 2.0
**Updated**: 2025-12-01
**Status**: Implementation Specification
**Purpose**: Define the authoritative schema for decentralized model parameter metadata in Mimic

---

## Quick Start

**Adding a new model parameter? Here's what you need:**

1. **Edit your module's metadata file:**
   - `src/modules/your_module/module_info.yaml`

2. **Minimal parameter definition:**
```yaml
parameter_definitions:
  - name: MyNewParameter
    type: double
    description: My new physics parameter
    units: dimensionless
```

3. **Regenerate code:**
```bash
make generate
```

4. **Use in your module:**
```c
// Parameter already declared in dependencies.parameters
// (automatically added from parameter_definitions)

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
- `type` - Data type (`int`, `double`, `string`)
- `description` - Brief description of parameter
- `units` - Physical units (optional, defaults to "dimensionless")

**Generated outputs:**
- `src/include/generated/model_parameters.h` - Parameter metadata struct, validation functions
- `src/include/generated/model_parameters.c` - Validation implementation (existence checking only)

**Important:** All parameters are **REQUIRED** in input files - no defaults are used. This ensures reproducible science.

**Full schema details below.**

---

## Overview

This document defines the YAML schema for model parameter metadata in Mimic. Model parameter metadata is the **single source of truth** for:
- Parameter structure (name, type, units)
- Validation rules (existence checking - NO range validation)
- Module dependencies (which parameters each module requires)
- Smart parameter validation (only require parameters for enabled modules)
- Scientific documentation (descriptions, units, source module)

**Decentralized Architecture**: Each module defines its own parameters in its `module_info.yaml` file. Multiple modules can define the same parameter (for shared parameters like BaryonFrac) - first module wins, types must match.

---

## Architecture Principles

The model parameter system implements **Vision Principles #3, #4, and #8**:

### Principle #3: Metadata-Driven Architecture
- YAML defines parameter structure
- Code generation creates type-safe validation
- Single source of truth co-located with modules

### Principle #4: Single Source of Truth (Decentralized)
- Each `module_info.yaml` defines parameters in `parameter_definitions:` section
- Shared parameters can be defined by multiple modules (type must match)
- First module to initialize wins (alphabetical order by module directory)
- Input YAML file provides parameter VALUES (user configuration)
- NO defaults, NO range checking - trust the user

### Principle #8: Type Safety and Validation
- Auto-generated validation functions: `validate_model_param_double()`, `validate_model_param_int()`
- Existence checking only (parameter must be defined)
- Type safety through generated accessors
- Clear error messages with parameter name

---

## No Defaults, No Range Checking Philosophy

**IMPORTANT**: Mimic does NOT use default parameter values or range validation.

**Why no defaults?**
- **Reproducible Science**: Parameter files fully specify the model - no hidden assumptions
- **Explicit Models**: Researchers know exactly what physics is configured
- **Comparable Runs**: Different runs can be compared by comparing parameter files

**Why no range checking?**
- **Trust the User**: Researchers know their parameter values
- **In-Code Validation**: Physics code validates values where it matters (with context)
- **Simplified Metadata**: Less metadata to maintain, clearer single-source-of-truth
- **Flexibility**: Exploration beyond "typical" ranges encouraged

**Smart Validation**: Only parameters needed by enabled modules are required. If you disable a module, you don't need to specify its parameters.

---

## File Structure

### Location
```
src/modules/your_module/module_info.yaml
```

Each module defines its own parameters. Shared parameters (like BaryonFrac) can be defined by multiple modules.

### Format
```yaml
# In module_info.yaml
parameter_definitions:
  - name: ParameterName
    type: double|int|string
    description: Brief description
    units: dimensionless|Msun|Mpc|etc  # Optional, defaults to "dimensionless"
```

### Generated Files

| File | Purpose | Content |
|------|---------|---------|
| `src/include/generated/model_parameters.h` | Header | Metadata struct, validation function declarations, constants |
| `src/include/generated/model_parameters.c` | Implementation | Validation functions (existence check only), metadata arrays |

---

## Field Reference

### Required Fields

#### `name` (string, required)
**Purpose**: Parameter identifier used in code and configuration files.

**Requirements**:
- Must be valid C identifier (alphanumeric + underscore)
- No spaces, special characters
- Case-sensitive
- Must be unique across all parameters (if multiple modules define same parameter, type must match)

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
- `string` - String values (file paths, names)

**Mapping to C types**:
```yaml
type: int     →  int
type: double  →  double
type: string  →  char*
```

**Examples**:
```yaml
type: double     # For most physics parameters (0.17, 0.01, etc.)
type: int        # For flags, modes, counts (0, 1, 2)
type: string     # For paths, file names
```

---

#### `description` (string, required)
**Purpose**: Brief, user-facing description of the parameter.

**Requirements**:
- Single line (no newlines)
- Concise (< 100 characters ideal)
- Self-contained (readable without references)
- Explains what parameter controls

**Examples**:
```yaml
description: "Cosmic baryon fraction (Omega_b / Omega_m)"
description: "Star formation efficiency (epsilon_SF in SFR equation)"
description: "Mass ratio threshold for major mergers"
```

---

### Optional Fields

#### `units` (string, optional)
**Purpose**: Document physical units for scientific clarity.

**Default**: `"dimensionless"` if not specified

**Common values**:
- `dimensionless` - Ratios, fractions, pure numbers
- `Msun` - Solar masses
- `Msun/h` - Solar masses with h-scaling
- `Mpc` or `Mpc/h` - Megaparsecs
- `km/s` - Kilometers per second
- `Gyr` - Gigayears
- `code_units` - Internal simulation units
- `path` - File system path

**Examples**:
```yaml
units: dimensionless       # BaryonFrac (Omega_b / Omega_m)
units: code_units          # EnergySNcode (10^51 erg in code units)
units: path                # CoolFunctionsDir (directory path)
```

---

## Complete Example

```yaml
# src/modules/sage_starformation_feedback/module_info.yaml
name: sage_starformation_feedback
version: "1.0"
description: SAGE star formation and feedback physics

parameter_definitions:
  - name: SfrEfficiency
    type: double
    description: "Star formation efficiency (epsilon_SF in SFR = epsilon_SF * M_cold / t_dyn)"
    units: dimensionless

  - name: FeedbackReheatingEpsilon
    type: double
    description: "Mass loading factor for SN feedback reheating"
    units: dimensionless

  - name: FeedbackEjectionEfficiency
    type: double
    description: "Fraction of reheated gas ejected to IGM"
    units: dimensionless

  - name: RecycleFraction
    type: double
    description: "Fraction of stellar mass returned to ISM (IMF-averaged)"
    units: dimensionless

# ... rest of module_info.yaml
```

### Shared Parameter Example

Multiple modules can define the same parameter:

```yaml
# src/modules/sage_infall/module_info.yaml
parameter_definitions:
  - name: BaryonFrac
    type: double
    description: "Cosmic baryon fraction (Omega_b / Omega_m)"
    units: dimensionless
```

```yaml
# src/modules/sage_satellite_stripping/module_info.yaml
parameter_definitions:
  - name: BaryonFrac
    type: double
    description: "Cosmic baryon fraction (Omega_b / Omega_m)"
    units: dimensionless
```

Both modules use BaryonFrac. The first module alphabetically wins (sage_infall), but both definitions must have matching types or code generation fails.

---

## Module Dependencies

Modules automatically declare parameter dependencies when they define parameters in `parameter_definitions:`. The system:
- Scans all `module_info.yaml` files for `parameter_definitions:`
- Collects unique parameters (first-wins, type-must-match)
- Tracks source_module for each parameter
- Uses `dependencies.parameters` for smart validation

The system uses these declarations for **smart validation**:
- Only parameters needed by enabled modules are required
- Validation function: `get_required_params_for_modules()` (auto-generated)
- Physics-free mode (no modules) requires NO parameters

---

## Code Generation

### Generated Structures

**`src/include/generated/model_parameters.h`:**
```c
/* Parameter metadata structure (simplified) */
struct ModelParameterMetadata {
    const char *name;          /* Parameter name */
    const char *type;          /* Type: int, double, string */
    const char *description;   /* Human-readable description */
    const char *units;         /* Physical units */
    const char *source_module; /* Module that defined this parameter */
};

#define NUM_REQUIRED_MODEL_PARAMETERS 22

extern const char *REQUIRED_MODEL_PARAMETERS[NUM_REQUIRED_MODEL_PARAMETERS];
extern const struct ModelParameterMetadata MODEL_PARAMETER_METADATA[NUM_REQUIRED_MODEL_PARAMETERS];
```

### Generated Validation

**`src/include/generated/model_parameters.c`:**
```c
/* Validate parameter exists (NO range checking) */
int validate_model_param_double(const char *param_name, double value) {
    (void)value;  /* Unused - no range validation */

    const struct ModelParameterMetadata *meta = get_model_param_metadata(param_name);
    if (meta == NULL) {
        ERROR_LOG("Model parameter '%s' not found in metadata", param_name);
        return -1;
    }

    return 0;  /* No range validation - trust the user */
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

### Defining Parameters

In `src/modules/your_module/module_info.yaml`:
```yaml
parameter_definitions:
  - name: BaryonFrac
    type: double
    description: "Cosmic baryon fraction (Omega_b / Omega_m)"
    units: dimensionless

  - name: SfrEfficiency
    type: double
    description: "Star formation efficiency"
    units: dimensionless
```

### Accessing Parameters

In module code:
```c
#include "generated/model_parameters.h"
#include "core/module_registry.h"

void your_module_init(void) {
    /* Get parameters (existence-checked by core before module init) */
    double baryon_frac = get_model_param_double("BaryonFrac");
    double sfr_eff = get_model_param_double("SfrEfficiency");
    int sf_mode = get_model_param_int("SFprescription");

    /* Validate values in context (in-code validation) */
    if (baryon_frac <= 0.0 || baryon_frac > 1.0) {
        ERROR_LOG("BaryonFrac = %g is physically unrealistic "
                  "(expected 0.15-0.18 from CMB)", baryon_frac);
        return;
    }

    /* Parameters guaranteed to be:
     * 1. Present (required by this module)
     * 2. Correct type (enforced by accessor functions)
     * 3. Physically reasonable (validated by module code where needed)
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
   - Validates parameters EXIST using `validate_model_param_*()`
   - NO range checking - trust the user
   - ERROR-level failures halt execution with clear messages

4. **Module Access** (during module execution)
   - Modules call `get_model_param_*()` to retrieve validated values
   - Modules perform in-code validation where physics context matters
   - Example: Check BaryonFrac is positive, SfrEfficiency is reasonable, etc.

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

### Parameter Definition
✓ **DO**:
- Define parameters in the module that primarily uses them
- Share common parameters (BaryonFrac, etc.) across modules
- Keep descriptions concise and informative
- Specify units when relevant

✗ **DON'T**:
- Duplicate parameter definitions with different types (will fail validation)
- Define parameters in modules that don't use them
- Omit descriptions or use vague language

### In-Code Validation
✓ **DO**:
- Validate parameter values in physics context
- Provide helpful error messages with expected ranges
- Document why certain values are problematic
- Use soft warnings for unusual (but valid) values

**Example**:
```c
if (sfr_eff < 0.001 || sfr_eff > 0.5) {
    WARN_LOG("SfrEfficiency = %g is outside typical range [0.01, 0.05]. "
             "Proceeding anyway, but results may be unphysical.", sfr_eff);
}
```

✗ **DON'T**:
- Silently clamp values (user won't know)
- Use arbitrary hard limits without explanation
- Fail on unusual but potentially valid values

---

## Common Patterns

### Physics Efficiency Parameters
```yaml
- name: SomeEfficiency
  type: double
  description: "Controls efficiency of [physical process]"
  units: dimensionless
```

In-code validation:
```c
if (efficiency < 0.0 || efficiency > 1.0) {
    ERROR_LOG("Efficiency must be between 0 and 1 (got %g)", efficiency);
    return -1;
}
```

### Mode Selectors
```yaml
- name: SomeRecipeOn
  type: int
  description: "Mode selector (0=off, 1=mode1, 2=mode2, 3=mode3)"
  units: dimensionless
```

In-code validation:
```c
if (recipe_mode < 0 || recipe_mode > 3) {
    ERROR_LOG("Recipe mode %d invalid (valid: 0-3)", recipe_mode);
    return -1;
}
```

### File Paths
```yaml
- name: SomeDirectory
  type: string
  description: "Directory containing [data files]"
  units: path
```

In-code validation:
```c
if (!directory_exists(dir_path)) {
    ERROR_LOG("Directory '%s' not found", dir_path);
    return -1;
}
```

---

## Shared Parameters

These parameters are commonly defined by multiple modules:

| Parameter | Type | Used By | Description |
|-----------|------|---------|-------------|
| `BaryonFrac` | double | infall, satellite_stripping | Cosmic baryon fraction |
| `RecycleFraction` | double | starformation_feedback, mergers | IMF-averaged recycled fraction |
| `Yield` | double | starformation_feedback, mergers | Metal yield per stellar mass |
| `AGNrecipeOn` | int | cooling, mergers | AGN feedback mode |

---

## Troubleshooting

### "Parameter X not found in metadata"
**Cause**: Typo in parameter name, or parameter not defined in any `module_info.yaml`
**Fix**: Check spelling, ensure parameter is defined, run `make generate`

### "Type mismatch for parameter X"
**Cause**: Multiple modules define same parameter with different types
**Fix**: Ensure all modules define parameter with identical type

### "Required parameter X missing"
**Cause**: Parameter needed by enabled module not specified in input file
**Fix**: Add parameter to `model_parameters:` section of input YAML

### Build errors after adding parameters
**Cause**: Forgot to regenerate code
**Fix**: Run `make generate` after editing module_info.yaml

---

## Migration from Centralized Model

**Old architecture** (pre-v2.0):
- Single `src/modules/model_parameters.yaml` file
- Range validation in metadata
- Recommended values, scientific notes, references fields

**New architecture** (v2.0+):
- Decentralized: each module's `module_info.yaml` has `parameter_definitions:`
- NO range validation (trust the user)
- Simplified metadata: name, type, description, units only
- In-code validation where physics context matters

**Migration steps** (for reference):
1. Move parameter definitions from centralized file to respective module_info.yaml files
2. Remove range, recommended, references, scientific_notes fields
3. Add in-code validation for physically meaningful ranges
4. Run `make generate` to regenerate code

---

## See Also

- **[Module Metadata Schema](../developer/module-metadata-schema.md)** - Schema for physics modules (includes parameter_definitions)
- **[Module Configuration Guide](../user/module-configuration.md)** - User guide for configuring parameters
- **[Module Developer Guide](../developer/module-developer-guide.md)** - Guide to creating new modules
- **[Vision Document](vision.md)** - Architectural principles (especially #3, #4, #8)

---

## Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.0 | 2025-11-28 | Initial specification (centralized architecture) |
| 2.0 | 2025-12-01 | Decentralized architecture, removed range validation, simplified metadata |

---

**Document Status**: ✅ Complete and current
**Maintenance**: Update when parameter schema changes or new features added
