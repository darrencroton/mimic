# Module Metadata Schema Specification

**Version**: 1.0
**Created**: 2025-11-12
**Status**: Implementation Specification
**Purpose**: Define the authoritative schema for module metadata in Mimic

---

## Quick Start

**Creating a new module?** Here's the minimum you need to know:

1. **Copy the template**: `cp -r src/modules/_template src/modules/your_module`
2. **Edit `module_info.yaml`** with your module's details (name, parameters, dependencies)
3. **Required fields**: `name`, `display_name`, `description`, `sources`, `register_function`
4. **Run**: `make generate-modules` (auto-generates registration code)
5. **Build**: `make clean && make`

**Minimal example:**
```yaml
name: my_module
display_name: "My Module"
description: "Does physics stuff"
version: "1.0.0"
category: physics

sources: [my_module.c]
headers: [my_module.h]
register_function: my_module_register

dependencies:
  requires: []  # List modules this depends on
  provides: []  # List properties this module creates

parameters: []  # Add your module's parameters here
tests:
  unit: test_unit_my_module.c
  integration: test_integration_my_module.py
```

**Most common fields:**
- `name` - Internal identifier (matches directory name)
- `sources` / `headers` - Your C files
- `register_function` - Name of your `*_register()` function
- `dependencies.requires` - Modules you need data from
- `dependencies.provides` - Properties you create
- `parameters` - Runtime configuration options

**Full schema details below.** This is a 1200+ line reference - use Ctrl+F to find what you need.

---

## Overview

This document defines the YAML schema for physics module metadata in Mimic. Module metadata is the **single source of truth** for:
- Module registration code generation (`src/modules/module_init.c`)
- Test system integration (`tests/unit/module_sources.mk`)
- Module documentation (`docs/user/module-reference.md`)
- Build-time module selection (Phase 5)
- Dependency resolution and validation

By defining modules once in metadata, we eliminate manual synchronization across 4+ files and reduce error opportunities by 75% when adding new modules.

---

## Architectural Context

### Vision Alignment

This module metadata system directly implements 4 of Mimic's 8 core architectural principles:

**Principle 3: Metadata-Driven Architecture**
- Module registration driven by YAML metadata, not hardcoded implementations

**Principle 4: Single Source of Truth**
- `module_info.yaml` is the authoritative definition, all else is generated

**Principle 5: Unified Processing Model**
- Automatic dependency resolution from declared requirements

**Principle 8: Type Safety and Validation**
- Build-time validation of module consistency and completeness

### Module System Architecture

```
┌─────────────────────────────────────────────┐
│  module_info.yaml (SINGLE SOURCE OF TRUTH)  │
│  - Module metadata                          │
│  - Dependencies                             │
│  - Parameters                               │
│  - Testing specs                            │
└─────────────────────────────────────────────┘
                     │
                     │ (generation script)
                     ▼
        ┌────────────┴────────────┐
        │                         │
        ▼                         ▼
┌──────────────────┐   ┌──────────────────┐
│ module_init.c    │   │ module_sources.mk│
│ (registration)   │   │ (test builds)    │
└──────────────────┘   └──────────────────┘
        │                         │
        ▼                         ▼
┌──────────────────┐   ┌──────────────────┐
│ module-reference │   │ validation       │
│ .md (docs)       │   │ (CI checks)      │
└──────────────────┘   └──────────────────┘
```

---

## Metadata File Organization

### Location

Each physics module has its own metadata file:

```
src/modules/
├── sage_infall/
│   ├── sage_infall.c
│   ├── sage_infall.h
│   └── module_info.yaml        ← Module metadata
├── simple_cooling/
│   ├── simple_cooling.c
│   ├── simple_cooling.h
│   └── module_info.yaml        ← Module metadata
└── _template/
    ├── template_module.c
    ├── template_module.h
    └── module_info.yaml.template  ← Template for new modules
```

