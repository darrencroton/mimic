# Module Template

**Purpose**: Complete guide and boilerplate for creating new Mimic physics modules

---

## Quick Start Guide

Mimic supports **two patterns** for creating modules:

### Pattern 1: Standalone Module (Simplest)

**Use when**: Single .c file, no tests or validation needed

```bash
# Just create the .c file in src/modules/
touch src/modules/my_module.c

# Implement the three required functions:
# - my_module_init()
# - my_module_process()
# - my_module_cleanup()

# Add to your input YAML:
modules:
  phase_1:
    - my_module: process_by_galaxy
```

**That's it!** No `module_info.yaml` needed. The system auto-discovers standalone modules.

### Pattern 2: Directory Module (Full-Featured)

**Use when**: Multiple files, tests, documentation, or validation needed

```bash
# Copy template
cp -r src/modules/_system/template src/modules/my_module
cd src/modules/my_module

# Rename files
mv template_module.c my_module.c
mv template_module_info.yaml module_info.yaml

# Edit and implement your physics
```

---

## Step-by-Step: Directory Module

### 1. Copy Template

```bash
cp -r src/modules/_system/template src/modules/my_module
cd src/modules/my_module
```

### 2. Rename Files

```bash
mv template_module.c my_module.c
mv template_module_info.yaml module_info.yaml
rm README.md  # Remove this file, create your own
```

### 3. Update module_info.yaml

**Minimal (required fields only)**:
```yaml
module:
  name: my_module
  supported_processing_modes: [process_by_galaxy]
```

**With validation** (recommended for production):
```yaml
module:
  name: my_module
  description: "Brief description of physics"
  supported_processing_modes: [process_by_galaxy]

  dependencies:
    properties:
      - ColdGas
      - StellarMass
    parameters:
      - MyEfficiency
      - MyParameter
```

**Multi-file module**:
```yaml
module:
  name: my_module
  description: "Brief description"
  supported_processing_modes: [process_by_galaxy]

  # my_module.c is implicit (always auto-included)
  # Only list ADDITIONAL files here:
  additional_files:
    - helper.c
    - lookup_tables.c
    - helper.h

  dependencies:
    properties:
      - ColdGas
    parameters:
      - MyParam
```

**Complete metadata** (production module with tests):
```yaml
module:
  name: my_module
  display_name: "My Physics Module"
  description: "Detailed description of physics implemented"
  version: "1.0.0"
  author: "Your Name"

  supported_processing_modes: [process_by_galaxy]

  additional_files:
    - helper.c
    - helper.h

  dependencies:
    properties:
      - ColdGas
      - StellarMass
    parameters:
      - MyEfficiency

  tests:
    unit: tests/test_unit_my_module.c
    integration: tests/test_integration_my_module.py

  docs:
    physics: README.md
```

### 4. Implement Module (my_module.c)

Your module needs three functions:

```c
#include <stdio.h>
#include <math.h>
#include "module_interface.h"
#include "error.h"
#include "_system/parameter_helpers.h"  // Convenient parameter loading macros

// ============================================================================
// MODEL PARAMETERS
// ============================================================================

static double my_efficiency;
static int my_option;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static float compute_physics(float mass, double dt) {
    return my_efficiency * mass * dt;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int my_module_init(void) {
    /* Load and validate parameters */
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("MyEfficiency", my_efficiency, 0.0, 1.0,
                                      "efficiency must be between 0 and 1");
    LOAD_PARAM_INT("MyOption", my_option);

    if (my_option < 0 || my_option > 2) {
        ERROR_LOG("MyOption = %d out of valid range [0, 2]", my_option);
        return -1;
    }

    VERBOSE_LOG("My Module initialized");
    VERBOSE_LOG("  MyEfficiency = %.3f", my_efficiency);
    VERBOSE_LOG("  MyOption = %d", my_option);
    return 0;
}

int my_module_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
    /* Process each galaxy */
    for (int i = 0; i < ngal; i++) {
        struct GalaxyData *gal = halos[i].galaxy;

        /* Read properties (inputs) */
        float cold_gas = gal->ColdGas;
        float stellar_mass = gal->StellarMass;

        /* Compute physics */
        float result = compute_physics(cold_gas, ctx->dt);

        /* Write properties (outputs) */
        gal->StellarMass += result;
        gal->ColdGas -= result;
    }

    return 0;
}

int my_module_cleanup(void) {
    VERBOSE_LOG("My Module cleaned up");
    return 0;
}
```

