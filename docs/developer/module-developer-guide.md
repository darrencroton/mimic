# Module Developer Guide

**Version**: 1.0 (Phase 4.1)
**Audience**: Developers implementing galaxy physics modules
**Prerequisites**: Familiarity with C programming, understanding of Mimic architecture (`docs/architecture/vision.md`)

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
cp -r src/modules/_template src/modules/my_module
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
vim metadata/properties/galaxy_properties.yaml

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
make generate-modules

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
EnabledModules  simple_cooling,simple_sfr,my_module
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

Galaxy properties are defined in `metadata/properties/galaxy_properties.yaml` and auto-generated into:
- C struct `GalaxyData` (in `src/include/generated/galaxy_properties.h`)
- Type-safe accessors
- Output formatting code
- Python dtypes for analysis

### Adding a New Property

**Step 1: Define in YAML**

Edit `metadata/properties/galaxy_properties.yaml`:

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
3. **Physics documentation**: `docs/physics/module-name.md`

---

## Testing Your Module

### Test Tiers

Mimic uses three test levels (see `docs/developer/testing.md` for details):

#### 1. Unit Tests (`tests/unit/`)

**Purpose**: Test pure physics calculations in isolation

**Example**: `tests/unit/test_my_module.c`

```c
#include "test_framework.h"

void test_cooling_rate_calculation() {
    // Test specific physics calculation
    double rate = calculate_cooling_rate(1e6, 0.02);  // T=1e6K, Z=0.02

    ASSERT_FLOAT_EQ(rate, 1.5e-22, 1e-24);  // Expected value ± tolerance
}

int main() {
    RUN_TEST(test_cooling_rate_calculation);
    // ... more tests
    return TEST_REPORT();
}
```

**What to test**:
- Pure physics functions (no halo dependencies)
- Edge cases (zero, negative, very large values)
- Parameter validation
- Lookup table interpolation

#### 2. Integration Tests (`tests/integration/`)

**Purpose**: Test module in full pipeline

**Example**: `tests/integration/test_my_module.py`

```python
import subprocess
import os

def test_my_module_runs():
    """Test module executes without errors"""
    result = subprocess.run(['./mimic', 'input/test.par'],
                          capture_output=True, text=True)
    assert result.returncode == 0, f"Module failed: {result.stderr}"

def test_my_module_output():
    """Test module produces expected output"""
    # Read output file
    data = read_binary_halos('output/test_00.dat')

    # Check property exists and has reasonable values
    assert 'MyProperty' in data.dtype.names
    assert data['MyProperty'].min() >= 0.0
    assert data['MyProperty'].max() < 1e5
```

**What to test**:
- Module runs without crashes
- Properties appear in output
- Module respects configuration
- Memory leaks (check with `print_allocated_by_category()`)

#### 3. Scientific Validation (`tests/scientific/`)

**Purpose**: Verify physics correctness

**Example**: `tests/scientific/test_my_module_physics.py`

```python
def test_mass_conservation():
    """Verify my module conserves mass"""
    data = read_binary_halos('output/science_test.dat')

    # Total baryonic mass should be conserved
    total_mass = data['ColdGas'] + data['StellarMass'] + data['MyProperty']
    expected = data['Mvir'] * BARYON_FRACTION

    # Allow 1% tolerance
    np.testing.assert_allclose(total_mass, expected, rtol=0.01)

def test_vs_sage_results():
    """Compare against SAGE reference"""
    mimic_data = read_binary_halos('output/my_module_test.dat')
    sage_data = read_sage_output('sage_reference/model.dat')

    # Statistical comparison
    assert_distributions_similar(mimic_data['MyProperty'],
                                sage_data['MyProperty'],
                                tolerance=0.1)
```

**What to test**:
- Mass/energy conservation
- Comparison to SAGE results
- Published results reproduction
- Physical plausibility checks

### Running Tests

```bash
# Run all tests
make tests

# Run specific tier
make test-unit
make test-integration
make test-scientific

# Run individual test
cd tests/unit && ./test_my_module.test
cd tests/integration && python test_my_module.py
```

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
- **Example Modules**: `src/modules/simple_cooling/`, `src/modules/simple_sfr/`
- **Property Metadata**: `metadata/properties/galaxy_properties.yaml`
- **Module Template**: `src/modules/_template/`

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

**Next Steps**: After reading this guide, proceed to `src/modules/_template/` to start implementing your module following the patterns documented here.