### Rationale

- Co-locates metadata with implementation (easy to find and update)
- Each module is self-contained and independent
- Template provides starting point for new modules
- Generator script discovers modules by scanning for `module_info.yaml` files

---

## Schema Definition

### Top-Level Structure

```yaml
# src/modules/module_name/module_info.yaml
module:
  # Core Metadata (required)
  name: string
  display_name: string
  description: string
  version: string
  author: string
  category: string

  # Source Files (required)
  sources: [string, ...]
  headers: [string, ...]
  register_function: string

  # Dependencies (required)
  dependencies:
    requires: [string, ...]
    provides: [string, ...]

  # Parameters (required, can be empty)
  parameters: [param_def, ...]

  # Testing (optional, but recommended)
  tests:
    unit: string
    integration: string
    scientific: string

  # Documentation (optional, but recommended)
  docs:
    physics: string
    user_guide_section: string

  # References (optional)
  references: [string, ...]

  # Build Configuration (optional)
  compilation_requires: [string, ...]
  default_enabled: boolean
```

---

## Required Fields

### Core Metadata

#### name (string, required)

**Purpose**: Unique module identifier used in code and configuration

**Rules**:
- Valid C identifier (alphanumeric + underscore, no spaces)
- Must be unique across all modules
- Convention: lowercase_with_underscores
- Used in: `EnabledModules` parameter, logging, registration

**Examples**:
```yaml
name: sage_infall
name: simple_cooling
name: starformation_feedback
```

**Validation**: Must match directory name (e.g., `sage_infall` module in `src/modules/sage_infall/`)

#### display_name (string, required)

**Purpose**: Human-readable name for documentation and UI

**Rules**:
- Can contain spaces, capitals, punctuation
- Used in documentation and logging
- Should be concise (2-5 words)

**Examples**:
```yaml
display_name: "SAGE Infall"
display_name: "Simple Cooling"
display_name: "Star Formation & Feedback"
```

#### description (string, required)

**Purpose**: One-sentence summary of module's physics

**Rules**:
- Complete sentence with period
- Describes what physics the module implements
- 10-30 words
- Used in generated documentation

**Examples**:
```yaml
description: "Cosmological gas infall and satellite stripping from SAGE model."
description: "Simple cooling model with baryon fraction-based gas accretion."
description: "Kennicutt-Schmidt star formation with supernova feedback."
```

#### version (string, required)

**Purpose**: Module version for tracking changes and compatibility

**Rules**:
- Semantic versioning: `MAJOR.MINOR.PATCH`
- Start at `1.0.0` for production modules
- Use `0.1.0` for PoC/experimental modules
- Increment on breaking changes, features, or bug fixes

**Examples**:
```yaml
version: "1.0.0"  # Production SAGE infall
version: "0.1.0"  # PoC simple cooling
version: "2.1.3"  # Mature module with fixes
```

#### author (string, required)

**Purpose**: Attribution and contact for module questions

**Rules**:
- Name or team identifier
- Can include "(ported from X)"
- Used in documentation

**Examples**:
```yaml
author: "Mimic Team (ported from SAGE)"
author: "D. Croton"
author: "Mimic Development Team"
```

#### category (string, required)

**Purpose**: Organize modules by physics type for documentation

**Rules**:
- Must be one of predefined categories (validated)
- Used to organize module documentation
- Helps users find related modules

**Valid Categories**:
- `gas_physics` - Cooling, heating, infall
- `star_formation` - SF recipes and feedback
- `stellar_evolution` - Stellar mass, recycling
- `black_holes` - AGN, quasar mode
- `mergers` - Galaxy mergers, morphology
- `environment` - Ram pressure, tides
- `reionization` - UV background, suppression
- `miscellaneous` - Utilities, tracking

**Examples**:
```yaml
category: gas_physics
category: star_formation
category: mergers
```

---

### Source Files

#### sources (list of strings, required)

