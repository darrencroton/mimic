# Module Developer Guide

**Version**: 1.0 (Phase 4.1)
**Audience**: Developers implementing galaxy physics modules
**Prerequisites**: Familiarity with C programming, understanding of Mimic architecture (`docs/architecture/vision.md`)

---

## Quick Reference

**Creating a module in 5 steps:**

1. **Copy template**: `cp -r src/modules/_system/template src/modules/my_module`
2. **Implement 3 functions**: `init()`, `process_halos()`, `cleanup()`
3. **Define metadata**: Edit `module_info.yaml` (name, parameters, dependencies)
4. **Auto-register**: Run `make generate`
5. **Test**: Unit tests + integration tests + scientific validation

**Module interface (must implement):**
- `init()` - Initialize module (load data, allocate memory)
- `process_halos()` - Process each FOF group (main physics)
- `cleanup()` - Free resources and shut down

**Key patterns:**
- Access galaxy data via property accessors: `get_ColdGas(gal)`, `set_StellarMass(gal, value)`
- Read parameters from `MimicConfig`: Named as `ModuleName_ParameterName`
- Add properties in `metadata/galaxy_properties.yaml`, run `make generate`
- Co-locate tests with module code (auto-discovered)

**Most important sections:**
- [Quick Start](#quick-start-creating-your-first-module) - Step-by-step module creation
- [Module Lifecycle](#module-lifecycle) - When each function is called
- [Property System](#property-system-integration) - Working with galaxy data
- [Testing Your Module](#testing-your-module) - Three-tier testing approach

**This is a 1100+ line comprehensive guide. Jump to what you need using the table of contents.**

---

## Table of Contents

1. [Introduction](#introduction)
2. [Module Architecture Overview](#module-architecture-overview)
3. [Quick Start: Creating Your First Module](#quick-start-creating-your-first-module)
4. [Module Lifecycle](#module-lifecycle)
5. [Property System Integration](#property-system-integration)
6. [Parameter Handling](#parameter-handling)
7. [Testing Your Module](#testing-your-module)
8. [Common Patterns and Examples](#common-patterns-and-examples)
9. [Anti-Patterns and Pitfalls](#anti-patterns-and-pitfalls)
10. [Debugging and Diagnostics](#debugging-and-diagnostics)
11. [Performance Considerations](#performance-considerations)
12. [Reference](#reference)

---

## Introduction

Mimic's modular architecture separates core infrastructure from galaxy physics implementations. Physics modules are **pure add-ons** that extend core functionality through well-defined interfaces, enabling:

- **Scientific Flexibility**: Switch physics implementations without recompilation
- **Independent Development**: Work on modules without touching core code
- **Clean Testing**: Test physics logic in isolation
- **Runtime Configuration**: Users enable/disable modules via parameter files

This guide teaches you how to create production-quality galaxy physics modules following Mimic's architectural principles.

### Core Principles (Review)

Before implementing a module, understand these principles from `docs/architecture/vision.md`:

1. **Physics-Agnostic Core**: Core has zero knowledge of specific physics. Modules interact only through defined interfaces.
2. **Runtime Modularity**: Module combinations configurable at runtime, not compile-time.
3. **Metadata-Driven**: Galaxy properties defined in YAML, not hardcoded in C.
4. **Single Source of Truth**: `GalaxyData` is the authoritative galaxy state, accessed via generated property accessors.

---

## Module Architecture Overview

### The Module Interface

All modules implement the `struct Module` interface defined in `src/core/module_interface.h`:

```c
struct Module {
    const char *name;              // Unique module identifier
    int (*init)(void);             // Initialize module (called once)
    int (*process_halos)(...);     // Process FOF group (called per group)
    int (*cleanup)(void);          // Cleanup module (called once)
};
```

### Module Execution Flow

```
Program Startup:
  1. register_all_modules()      → Modules register themselves
  2. module_system_init()        → Calls init() on enabled modules

Tree Processing (per FOF group):
  3. module_execute_pipeline()   → Calls process_halos() on all modules

Program Shutdown:
  4. module_system_cleanup()     → Calls cleanup() in reverse order
```

### Module Context

Modules receive context during execution:

```c
struct ModuleContext {
    double redshift;                    // Current snapshot redshift
    double time;                        // Current cosmic time
    const struct MimicConfig *params;   // Read-only simulation parameters
};
```

---

## Quick Start: Creating Your First Module

**NEW (Phase 4.2.5)**: Module registration is now **automatic** via metadata. No manual code editing required!

### Step 1: Copy the Template

```bash
cp -r src/modules/_system/template src/modules/my_module
```

### Step 2: Rename Files

```bash
cd src/modules/my_module
mv template_module.h my_module.h
mv template_module.c my_module.c
```

### Step 3: Create Module Metadata

Create `src/modules/my_module/module_info.yaml`:

```yaml
module:
  # Core metadata
  name: my_module
  display_name: "My Module"
  description: "Brief description of what this module does."
  version: "1.0.0"
  author: "Your Name"
  category: gas_physics  # or star_formation, mergers, etc.

  # Source files
  sources:
    - my_module.c
  headers:
    - my_module.h
  register_function: my_module_register

  # Dependencies
  dependencies:
    requires: []  # Properties your module needs
    provides: []  # Properties your module creates

  # Parameters
  parameters: []  # Add your parameters here

  # Build configuration
  compilation_requires: []
  default_enabled: true
```

See `docs/developer/module-metadata-schema.md` for complete schema documentation.

### Step 4: Update File Contents

Edit `my_module.h` and `my_module.c`:
- Replace all occurrences of `template_module` with `my_module`
- Replace `TEMPLATE_MODULE` with `MY_MODULE`
- Update file documentation
- Implement your physics logic

### Step 5: Define Properties (if needed)

If your module needs new galaxy properties:

```bash
# Edit property metadata
vim metadata/galaxy_properties.yaml

# Add your property:
# - name: MyProperty
#   type: float
#   units: "1e10 Msun/h"
#   description: "My physics quantity"
#   output: true
#   created_by: my_module
#   init_source: default
#   init_value: 0.0
#   output_source: galaxy_property

# Add to module_info.yaml dependencies.provides:
# provides:
#   - MyProperty

# Generate code
make generate
```

### Step 6: Build and Test

```bash
# Validate metadata
make validate-modules

# Generate registration code (automatic during build, but you can test it)
make generate

# Build
make

# Your module is now automatically registered!
```

**That's it!** No manual editing of `module_init.c` or test scripts required. The build system automatically:
- Generates module registration code
- Updates test build configuration
- Creates module documentation
- Validates dependencies

### Step 6: Build and Test

```bash
make                  # Compile
make test-unit        # Run unit tests
make test-integration # Run integration tests
```

### Step 7: Configure and Run

Add to your `.par` file:

```
EnabledModules  existing_module_a,existing_module_b,my_module
MyModule_SomeParameter  1.5
```

Run:

```bash
./mimic input/millennium.par
```

---

## Module Lifecycle

### 1. Init Phase (`init` function)

**Purpose**: One-time initialization during program startup

**Responsibilities**:
- Read module parameters from configuration
- Allocate persistent memory structures
- Load external data files (cooling tables, yields, etc.)
- Initialize lookup tables or caches
- Log module configuration

**Example**:

```c
static double MY_EFFICIENCY = 0.02;  // Default
static double *cooling_table = NULL;

static int my_module_init(void) {
    // Read parameters
    module_get_double("MyModule", "Efficiency", &MY_EFFICIENCY, 0.02);

    // Allocate persistent memory
    cooling_table = malloc_tracked(1000 * sizeof(double), MEM_UTILITY);
    if (cooling_table == NULL) {
        ERROR_LOG("Failed to allocate cooling table");
        return -1;
    }

    // Load external data
    if (load_cooling_table("data/cooling.dat", cooling_table) != 0) {
        ERROR_LOG("Failed to load cooling table");
        return -1;
    }

    // Log configuration
    INFO_LOG("My module initialized");
    INFO_LOG("  Efficiency = %.3f", MY_EFFICIENCY);
    INFO_LOG("  Cooling table: 1000 entries loaded");

    return 0;
}
```

**Common Patterns**:
- ✅ Use `module_get_double()`, `module_get_int()` for parameters
- ✅ Use `malloc_tracked()` for memory allocation (enables leak detection)
- ✅ Validate all inputs and external data
- ✅ Log configuration details for debugging
- ✅ Return -1 on error, 0 on success

**Anti-Patterns**:
- ❌ Don't modify simulation parameters (`ctx->params` is read-only)
- ❌ Don't allocate per-halo memory here (do it in `process_halos`)
- ❌ Don't hardcode parameter values (read from configuration)

### 2. Process Phase (`process_halos` function)

**Purpose**: Apply physics to a FOF group of halos

**Responsibilities**:
- Iterate through halos in FOF group
- Access galaxy properties via `halos[i].galaxy->PropertyName`
- Compute physics updates
- Modify galaxy properties
- Handle all halo types (central/satellite/orphan)
- Use context for redshift-dependent physics

**Function Signature**:

```c
static int my_module_process(struct ModuleContext *ctx,
                               struct Halo *halos,
                               int ngal)
```

**Example**:

```c
static int my_module_process(struct ModuleContext *ctx,
                               struct Halo *halos,
                               int ngal) {
    // Validate inputs
    if (halos == NULL || ngal <= 0) {
        return 0;  // Nothing to process
    }

    // Access context
    double z = ctx->redshift;
    double hubble_h = ctx->params->Hubble_h;

    // Process each halo
    for (int i = 0; i < ngal; i++) {
        // Only process centrals (common pattern)
        if (halos[i].Type != 0) {
            continue;
        }

        // Validate galaxy data
        if (halos[i].galaxy == NULL) {
            ERROR_LOG("Halo %d (Type=0) has NULL galaxy data", i);
            return -1;
        }

        // Read properties (inputs)
        float cold_gas = halos[i].galaxy->ColdGas;
        float mvir = halos[i].Mvir;

        // Compute physics
        float delta_mass = compute_my_physics(cold_gas, mvir, z);

        // Update properties (outputs)
        halos[i].galaxy->MyProperty += delta_mass;
        halos[i].galaxy->ColdGas -= delta_mass;

        // Debug logging (use DEBUG_LOG for per-halo details)
        DEBUG_LOG("Halo %d: ColdGas=%.3e, ΔMass=%.3e (z=%.3f)",
                  i, cold_gas, delta_mass, z);
    }

    return 0;
}
```

**Common Patterns**:
- ✅ Validate inputs (`halos != NULL`, `ngal > 0`)
- ✅ Check `halos[i].galaxy != NULL` before access
- ✅ Filter by halo type if your physics is type-specific
- ✅ Use context for redshift/time-dependent calculations
- ✅ Use `DEBUG_LOG` for per-halo details (can be disabled at runtime)
- ✅ Return -1 on errors, 0 on success

**Anti-Patterns**:
- ❌ Don't modify halo properties (Mvir, Rvir, etc.) - these are read-only
- ❌ Don't assume execution order relative to other modules
- ❌ Don't allocate memory without freeing it (causes leaks)
- ❌ Don't use hardcoded array sizes (ngal varies)

### 3. Cleanup Phase (`cleanup` function)

**Purpose**: Free resources during program shutdown

**Responsibilities**:
- Free all allocated memory
- Close open files
- Log final statistics (optional)
- Clean up module state

**Example**:

```c
static int my_module_cleanup(void) {
    // Free persistent memory
    if (cooling_table != NULL) {
        free_tracked(cooling_table, MEM_UTILITY);
        cooling_table = NULL;
    }

    // Log final statistics
    INFO_LOG("My module cleaned up");
    INFO_LOG("  Total halos processed: %d", total_halos_processed);

    return 0;
}
```

**Common Patterns**:
- ✅ Free all memory allocated in `init()`
- ✅ Use `free_tracked()` to match `malloc_tracked()`
- ✅ Set pointers to NULL after freeing
- ✅ Check for NULL before freeing (defensive)
- ✅ Log cleanup completion

**Anti-Patterns**:
- ❌ Don't forget to free allocated memory (memory leaks!)
- ❌ Don't free memory you didn't allocate
- ❌ Don't access freed memory

---

## Property System Integration

### Understanding the Property System

Galaxy properties are defined in `metadata/galaxy_properties.yaml` and auto-generated into:
- C struct `GalaxyData` (in `src/include/generated/property_defs.h`)
- Type-safe accessors
- Output formatting code
- Python dtypes for analysis

### Adding a New Property

**Step 1: Define in YAML**

Edit `metadata/galaxy_properties.yaml`:

```yaml
galaxy_properties:
  - name: MyNewProperty
    type: float                    # or int, double
    units: "1e10 Msun/h"           # Physical units
    description: "Brief description of physics"
    output: true                   # Include in output files
    created_by: my_module          # Module that creates this
    init_source: default           # How to initialize
    init_value: 0.0                # Initial value
    output_source: galaxy_property # Where to get for output
    range: [0.0, 10000.0]          # Valid range (optional)
    sentinels: [0.0]               # Legitimate special values (optional)
```

**Step 2: Generate Code**

```bash
make generate
```

This creates type-safe accessors you can use immediately.

**Step 3: Access in Module**

```c
// Read property
float value = halos[i].galaxy->MyNewProperty;

// Write property
halos[i].galaxy->MyNewProperty = new_value;

// Increment property
halos[i].galaxy->MyNewProperty += delta;
```

### Property Naming Conventions

- **PascalCase**: `ColdGas`, `StellarMass`, `BlackHoleMass`
- **Descriptive**: `MetalsCold` not `MC`
- **Consistent Units**: Document units in YAML metadata

### Property Initialization

Properties are initialized automatically based on `init_source`:
- `default`: Uses `init_value` from metadata
- `inherit`: Copies from progenitor (deep copy in core)

Most properties use `default` with `init_value: 0.0`.

### Property Lifetime

```
Halo Created:
  → Properties initialized (default or inherited)

Module Processing:
  → Modules read/write properties via accessors

Output Written:
  → Properties written to output files

Halo Freed:
  → Memory automatically cleaned up
```

---

## Parameter Handling

### Parameter Naming Convention

Format: `ModuleName_ParameterName`

Examples:
- `SimpleCooling_BaryonFraction`
- `Cooling_MinTemperature`
- `StarFormation_Efficiency`

### Reading Parameters

Use the parameter API in `module_registry.h`:

```c
// Double parameter
double efficiency;
module_get_double("MyModule", "Efficiency", &efficiency, 0.02);  // default=0.02

// Integer parameter
int min_mass;
module_get_int("MyModule", "MinHaloMass", &min_mass, 100);  // default=100

// String parameter
char model[256];
module_get_parameter("MyModule", "Model", model, sizeof(model), "default");
```

### Parameter Validation

Always validate parameter values:

```c
static int my_module_init(void) {
    double efficiency;
    module_get_double("MyModule", "Efficiency", &efficiency, 0.02);

    // Validate range
    if (efficiency < 0.0 || efficiency > 1.0) {
        ERROR_LOG("MyModule_Efficiency = %.3f is outside valid range [0.0, 1.0]",
                  efficiency);
        return -1;
    }

    // Store validated value
    MY_EFFICIENCY = efficiency;

    INFO_LOG("  Efficiency = %.3f (validated)", MY_EFFICIENCY);
    return 0;
}
```

### Parameter Documentation

Document parameters in:
1. **Module code comments**: Explain physics meaning
2. **User documentation**: `docs/user/module-configuration.md`
3. **Physics documentation**: Module's `README.md` in the module directory

---

## Testing Your Module

### Test Tiers

Mimic uses three test levels (see `docs/developer/testing.md` for details). **Tests are co-located with module code** and auto-discovered via the test registry system.

#### 1. Unit Tests (C)

**Purpose**: Test module software quality (lifecycle, memory, integration)

**Location**: `src/modules/my_module/test_my_module.c`

**Example**:

```c
#include "framework/test_framework.h"
#include "../../core/module_registry.h"
#include "my_module.h"

int test_module_initialization(void) {
    reset_config();
    init_memory_system(0);

    strcpy(MimicConfig.EnabledModules[0], "my_module");
    MimicConfig.NumEnabledModules = 1;

    int result = module_system_init();
    TEST_ASSERT(result == 0, "Module initialization should succeed");

    module_system_cleanup();
    check_memory_leaks();
    return TEST_PASS;
}

int main(void) {
    init_test_suite("My Module");

    register_test(test_module_initialization, "test_module_initialization");
    // ... more tests

    return run_all_tests();
}
```

**What to test**:
- Module registration and initialization
- Parameter reading from config
- Memory safety (no leaks during operation)
- Property access patterns
- Error handling

**Declared in**: `module_info.yaml`:
```yaml
tests:
  unit: test_my_module.c
```

#### 2. Integration Tests (Python)

**Purpose**: Test module in full pipeline execution

**Location**: `src/modules/my_module/test_my_module.py`

**Example**:

```python
#!/usr/bin/env python3
import os
import sys
import unittest

# Add tests directory to path for framework import
repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), '../../..'))
sys.path.insert(0, os.path.join(repo_root, 'tests'))
from framework import load_binary_halos

class TestMyModule(unittest.TestCase):
    def test_module_loads(self):
        """Test module executes without errors"""
        returncode, stdout, stderr = self._run_mimic(param_file)
        self.assertEqual(returncode, 0)

    def test_output_properties(self):
        """Test module produces expected output properties"""
        halos = load_binary_halos('output/test_00.dat')
        self.assertIn('MyProperty', halos.dtype.names)
        self.assertGreater(halos['MyProperty'].min(), 0.0)

if __name__ == '__main__':
    unittest.main(verbosity=2)
```

**What to test**:
- Module runs without crashes
- Properties appear in output
- Module respects configuration
- Memory safety (no leaks)
- Multi-module pipeline integration

**Declared in**: `module_info.yaml`:
```yaml
tests:
  unit: test_my_module.c
  integration: test_my_module.py
```

#### 3. Scientific Validation (Python)

**Purpose**: Verify physics correctness (often deferred until dependencies ready)

**Location**: `src/modules/my_module/test_my_module_validation.py`

**Example**:

```python
def test_mass_conservation():
    """Verify my module conserves mass"""
    data = read_binary_halos('output/science_test.dat')

    # Total baryonic mass should be conserved
    total_mass = data['ColdGas'] + data['StellarMass'] + data['MyProperty']
    expected = data['Mvir'] * BARYON_FRACTION

    # Allow 1% tolerance
    np.testing.assert_allclose(total_mass, expected, rtol=0.01)

def test_vs_reference_results():
    """Compare against reference implementation"""
    mimic_data = read_binary_halos('output/my_module_test.dat')
    reference_data = read_reference_output('reference/model.dat')

    # Statistical comparison
    assert_distributions_similar(mimic_data['MyProperty'],
                                reference_data['MyProperty'],
                                tolerance=0.1)
```

**What to test**:
- Mass/energy conservation
- Comparison to reference implementation results
- Published results reproduction
- Physical plausibility checks

**Declared in**: `module_info.yaml`:
```yaml
tests:
  unit: test_my_module.c
  integration: test_my_module.py
  scientific: test_my_module_validation.py
```

#### Deferring Scientific Validation

It's acceptable to defer physics validation tests when module dependencies are incomplete. However, you must still create the test file as a placeholder.

**When to defer:**

1. **Dependencies incomplete**: Module requires downstream modules for validation
   - Example: `infall_module` needs `cooling_module` + `starformation_module` to validate mass flows
2. **End-to-end validation required**: Physics can only be validated in full pipeline
3. **Reference data not yet available**: Reference comparison data requires complete pipeline run

**Requirements for deferral:**

1. ✅ **Create placeholder test file** - Don't skip the file entirely
2. ✅ **Software quality tests complete** - Unit and integration tests must pass
3. ✅ **Document the deferral** - README explains when/how validation will occur
4. ✅ **Clear plan** - State what validation will be done and when

**Placeholder Example:**

```python
#!/usr/bin/env python3
"""
Scientific validation tests for infall_module.

STATUS: DEFERRED

RATIONALE:
Physics validation requires downstream modules (cooling_module,
starformation_module, reincorporation_module) to validate
complete mass flows through the galaxy formation pipeline.

PLAN:
After downstream modules are implemented:
- Compare Mimic vs reference outputs on identical trees
- Validate mass conservation through full pipeline
- Check reionization suppression curves
- Verify statistical galaxy properties (stellar mass functions, etc.)

For now, unit and integration tests verify software quality.
"""

import unittest

class TestInflallModuleValidation(unittest.TestCase):
    """Scientific validation tests - deferred until dependencies complete."""

    def test_deferred_placeholder(self):
        """Placeholder test - validation deferred until dependencies complete"""
        self.skipTest("Physics validation deferred until cooling_module, "
                     "starformation_module, reincorporation_module complete")

if __name__ == '__main__':
    unittest.main(verbosity=2)
```

**Document in README:**

```markdown
## Testing Status

### Software Quality Testing ✅

**Unit Tests**: ✅ 5 tests passing
**Integration Tests**: ✅ 7 tests passing

### Physics Validation ⏸️

**Status**: Deferred

**Rationale**: Physics validation requires comparing complete mass flows
through the full galaxy formation pipeline. With only `infall_module`
implemented, we cannot validate:
- Correct HotGas amounts (requires cooling module to consume it)
- Baryon cycling (requires star formation and feedback)
- Reionization suppression effects (requires seeing impact on galaxy populations)

**Plan**: After downstream modules complete, conduct comprehensive validation:
- Compare Mimic vs reference outputs on identical trees
- Validate mass conservation through full pipeline
- Check reionization suppression curves
```

**What NOT to defer:**
- ❌ Unit tests - Always implement (test software quality)
- ❌ Integration tests - Always implement (test pipeline integration)
- ❌ Simple physics checks - If testable in isolation, test it now

**Reference:** See module-specific test files for production placeholder examples.

### Test Auto-Discovery

Tests are automatically discovered via the test registry system:

1. **Declare tests** in `module_info.yaml`
2. **Generate registry**: `make generate-test-registry` (done automatically by `make tests`)
3. **Tests auto-run**: Test runners read registry and execute all module tests

**Benefits**:
- Tests co-located with module code
- Adding/removing modules automatically updates test suite
- Physics-agnostic core maintained
- No manual test list maintenance

### Running Tests

```bash
# Run all tests (auto-discovers all module tests)
make tests

# Run specific tier (auto-discovers module tests for that tier)
make test-unit
make test-integration
make test-scientific

# Regenerate test registry (done automatically)
make generate-test-registry

# Validate test registry (checks declared tests exist)
make validate-test-registry
```

See `docs/developer/testing.md` for comprehensive testing guide.

---

## Common Patterns and Examples

### Pattern 1: Time Evolution with Timestep

```c
// Get timestep (calculated by core)
float dt = halos[i].dT;

// Validate timestep
if (dt <= 0.0f) {
    DEBUG_LOG("Halo %d: Invalid dT=%.3f, skipping", i, dt);
    continue;
}

// Apply time evolution
float rate = compute_rate(halos[i].galaxy->SomeProperty, ctx->redshift);
float delta = rate * dt;
halos[i].galaxy->SomeProperty += delta;
```

### Pattern 2: Redshift-Dependent Physics

```c
// Get current redshift from context
double z = ctx->redshift;

// Compute redshift-dependent quantity
double hubble_z = ctx->params->Hubble_h * sqrt(ctx->params->Omega * pow(1+z, 3)
                                               + ctx->params->OmegaLambda);

// Use in physics
float cooling_rate = interpolate_cooling_table(temperature, z);
```

### Pattern 3: Module Dependencies

If your module depends on properties from another module:

```c
static int my_module_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
    for (int i = 0; i < ngal; i++) {
        // Check dependency property exists
        float cold_gas = halos[i].galaxy->ColdGas;

        if (cold_gas <= 0.0f) {
            // No gas available - skip this halo
            continue;
        }

        // Use property in physics
        float star_formation = compute_sf(cold_gas);
        halos[i].galaxy->StellarMass += star_formation;
        halos[i].galaxy->ColdGas -= star_formation;
    }
    return 0;
}
```

**Configuration**: Ensure modules run in correct order in `.par` file:
```
EnabledModules  simple_cooling,my_module  # cooling provides ColdGas
```

### Pattern 4: Type-Specific Physics

```c
for (int i = 0; i < ngal; i++) {
    // Handle different halo types differently
    switch (halos[i].Type) {
        case 0:  // Central galaxy
            process_central(ctx, &halos[i]);
            break;
        case 1:  // Satellite galaxy
            process_satellite(ctx, &halos[i]);
            break;
        case 2:  // Orphan galaxy
            process_orphan(ctx, &halos[i]);
            break;
        default:
            ERROR_LOG("Unknown halo type: %d", halos[i].Type);
            return -1;
    }
}
```

### Pattern 5: Lookup Table Interpolation

```c
// In init(): Load table
static double *temperature_table = NULL;
static double *cooling_rate_table = NULL;
static int table_size = 0;

static int my_module_init(void) {
    table_size = 1000;
    temperature_table = malloc_tracked(table_size * sizeof(double), MEM_UTILITY);
    cooling_rate_table = malloc_tracked(table_size * sizeof(double), MEM_UTILITY);

    // Load from file
    load_tables("data/cooling.dat", temperature_table, cooling_rate_table, table_size);
    return 0;
}

// In process(): Interpolate
static float interpolate_cooling(float temperature) {
    // Binary search or linear interpolation
    int i = binary_search(temperature_table, table_size, temperature);

    if (i < 0 || i >= table_size - 1) {
        return 0.0f;  // Out of range
    }

    // Linear interpolation
    float t0 = temperature_table[i];
    float t1 = temperature_table[i+1];
    float c0 = cooling_rate_table[i];
    float c1 = cooling_rate_table[i+1];

    float frac = (temperature - t0) / (t1 - t0);
    return c0 + frac * (c1 - c0);
}
```

### Pattern 6: Multi-File Module Organization

**When to use:** Complex modules with multiple physics components or large data processing routines.

For complex modules, factor physics into multiple files to improve maintainability:

```
src/modules/my_module/
├── my_module.c           # Main module lifecycle (init, process, cleanup)
├── my_module.h           # Public module interface
├── physics_helper.c      # Physics calculations and algorithms
├── physics_helper.h      # Helper function declarations
├── data_loader.c         # Data file loading and interpolation
├── data_loader.h         # Data loader interface
└── data/                 # Module-specific data files
    ├── table1.dat
    ├── table2.dat
    └── README.txt        # Data file documentation
```

**Example from cooling module:**

```c
// cooling_model.h - Main module interface
#ifndef COOLING_MODEL_H
#define COOLING_MODEL_H

#include "types.h"

void cooling_model_register(void);

#endif

// cooling_tables.h - Helper for table operations
#ifndef COOLING_TABLES_H
#define COOLING_TABLES_H

int load_cooling_tables(const char *dir);
double get_cooling_rate(double temp, double metallicity);
void cleanup_cooling_tables(void);

#endif
```

**Benefits:**
- Separates module lifecycle from physics implementation
- Easier to test individual components
- Clearer code organization for complex physics
- Better collaboration (different developers can work on different files)

**When to keep single-file:**
- Simple modules (<500 lines)
- Single physics process
- No complex data loading

**Reference:** See module implementations for production examples with separate helper files.

### Pattern 7: Module-Specific Data Files

**Principle:** Module-specific data files (tables, yields, etc.) should be stored WITH the module, not in global directories.

**✅ CORRECT - Data co-located with module:**
```
src/modules/cooling_model/
├── cooling_model.c
├── cooling_model.h
└── CoolFunctions/              # Data WITH module
    ├── table_mzero.dat
    ├── table_m-30.dat
    └── ABOUT.txt
```

**❌ WRONG - Data in global directory:**
```
input/
└── CoolFunctions/              # Violates module isolation
    └── *.dat

src/modules/cooling_model/
├── cooling_model.c             # Far from its data
└── cooling_model.h
```

**Loading Data from Module Directory:**

```c
// In module_info.yaml, define parameter with default to module directory
parameters:
  - name: CoolFunctionsDir
    type: string
    default: "src/modules/cooling_model/CoolFunctions"
    description: "Directory containing cooling function tables"

// In init(): Load from configurable path (with sensible default)
static char COOL_FUNCTIONS_DIR[512] = "src/modules/cooling_model/CoolFunctions";

static int my_module_init(void) {
    // Read parameter (allows override if needed)
    module_get_parameter("CoolingModel", "CoolFunctionsDir",
                        COOL_FUNCTIONS_DIR, sizeof(COOL_FUNCTIONS_DIR),
                        "src/modules/cooling_model/CoolFunctions");

    // Load tables from module directory
    if (load_cooling_tables(COOL_FUNCTIONS_DIR) != 0) {
        ERROR_LOG("Failed to load cooling tables from %s", COOL_FUNCTIONS_DIR);
        return -1;
    }

    INFO_LOG("Loaded cooling tables from %s", COOL_FUNCTIONS_DIR);
    return 0;
}
```

**Benefits:**
- **Modules are self-contained** - All module files in one directory
- **No global state pollution** - Each module owns its data
- **Easy to archive/remove** - Delete module directory, done
- **Clear ownership** - No ambiguity about which module uses what data
- **Version control** - Data versioned with module code

**When to use global data:**
- Simulation input files (merger trees, cosmology)
- Shared across ALL modules (e.g., cosmological constants)
- User-provided data (not module-specific)

**Reference:** See module implementations for production examples with data files co-located with module code.

### Pattern 8: Using Shared Physics Utilities

**Principle:** When multiple modules need identical physics functions (e.g., metallicity calculations), use shared utilities to avoid code duplication while maintaining a single source of truth.

**Location:** `src/modules/shared/UTILITY_NAME/`

**Using a shared utility:**

```c
// In your module .c file, include with relative path
#include "../shared/metallicity/metallicity.h"

static int my_module_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
    for (int i = 0; i < ngal; i++) {
        // Use shared utility function
        float Z = mimic_get_metallicity(halos[i].galaxy->HotGas,
                                       halos[i].galaxy->MetalsHotGas);

        // Use in your physics
        float cooling_rate = compute_cooling(temperature, Z);
        // ...
    }
    return 0;
}
```

**Benefits:**
- Single source of truth (edit once, all modules update automatically)
- Clear dependencies (relative path shows exactly what you're using)
- Automatic propagation (Make rebuilds dependent modules when utility changes)
- No build system complexity (simple relative includes)

**When to use:**
- Function appears identically in 2+ modules
- Physics calculation is generic (not module-specific)
- Function is simple and focused (good candidate for header-only utility)

**When NOT to use:**
- Module-specific physics variants (keep in module directory)
- Complex state management (consider module-level implementation)
- Only one module uses it (premature abstraction)

**Reference:** See `src/modules/shared/README.md` for creating new shared utilities.

---

## Anti-Patterns and Pitfalls

### ❌ Modifying Read-Only Data

```c
// WRONG: Don't modify halo properties
halos[i].Mvir = new_value;  // ❌ Core manages this
halos[i].Rvir = new_value;  // ❌ Read-only

// CORRECT: Only modify galaxy properties
halos[i].galaxy->ColdGas = new_value;  // ✅
```

### ❌ Hardcoded Parameters

```c
// WRONG: Hardcoded value
static double EFFICIENCY = 0.02;  // ❌ Can't be changed by users

// CORRECT: Read from configuration
static double EFFICIENCY = 0.02;  // Default

static int my_module_init(void) {
    module_get_double("MyModule", "Efficiency", &EFFICIENCY, 0.02);  // ✅
    return 0;
}
```

### ❌ Memory Leaks

```c
// WRONG: Allocate without tracking
double *table = malloc(1000 * sizeof(double));  // ❌ Leak on error paths

// CORRECT: Use tracked allocation
double *table = malloc_tracked(1000 * sizeof(double), MEM_UTILITY);  // ✅
if (table == NULL) {
    ERROR_LOG("Allocation failed");
    return -1;
}

// CORRECT: Free in cleanup
static int my_module_cleanup(void) {
    if (table != NULL) {
        free_tracked(table, MEM_UTILITY);  // ✅
        table = NULL;
    }
    return 0;
}
```

### ❌ Assuming Module Execution Order

```c
// WRONG: Assume cooling ran first
float cold_gas = halos[i].galaxy->ColdGas;  // ❌ Might be uninitialized

// CORRECT: Check and handle missing data
float cold_gas = halos[i].galaxy->ColdGas;
if (cold_gas <= 0.0f) {
    continue;  // ✅ Skip if no gas (cooling might not have run yet)
}
```

### ❌ Modifying Global State Without Synchronization

```c
// WRONG: Shared mutable state (not thread-safe for future MPI)
static int global_counter = 0;  // ❌

static int my_module_process(...) {
    global_counter++;  // ❌ Race condition
}

// CORRECT: Local state or proper synchronization
static int my_module_process(...) {
    int local_counter = 0;  // ✅
    for (...) {
        local_counter++;
    }
}
```

---

## Debugging and Diagnostics

### Logging Levels

Use appropriate logging levels:

```c
INFO_LOG("Module initialized");              // Always shown
DEBUG_LOG("Halo %d: value=%.3e", i, val);   // Only with --verbose
ERROR_LOG("Critical error occurred");        // Always shown, to stderr
```

### Memory Leak Detection

Check for memory leaks after running:

```bash
./mimic input/test.par
# At end of output, check for:
# "Total allocated: 0 bytes" (no leaks)
# or
# "WARNING: Memory leak detected: X bytes"
```

In code, use:

```c
// In cleanup, verify all memory freed
print_allocated_by_category();
```

### Debug Output

Enable debug logging:

```bash
./mimic --verbose input/test.par
```

### Validation Checks

Add assertions during development:

```c
#include <assert.h>

assert(halos[i].galaxy != NULL);
assert(cold_gas >= 0.0f);
assert(dt > 0.0f && dt < 10.0f);  // Sanity check
```

---

## Performance Considerations

### Profiling

Profile your module on full-scale simulations:

```bash
# Run with timing
time ./mimic input/millennium.par

# Check per-module performance in logs
# INFO_LOG in module code can show timing
```

### Optimization Tips

1. **Minimize allocations in hot loops**:
```c
// WRONG: Allocate per halo
for (int i = 0; i < ngal; i++) {
    double *temp = malloc(100 * sizeof(double));  // ❌ Slow
    // ...
    free(temp);
}

// CORRECT: Allocate once
double *temp = malloc(100 * sizeof(double));  // ✅
for (int i = 0; i < ngal; i++) {
    // Use temp
}
free(temp);
```

2. **Cache-friendly access**:
```c
// Access properties in order for cache efficiency
for (int i = 0; i < ngal; i++) {
    float m = halos[i].galaxy->Mass;      // Sequential access
    float c = halos[i].galaxy->ColdGas;   // ✅ Cache-friendly
}
```

3. **Avoid expensive operations**:
```c
// Cache expensive calculations
double hubble_z = compute_hubble(ctx->redshift, ctx->params);  // Once per FOF group
for (int i = 0; i < ngal; i++) {
    // Use cached hubble_z
}
```

---

## Reference

### Key Files

- **Module Interface**: `src/core/module_interface.h`
- **Module Registry**: `src/core/module_registry.h`
- **Example Modules**: See existing module implementations in `src/modules/`
- **Property Metadata**: `metadata/galaxy_properties.yaml`
- **Module Template**: `src/modules/_system/template/`

### Key Functions

```c
// Parameter reading
module_get_double(module_name, param_name, &out_value, default);
module_get_int(module_name, param_name, &out_value, default);
module_get_parameter(module_name, param_name, out_str, max_len, default);

// Memory management
malloc_tracked(size, category);
free_tracked(ptr, category);
print_allocated_by_category();

// Logging
INFO_LOG(format, ...);
DEBUG_LOG(format, ...);
ERROR_LOG(format, ...);
```

### Documentation

- **Architecture**: `docs/architecture/vision.md`
- **Testing**: `docs/developer/testing.md`
- **Coding Standards**: `docs/developer/coding-standards.md`
- **User Guide**: `docs/user/module-configuration.md`

### Getting Help

1. Review example modules: `src/modules/simple_*/`
2. Check implementation log: `docs/architecture/module-implementation-log.md` (lessons from other modules)
3. Read interface documentation: `src/core/module_interface.h`
4. Review SAGE source for physics reference: `sage-code/`

---

## Summary Checklist

When implementing a module, ensure:

- [ ] Module follows naming conventions (`lowercase_with_underscores`)
- [ ] All three lifecycle functions implemented (`init`, `process_halos`, `cleanup`)
- [ ] Properties defined in YAML metadata, not hardcoded
- [ ] Parameters read from configuration, not hardcoded
- [ ] Memory allocations use `malloc_tracked()` / `free_tracked()`
- [ ] Error handling returns -1 on failure, 0 on success
- [ ] Logging uses appropriate levels (INFO/DEBUG/ERROR)
- [ ] Module registered in `src/modules/module_init.c`
- [ ] Unit tests written for pure physics
- [ ] Integration tests verify full pipeline
- [ ] Scientific tests validate physics correctness
- [ ] Documentation updated (user guide, physics docs, implementation log)
- [ ] Code follows coding standards (`docs/developer/coding-standards.md`)
- [ ] No memory leaks (verified with `print_allocated_by_category()`)

---

**Next Steps**: After reading this guide, proceed to `src/modules/_system/template/` to start implementing your module following the patterns documented here.