**Important**:
- Function names **must** follow pattern: `{module_name}_{init|process|cleanup}`
- Use `LOAD_PARAM_*` macros or `model_get_*()` functions to read parameters
- Parameters must be in input YAML file (no defaults)
- Return 0 on success, non-zero on failure

### 5. Define Properties (if needed)

If your module creates new galaxy properties:

```bash
# Edit property metadata
vim src/modules/model_properties.yaml

# Add your properties:
properties:
  - name: MyNewProperty
    type: float
    units: "1e10 Msun/h"
    description: "Description of what this property represents"
    output: true
    init_source: default
    init_value: 0.0f

# Generate code
make generate
```

### 6. Build and Test

```bash
# Generate module registration
make generate

# Build
make clean && make

# Test
./mimic input/test.yaml

# Run full test suite
make tests
```

### 7. Add to Configuration

Edit your input YAML file:

```yaml
modules:
  # Choose appropriate phase
  pre_timestep:                    # Setup (runs once before substeps)
    - sage_reionization: process_full_halo

  phase_1:                         # Main physics (runs each substep)
    - my_module: process_by_galaxy # Add your module here

  phase_2:                         # Secondary physics (runs each substep)
    - sage_mergers: process_full_halo

  post_timestep: []                # Finalization (runs once after substeps)

  parameters:
    # All parameters used by enabled modules MUST be specified
    MyEfficiency: 0.1
    MyOption: 1
    # ... (all other parameters)
```

---

## Module Structure Reference

### Required Fields

**module_info.yaml**:
```yaml
module:
  name: my_module                           # REQUIRED
  supported_processing_modes: [...]         # REQUIRED
```

**my_module.c**:
- `my_module_init()` - Initialize parameters, validate
- `my_module_process()` - Main physics logic
- `my_module_cleanup()` - Free resources

### Optional Fields

```yaml
module:
  description: "..."                # Human-readable description
  display_name: "..."               # Auto-generated from name if omitted
  version: "1.0.0"                  # Defaults to "1.0.0"
  author: "..."                     # Attribution

  additional_files:                 # For multi-file modules
    - helper.c
    - helper.h

  dependencies:                     # Validation helpers
    properties: [...]               # Properties used (read or written)
    parameters: [...]               # Parameters from input YAML

  tests:                            # Test coverage
    unit: tests/test_unit_my_module.c
    integration: tests/test_integration_my_module.py

  docs:                             # Documentation
    physics: README.md

  compilation_requires: []          # Features needed (hdf5, mpi, gsl)
```

---

## Processing Modes

Choose the appropriate mode for your physics:

### process_by_galaxy

**What**: Core loops over galaxies, module processes one at a time (ngal = 1)

**Use when**:
- Per-galaxy physics
- Time integration with dt
- No cross-galaxy dependencies

**Examples**: Cooling, star formation, feedback

**Code pattern**:
```c
int my_module_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
    // ngal will always be 1
    struct GalaxyData *gal = halos[0].galaxy;

    // Process this one galaxy using ctx->dt
    float delta_mass = my_efficiency * gal->ColdGas * ctx->dt;
    gal->StellarMass += delta_mass;

    return 0;
}
```

### process_full_halo

**What**: Module receives entire galaxy array (ngal ≥ 1)

**Use when**:
- Snapshot-level operations
- Cross-galaxy interactions
- FOF-group calculations

**Examples**: Reionization, infall budget, mergers

**Code pattern**:
```c
int my_module_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
    // Process all galaxies in FOF group
    for (int i = 0; i < ngal; i++) {
        struct GalaxyData *gal = halos[i].galaxy;

        // Can access other galaxies in array
        // for cross-galaxy operations
    }

    return 0;
}
```

---

## Parameter Loading