**Purpose**: List of C source files for this module

**Rules**:
- Paths relative to module directory
- Must exist (validated)
- Typically just one file: `module_name.c`
- Multiple files allowed for complex modules

**Examples**:
```yaml
sources:
  - sage_infall.c

sources:
  - cooling_main.c
  - cooling_tables.c
```

**Validation**: All listed files must exist in module directory

#### headers (list of strings, required)

**Purpose**: List of C header files for this module

**Rules**:
- Paths relative to module directory
- Must exist (validated)
- Typically just one file: `module_name.h`
- Must declare the register function

**Examples**:
```yaml
headers:
  - sage_infall.h

headers:
  - cooling.h
  - cooling_tables.h
```

**Validation**: All listed files must exist in module directory

#### register_function (string, required)

**Purpose**: Name of the function that registers this module

**Rules**:
- Valid C identifier
- Convention: `module_name_register`
- Must be declared in header
- Must be defined in source
- Function signature: `void register_function(void)`

**Examples**:
```yaml
register_function: sage_infall_register
register_function: simple_cooling_register
```

**Validation**:
- Must exist in at least one source file
- Must follow naming convention: `{module_name}_register`
- Function must call `module_registry_add()`

---

### Dependencies

#### dependencies.requires (list of strings, required, can be empty)

**Purpose**: Galaxy properties that this module reads/requires

**Rules**:
- List of property names (must match `galaxy_properties.yaml`)
- Empty list `[]` if module doesn't require properties
- Used for dependency resolution and documentation
- Modules are ordered so dependencies are satisfied

**Examples**:
```yaml
dependencies:
  requires: []  # No dependencies (e.g., sage_infall)

dependencies:
  requires:
    - HotGas       # simple_cooling needs HotGas
    - MetalsHotGas

dependencies:
  requires:
    - ColdGas      # simple_sfr needs ColdGas
```

**Validation**:
- All required properties must be provided by some other module OR created by this module
- Circular dependencies are detected and reported

#### dependencies.provides (list of strings, required, can be empty)

**Purpose**: Galaxy properties that this module creates/modifies

**Rules**:
- List of property names (must match `galaxy_properties.yaml`)
- Empty list `[]` if module doesn't create properties
- Used for dependency resolution and documentation
- Properties should match those defined in galaxy_properties.yaml

**Examples**:
```yaml
dependencies:
  provides:
    - HotGas
    - MetalsHotGas
    - EjectedMass
    - MetalsEjectedMass
    - ICS
    - MetalsICS

dependencies:
  provides:
    - ColdGas      # simple_cooling creates ColdGas

dependencies:
  provides:
    - StellarMass  # simple_sfr creates StellarMass
```

**Validation**:
- All provided properties should exist in `galaxy_properties.yaml`
- Warning if property listed but not in property metadata
- Used to construct dependency graph

---

### Parameters

#### parameters (list of parameter definitions, required, can be empty)

**Purpose**: Define module parameters with defaults and validation

**Rules**:
- List of parameter definitions (see Parameter Structure below)
- Empty list `[]` if module has no parameters
- Parameters are read via `module_get_*()` functions
- Naming convention: `{ModuleName}_{ParameterName}` in `.par` file

**Parameter Structure**:

```yaml
parameters:
  - name: string              # Parameter name (in code)
    type: string              # Data type: double, int, string
    default: value            # Default value (type-appropriate)
    range: [min, max]         # Optional: valid range for numeric types
    description: string       # Human-readable description
    units: string             # Optional: physical units
```

**Example** (full module with parameters):