### Using Helper Macros (Recommended)

```c
#include "_system/parameter_helpers.h"

// Load and validate in one call
LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("MyParam", my_param, 0.0, 1.0, "must be between 0 and 1");
LOAD_AND_VALIDATE_RANGE_INCLUSIVE("MyMass", my_mass, 1e8, 1e12, "must be realistic mass");
LOAD_AND_VALIDATE_POSITIVE("MyRate", my_rate, "must be positive");

// Load without validation
LOAD_PARAM_DOUBLE("MyDouble", my_double);
LOAD_PARAM_INT("MyInt", my_int);
LOAD_PARAM_STRING("MyPath", my_path);
```

### Using Direct Functions

```c
#include "model_parameters.h"

double my_efficiency;
if (model_get_double("MyEfficiency", &my_efficiency) != 0) {
    ERROR_LOG("Failed to read MyEfficiency");
    return -1;
}

// Validate
if (my_efficiency < 0.0 || my_efficiency > 1.0) {
    ERROR_LOG("MyEfficiency = %.3f out of range [0.0, 1.0]", my_efficiency);
    return -1;
}
```

**Important**: All parameters used must be:
1. Listed in `module_info.yaml` under `dependencies.parameters`
2. Specified in input YAML file (no defaults in code)

---

## Best Practices

### Module Independence
- ✅ Modules communicate **only** through property system
- ❌ Never call other module functions directly
- ✅ Declare dependencies in `module_info.yaml`

### Property Access
- ✅ Read inputs: `float mass = gal->StellarMass;`
- ✅ Write outputs: `gal->ColdGas += accreted_mass;`
- ❌ Don't modify read-only halo properties (Mvir, Rvir, etc.)

### Memory Management
```c
#include "memory.h"

// Allocate with category tracking
float *data = mymalloc_cat(size * sizeof(float), MEM_PHYSICS);

// Free in cleanup
myfree(data);
```

### Error Handling
```c
// Errors (always logged)
ERROR_LOG("Critical failure: %s", reason);
return -1;

// Verbose output (only with --verbose or --debug)
VERBOSE_LOG("Module initialized with param = %.3f", param);

// Debug output (only with --debug)
DEBUG_LOG("Detailed debug info: %d", value);
```

### Shared Utilities
- Place reusable physics code in `src/modules/_shared/`
- Header-only utilities for fast compilation
- Include via: `#include "_shared/my_utility.h"`

---

## Examples

### Simple Single-File Module

**src/modules/simple_infall.c**:
```c
#include "module_interface.h"
#include "error.h"
#include "_system/parameter_helpers.h"

static double baryon_fraction;

int simple_infall_init(void) {
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", baryon_fraction, 0.0, 1.0,
                                      "baryon fraction must be physical");
    VERBOSE_LOG("Simple infall initialized: f_b = %.3f", baryon_fraction);
    return 0;
}

int simple_infall_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
    struct GalaxyData *gal = halos[0].galaxy;

    // Simple infall calculation
    float infall = baryon_fraction * halos[0].Mvir;
    gal->ColdGas += infall * ctx->dt;

    return 0;
}

int simple_infall_cleanup(void) {
    return 0;
}
```

No `module_info.yaml` needed! Just add to input YAML:
```yaml
modules:
  phase_1:
    - simple_infall: process_by_galaxy
  parameters:
    GlobalBaryonFraction: 0.17
```

### Full-Featured Production Module

See working examples:
- `src/modules/sage_add_infall/` - Infall with metallicity tracking
- `src/modules/sage_reionization/` - Reionization suppression
- `src/modules/_archive/sage_cooling/` - Multi-file with lookup tables

---

## Next Steps

After creating your module:

1. **Write Tests**: Unit, integration, and scientific validation
2. **Document Physics**: Create comprehensive README.md in your module directory
3. **Run Tests**: `make tests` to verify everything works
4. **Update Documentation**: Add physics description to your README

**Need help?**
- Architecture principles: `docs/VISION.md`
- Complete developer guide: `docs/DEVELOPER-GUIDE.md`
- Module schema reference: `docs/REFERENCE.md`