```yaml
parameters:
  - name: BaryonFrac
    type: double
    default: 0.17
    range: [0.0, 1.0]
    description: "Cosmic baryon fraction (Omega_b / Omega_m)"
    units: "dimensionless"

  - name: ReionizationOn
    type: int
    default: 1
    range: [0, 1]
    description: "Enable reionization suppression (0=off, 1=on)"
    units: "dimensionless"

  - name: Reionization_z0
    type: double
    default: 8.0
    range: [0.0, 20.0]
    description: "Redshift when UV background turns on"
    units: "dimensionless"

  - name: CoolFunctionsDir
    type: string
    default: "input/CoolFunctions"
    description: "Directory containing cooling function tables"
```

**Parameter Types**:
- `double` - Floating-point (use `module_get_double()`)
- `int` - Integer (use `module_get_int()`)
- `string` - Text (use `module_get_parameter()`)

**Validation**:
- Parameter names must follow convention
- Default values must match type
- Range must be valid for numeric types (min ≤ max)
- Generated documentation includes all parameters

**Usage in .par file**:
```
# Module parameters follow ModuleName_ParameterName convention
SageInfall_BaryonFrac  0.17
SageInfall_ReionizationOn  1
SageInfall_Reionization_z0  8.0
```

---

### Testing (optional but strongly recommended)

#### tests.unit (string, optional)

**Purpose**: Path to unit test file for this module

**Rules**:
- Relative to `tests/unit/` directory
- Should test pure physics functions in isolation
- Examples: table interpolation, parameter parsing, calculations

**Example**:
```yaml
tests:
  unit: test_unit_sage_infall.c
```

**Generated**: Automatically included in test build system

#### tests.integration (string, optional)

**Purpose**: Path to integration test file for this module

**Rules**:
- Co-located with module in `src/modules/MODULE_NAME/` directory
- Should test module in full pipeline context
- Examples: module loads, properties appear in output, parameter configuration
- Naming convention: `test_integration_MODULE_NAME.py`

**Example**:
```yaml
tests:
  integration: test_integration_sage_infall.py
```

#### tests.scientific (string, optional)

**Purpose**: Path to scientific validation test file

**Rules**:
- Co-located with module in `src/modules/MODULE_NAME/` directory
- Should validate physics against reference results
- Examples: SAGE comparison, published results, mass conservation
- Naming convention: `test_scientific_MODULE_NAME*.py`

**Example**:
```yaml
tests:
  scientific: test_scientific_sage_infall_validation.py
```

---

### Documentation (optional but strongly recommended)

#### docs.physics (string, optional)

**Purpose**: Path to physics documentation for this module

**Rules**:
- Relative to repository root
- Should explain physics equations, assumptions, references
- Markdown format recommended

**Example**:
```yaml
docs:
  physics: src/modules/sage_infall/README.md
```

**Generated**: Linked in module reference documentation

#### docs.user_guide_section (string, optional)

**Purpose**: Section title for user guide documentation

**Rules**:
- Human-readable section title
- Used in auto-generated documentation
- Should match display_name for consistency

**Example**:
```yaml
docs:
  user_guide_section: "SAGE Infall Module"
```

---

### References (optional but recommended)

#### references (list of strings, optional)

**Purpose**: Citation information for module physics

**Rules**:
- List of paper citations or source references
- Free-form text (citation style not enforced)
- Used in generated documentation

**Example**:
```yaml
references:
  - "Gnedin (2000) - Reionization model"
  - "Kravtsov et al. (2004) - Filtering mass formulas"
  - "Croton et al. (2016) - SAGE model description"
  - "SAGE source: sage-code/model_infall.c"
```

---

### Build Configuration (optional)

#### compilation_requires (list of strings, optional)

**Purpose**: External dependencies required to compile this module

**Rules**:
- List of feature flags: `HDF5`, `MPI`, `GSL`, etc.
- Empty list or omitted if no special dependencies
- Used for conditional compilation (Phase 5)

**Example**:
```yaml
compilation_requires:
  - HDF5  # Module needs HDF5 library

compilation_requires: []  # No special requirements
```

**Future Use**: Build system can exclude modules if dependencies unavailable

#### default_enabled (boolean, optional, default: false)

**Purpose**: Whether module should be in default `EnabledModules` list

**Rules**:
- `true` - Include in default configuration
- `false` - User must explicitly enable
- Optional modules should be false

**Example**:
```yaml
default_enabled: true   # Core production module
default_enabled: false  # Optional experimental module
```

---

## Complete Examples

### Example: SAGE Infall Module (Complete)

```yaml
# src/modules/sage_infall/module_info.yaml
module:
  # Core Metadata
  name: sage_infall
  display_name: "SAGE Infall"
  description: "Cosmological gas infall and satellite stripping from SAGE model."
  version: "1.0.0"
  author: "Mimic Team (ported from SAGE)"
  category: gas_physics

  # Source Files
  sources: [sage_infall.c]
  headers: [sage_infall.h]
  register_function: sage_infall_register

  # Dependencies
  dependencies:
    requires: []  # Creates initial gas
    provides: [HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS]

  # Parameters
  parameters:
    - name: BaryonFrac
      type: double
      default: 0.17
      range: [0.0, 1.0]
      description: "Cosmic baryon fraction (Omega_b / Omega_m)"
      units: "dimensionless"

    - name: ReionizationOn
      type: int
      default: 1
      range: [0, 1]
      description: "Enable reionization suppression of infall (0=off, 1=on)"
      units: "dimensionless"

    - name: ReionizationModifier
      type: double
      default: 1.0
      range: [0.0, 10.0]
      description: "Reionization suppression strength multiplier"
      units: "dimensionless"

  # Testing
  tests:
    unit: test_unit_sage_infall.c
    integration: test_integration_sage_infall.py
    scientific: test_scientific_sage_infall_validation.py

  # Documentation
  documentation:
    reference_papers:
      - "Croton et al. 2006, MNRAS, 365, 11"
      - "Guo et al. 2011, MNRAS, 413, 101"
    file: src/modules/sage_infall/README.md
```

**Additional examples** available in existing modules:
- `src/modules/sage_cooling/module_info.yaml` - Physics module with multiple parameters
- `src/modules/test_fixture/module_info.yaml` - Minimal testing module
- `src/modules/_template/module_info.yaml.template` - Template for new modules

**For field-by-field reference**, see [Schema Definition](#schema-definition) section above.

## Generated Code Structure

### 1. Module Registration (src/modules/module_init.c)

```c
/* AUTO-GENERATED FILE - DO NOT EDIT MANUALLY */
/* Generated from module metadata by scripts/generate_module_registry.py */
/* Source: src/modules/*/module_info.yaml */
/*
 * To regenerate:
 *   make generate-modules
 *
 * To validate:
 *   make validate-modules
 */

#include "module_registry.h"

/* Auto-generated module includes (sorted alphabetically) */
#include "sage_infall/sage_infall.h"
#include "simple_cooling/simple_cooling.h"
#include "simple_sfr/simple_sfr.h"

/**
 * @brief Register all available physics modules
 *
 * Modules registered: 3
 *
 * Dependency order:
 * 1. sage_infall: provides HotGas, MetalsHotGas, ...
 * 2. simple_cooling: requires [] → provides ColdGas
 * 3. simple_sfr: requires ColdGas → provides StellarMass
 */
void register_all_modules(void) {
    /* Register in dependency-resolved order */
    sage_infall_register();      /* Provides: HotGas, MetalsHotGas, EjectedMass, ... */
    simple_cooling_register();   /* Requires: [] → Provides: ColdGas */
    simple_sfr_register();       /* Requires: ColdGas → Provides: StellarMass */
}
```

### 2. Test Module Sources (tests/unit/module_sources.mk)

```makefile
# AUTO-GENERATED FILE - DO NOT EDIT MANUALLY
# Generated from module metadata by scripts/generate_module_registry.py

# Module source files for unit testing
MODULE_SRCS = \
    $(SRC_DIR)/core/module_registry.c \
    $(SRC_DIR)/modules/sage_infall/sage_infall.c \
    $(SRC_DIR)/modules/simple_cooling/simple_cooling.c \
    $(SRC_DIR)/modules/simple_sfr/simple_sfr.c \
    $(SRC_DIR)/modules/module_init.c
```

### 3. Module Reference Documentation (docs/user/module-reference.md)

```markdown
# Module Reference

Auto-generated from module metadata. Last updated: 2025-11-12

---

## Available Modules (3 modules)

### Gas Physics (2 modules)

#### sage_infall - SAGE Infall

Cosmological gas infall and satellite stripping from SAGE model.

**Version**: 1.0.0
**Author**: Mimic Team (ported from SAGE)

**Dependencies**:
- Requires: (none)
- Provides: HotGas, MetalsHotGas, EjectedMass, MetalsEjectedMass, ICS, MetalsICS, TotalSatelliteBaryons

**Parameters**:
- `SageInfall_BaryonFrac` (double, default: 0.17, range: [0.0, 1.0])
  Cosmic baryon fraction (Omega_b / Omega_m)

- `SageInfall_ReionizationOn` (int, default: 1, range: [0, 1])
  Enable reionization suppression (0=off, 1=on)

[... more parameters ...]

**References**:
- Gnedin (2000) - Reionization model
- Kravtsov et al. (2004) - Filtering mass formulas
- Croton et al. (2016) - SAGE model description

**Physics Documentation**: [src/modules/sage_infall/README.md](../../src/modules/sage_infall/README.md)

---

#### simple_cooling - Simple Cooling

[... similar format ...]

---

### Star Formation (1 module)

#### simple_sfr - Simple Star Formation

[... similar format ...]
```

### 4. Validation Hash (build/module_registry_hash.txt)

```
# AUTO-GENERATED - DO NOT EDIT
# Hash of all module metadata files for validation
# Generated: 2025-11-12 10:30:15

MODULE_HASH=7a8f3b2c1d9e4f6a8b7c2d1e3f4a5b6c
```

---

## Validation Rules

The validation script (`scripts/validate_modules.py`) performs comprehensive checks:

### 1. Schema Validation
- All required fields present
- Field types correct (string, list, dict)
- Enum values valid (category, parameter types)
- Version follows semantic versioning

### 2. File Existence Checks
- All source files exist in module directory
- All header files exist in module directory
- Test files exist in appropriate directories
- Documentation files exist

### 3. Naming Convention Checks
- Module name matches directory name
- Module name is valid C identifier
- Register function follows convention: `{name}_register`
- Parameter names don't violate conventions

### 4. Code Verification
- Register function exists in source files
- Register function has correct signature
- Module struct properly defined

### 5. Dependency Validation
- No circular dependencies
- All required properties exist (check against property metadata)
- All provided properties exist (check against property metadata)
- Dependency graph is valid (topological sort succeeds)

### 6. Parameter Validation
- Default values match declared types
- Ranges valid for numeric types (min ≤ max)
- Parameter naming follows `ModuleName_ParameterName` format
- No duplicate parameter names within module

### 7. Consistency Checks
- Version format valid
- Category is recognized
- No duplicate module names

---

## Validation Exit Codes

The validator uses specific exit codes for different error types:

- `0` - Success (all validations passed)
- `1` - Schema error (missing fields, wrong types)
- `2` - File not found (source, header, test, doc)
- `3` - Dependency error (circular, unresolved)
- `4` - Naming convention violation
- `5` - Parameter validation error
- `6` - Code verification error (register function not found)

---

## Development Workflow

### Adding a New Module

**Step 1**: Copy template
```bash
cp -r src/modules/_template src/modules/my_new_module
```

**Step 2**: Create `module_info.yaml`
```bash
cd src/modules/my_new_module
cp module_info.yaml.template module_info.yaml
# Edit module_info.yaml with your module details
```

**Step 3**: Implement module
- Edit `my_new_module.c` and `.h`
- Implement init, process, cleanup functions
- Define module struct and register function

**Step 4**: Validate and generate
```bash
make validate-modules  # Check for errors
make generate-modules  # Generate registration code
```

**Step 5**: Build and test
```bash
make                   # Compile
make test-unit         # Run unit tests
make test-integration  # Run integration tests
```

That's it! No manual synchronization required.

### Updating an Existing Module

**Step 1**: Update `module_info.yaml`
- Change parameters, dependencies, documentation, etc.

**Step 2**: Regenerate
```bash
make generate-modules
```

**Step 3**: Rebuild
```bash
make clean && make
```

All generated files automatically updated.

---

## Migration Strategy

### Migrating Existing Modules

For existing modules without `module_info.yaml`:

**Step 1**: Create metadata file
```yaml
# src/modules/existing_module/module_info.yaml
module:
  name: existing_module
  display_name: "Existing Module"
  # ... fill in all fields based on current implementation
```

**Step 2**: Validate
```bash
python3 scripts/validate_modules.py src/modules/existing_module/module_info.yaml
```

**Step 3**: Fix any validation errors
- Add missing test files
- Fix naming conventions
- Resolve dependency issues

**Step 4**: Regenerate all
```bash
make generate-modules
```

**Step 5**: Verify
```bash
make clean && make && make tests
```

---

## Best Practices

### Documentation
- Write clear, complete descriptions
- Include all relevant paper citations
- Document parameter units explicitly
- Link to physics documentation

### Dependencies
- Be explicit about property requirements
- Only list properties you actually use
- Document why dependencies exist
- Keep dependency chains short when possible

### Parameters
- Provide reasonable defaults
- Use appropriate ranges for validation
- Include units in description
- Follow naming convention strictly

### Testing
- Always provide test files
- Test at all three tiers (unit, integration, scientific)
- Validate against reference results
- Document expected test behavior

### Organization
- Group related parameters together
- Use clear, consistent naming
- Comment complex parameter interactions
- Keep metadata file readable (use YAML formatting)

### Versioning
- Start at 1.0.0 for production modules
- Use 0.x.y for experimental modules
- Increment MAJOR for breaking changes
- Increment MINOR for new features
- Increment PATCH for bug fixes

---

## Common Mistakes to Avoid

### ❌ Wrong: Hardcoded registration
```c
// In module_init.c - DON'T DO THIS
void register_all_modules(void) {
    my_new_module_register();  // Manual addition
}
```

### ✅ Right: Metadata-driven registration
```yaml
# In module_info.yaml - DO THIS
module:
  name: my_new_module
  register_function: my_new_module_register
```
Then run `make generate-modules`.

### ❌ Wrong: Inconsistent naming
```yaml
module:
  name: sage_infall
  register_function: sage_infall_init  # Wrong!
```

### ✅ Right: Follow convention
```yaml
module:
  name: sage_infall
  register_function: sage_infall_register  # Correct!
```

### ❌ Wrong: Missing dependencies
```yaml
dependencies:
  requires: []  # But module actually uses HotGas!
  provides:
    - ColdGas
```

### ✅ Right: Complete dependencies
```yaml
dependencies:
  requires:
    - HotGas      # Explicitly list all requirements
    - MetalsHotGas
  provides:
    - ColdGas
```

### ❌ Wrong: Parameter naming
```yaml
parameters:
  - name: baryon_frac  # Wrong case!
```

### ✅ Right: Follow convention
```yaml
parameters:
  - name: BaryonFrac   # Correct: PascalCase for readability
```

---

## Version History

- **v1.0** (2025-11-12): Initial specification for Phase 4.2.5 infrastructure implementation

---

**This specification is authoritative for module metadata in Mimic. All module definitions must conform to this schema.**
