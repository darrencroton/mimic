# Mimic Coding Standards

## Philosophy

Mimic code should be **professional, clean, and minimal**. Documentation should be concise while capturing essential information. Avoid verbosity - every comment should add value.

**Key Principles**:
- Brief descriptions, not verbose explanations
- Remove redundant information
- Comment only non-obvious logic
- Single source of truth (no duplicating YAML info in headers)
- Let code speak where self-evident

---

## File Documentation

### Header Files (.h)

Header files should have a concise header with:
1. Brief 1-2 sentence description of the module/component
2. Key physics equation or principle (for physics modules)
3. Reference citation

**Good Example (Physics Module)**:
```c
/**
 * @file    sage_calculate_infall.h
 * @brief   SAGE infall module interface
 *
 * Implements cosmological gas infall onto central galaxies from the SAGE model.
 * Central galaxies accrete baryonic gas proportional to halo growth, modified by
 * reionization suppression (set by sage_reionization module).
 *
 * Physics: InfallingGas = HaloBaryonFraction × Mvir - total_baryons
 *
 * Reference: Croton et al. (2006, 2016)
 */

#ifndef SAGE_CALCULATE_INFALL_H
#define SAGE_CALCULATE_INFALL_H

void sage_calculate_infall_register(void);

#endif
```

**Bad Example** (too verbose):
```c
/**
 * @file    sage_calculate_infall.h
 * @brief   SAGE infall module interface
 *
 * This module implements the cosmological gas infall process from the SAGE
 * (Semi-Analytic Galaxy Evolution) model. It handles the infall of baryonic
 * gas onto central galaxies, which is a fundamental process in galaxy formation.
 *
 * The module computes infall as follows:
 * - Calculate expected baryonic mass based on halo virial mass
 * - Account for existing baryonic matter in the galaxy
 * - Apply reionization suppression for low-mass halos
 * - Add the infalling gas to the hot halo reservoir
 *
 * Key Features:
 * - Handles only central galaxies (Type 0)
 * - Uses HaloBaryonFraction property for baryon content
 * - Integrates with sage_reionization for suppression
 * - Updates HotGas and MetalsHotGas properties
 *
 * Dependencies:
 * - Properties: HotGas, MetalsHotGas, HaloBaryonFraction, InfallingSuppression
 * - Parameters: None
 *
 * Vision Principles:
 * - Physics-Agnostic Core (Principle #1)
 * - Runtime Modularity (Principle #2)
 *
 * Physics: InfallingGas = HaloBaryonFraction × Mvir - total_baryons
 *
 * Reference: Croton et al. (2006, 2016)
 */
```

### Implementation Files (.c)

Implementation files should follow the same pattern as headers:

**Good Example**:
```c
/**
 * @file    sage_calculate_infall.c
 * @brief   SAGE infall module implementation
 *
 * Implements cosmological gas infall onto central galaxies. Baryonic gas accretes
 * proportional to halo growth, modified by reionization suppression.
 *
 * Physics: InfallingGas = HaloBaryonFraction × Mvir - total_baryons
 *
 * Key functions:
 * - sage_calculate_infall_init(): Initialize module
 * - sage_calculate_infall_process(): Compute infall for central galaxies
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <stdio.h>
#include <stdlib.h>
// ... rest of file
```

---

## Function Documentation

### Minimal but Complete

Function headers should be concise. Include only essential information.

**Good Example**:
```c
/**
 * @brief   Initialize infall module
 *
 * @return  0 on success, non-zero on failure
 */
static int sage_calculate_infall_init(void) {
  INFO_LOG("Infall module initialized");
  return 0;
}
```

**Good Example (with parameters)**:
```c
/**
 * @brief   Process halos in a FOF group
 *
 * Compute gas infall for central galaxies.
 *
 * @param   ctx     Module execution context (redshift, time, params)
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
static int sage_calculate_infall_process(struct ModuleContext *ctx,
                                struct Halo *halos, int ngal) {
  // implementation
}
```

**Bad Example** (too verbose):
```c
/**
 * @brief   Process halos in a FOF group
 *
 * Called for each FOF group during tree processing. This is where galaxy
 * physics is computed. Responsibilities:
 * - Iterate through halos in FOF group
 * - Access galaxy properties via halos[i].galaxy->PropertyName
 * - Compute physics updates using helper functions
 * - Modify galaxy properties
 * - Handle all halo types appropriately
 * - Use context for redshift-dependent physics
 *
 * @param   ctx     Module execution context (redshift, time, params)
 * @param   halos   Array of halos in the FOF group (FoFWorkspace)
 * @param   ngal    Number of halos in the array
 * @return  0 on success, non-zero on failure
 */
```

### Template Functions

For template/instructional code, maintain balance between brevity and helpfulness:

```c
/**
 * @brief   [Brief description of physics calculation]
 *
 * @param   input1  [Description]
 * @param   input2  [Description]
 * @return  [Description]
 */
static float compute_physics(float input1, double input2) {
  // TODO: Implement physics calculation
  float result = example_param1 * input1 * input2;
  return result;
}
```

---

## Inline Comments

### Use Sparingly

Only comment non-obvious logic. Don't state the obvious.

**Good** (comments add value):
```c
// Filter by halo type if needed (Type 0=central, 1=satellite, 2=orphan)
if (halos[i].Type != 0) {
  continue;
}

// Read properties (halo properties are read-only)
float mvir = halos[i].Mvir;
```

**Bad** (obvious comments):
```c
// Loop through all halos in the FOF group
for (int i = 0; i < ngal; i++) {

  // Check if this is a central galaxy
  if (halos[i].Type != 0) {
    continue;  // Skip this halo if not central
  }

  // Get the virial mass from the halo structure
  float mvir = halos[i].Mvir;
}
```

### Section Headers

Use lightweight section dividers:

**Good**:
```c
// ============================================================================
// MODEL PARAMETERS
// ============================================================================

// Parameters read from YAML via model_get_*() functions
static double baryon_frac;
```

**Bad**:
```c
/* ============================================================================
 * MODEL PARAMETERS
 * ============================================================================
 * Model parameters read from input YAML file via model_get_*() functions
 *
 * Example: If your module uses BaryonFrac and SfrEfficiency:
 *   static double baryon_frac;     // Read via model_get_double("BaryonFrac", ...)
 *   static double sfr_efficiency;  // Read via model_get_double("SfrEfficiency", ...)
 */
```

---

## Module Metadata (module_info.yaml)

### Minimal YAML Structure

Module metadata should be bare YAML with no comments or section dividers.

**Good Example**:
```yaml
module:
  name: sage_calculate_infall
  display_name: "SAGE Infall"
  description: "Cosmological gas infall onto central galaxies from SAGE model"
  version: "1.0.0"
  author: "Mimic Team (ported from SAGE)"

  sources:
    - sage_calculate_infall.c

  headers:
    - sage_calculate_infall.h

  register_function: sage_calculate_infall_register

  dependencies:
    properties:
      - HotGas
      - MetalsHotGas
    parameters: []

  tests:
    unit: test_unit_sage_calculate_infall.c
    integration: test_integration_sage_calculate_infall.py
    scientific: test_scientific_sage_calculate_infall_validation.py

  docs:
    physics: src/modules/sage_calculate_infall/README.md

  compilation_requires: []
```

**Bad Example** (verbose with comments):
```yaml
# ===========================================================================
# Core Metadata (REQUIRED)
# ===========================================================================

module:
  name: sage_calculate_infall  # REQUIRED: lowercase_with_underscores
  display_name: "SAGE Infall"  # REQUIRED: Human-readable name
  description: "Cosmological gas infall onto central galaxies from SAGE model"
  version: "1.0.0"  # Semantic versioning: MAJOR.MINOR.PATCH
  author: "Mimic Team (ported from SAGE)"

  # ===========================================================================
  # Source Files (REQUIRED)
  # ===========================================================================

  sources:
    - sage_calculate_infall.c  # Module implementation

  headers:
    - sage_calculate_infall.h  # Module interface

  register_function: sage_calculate_infall_register  # Must be {module_name}_register

  # ===========================================================================
  # Dependencies (REQUIRED)
  # ===========================================================================

  dependencies:
    # All halo and galaxy properties read or written by this module
    properties:
      - HotGas
      - MetalsHotGas

    # All model parameters accessed via model_get_*() functions
    parameters: []

  # ... etc
```

### Template Exception

For `template_module_info.yaml`, include a minimal instructional comment:

```yaml
# Template module metadata - copy to your module directory as module_info.yaml and customize
module:
  name: template_module
  # ... rest of template
```

---

## Code Organization

### Module Structure

Physics modules should follow this structure:

1. File header (concise)
2. Includes
3. Model parameters section
4. Physics constants section (if needed)
5. Module state section (if needed)
6. Helper functions
7. Module lifecycle functions (init, process, cleanup)
8. Module registration

**Example Organization**:
```c
/**
 * @file    my_module.c
 * @brief   Brief module description
 *
 * Physics: Key equation
 *
 * Reference: Citation
 */

#include <math.h>
#include "my_module.h"

// ============================================================================
// MODEL PARAMETERS
// ============================================================================

static double my_param;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static float compute_physics(float x) {
  return x * my_param;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

static int my_module_init(void) {
  // Load parameters
  LOAD_PARAM_DOUBLE("MyParam", my_param);
  INFO_LOG("Module initialized with MyParam = %.3f", my_param);
  return 0;
}

static int my_module_process(struct ModuleContext *ctx,
                              struct Halo *halos, int ngal) {
  // Process halos
  return 0;
}

static int my_module_cleanup(void) {
  INFO_LOG("Module cleaned up");
  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

extern const enum ProcessingMode my_module_supported_modes[];

static struct Module my_module = {
    .name = "my_module",
    .init = my_module_init,
    .process = my_module_process,
    .cleanup = my_module_cleanup,
    .supported_processing_modes = my_module_supported_modes,
    .num_supported_modes = 1
};

void my_module_register(void) {
  module_registry_add(&my_module);
}
```

---

## Logging Best Practices

### Use Appropriate Logging Macros

Mimic provides a hierarchy of logging macros for different message types:

- **DEBUG_LOG()**: Detailed debugging information (loop iterations, variable values)
- **INFO_LOG()**: General informational messages (processing milestones)
- **VERBOSE_LOG()**: Configuration and initialization details (only shown with --verbose or --debug)
- **WARNING_LOG()**: Warnings that don't stop execution
- **ERROR_LOG()**: Recoverable errors
- **FATAL_ERROR()**: Unrecoverable errors (terminates program)

### When to Use VERBOSE_LOG()

Use `VERBOSE_LOG()` for messages that should only appear in verbose mode:
- Module initialization configuration parameters
- Detailed setup information
- Non-critical status messages that clutter default output

**Example**:
```c
static int my_module_init(void) {
  /* Load parameters */
  LOAD_PARAM_DOUBLE("MyParam", my_param);

  /* Log configuration only in verbose mode */
  VERBOSE_LOG("My Module initialized");
  VERBOSE_LOG("  MyParam = %.3f", my_param);
  VERBOSE_LOG("  MyOption = %s", my_option ? "enabled" : "disabled");

  return 0;
}
```

### Logging Modes

Mimic supports different logging modes via command-line flags:
- Default: INFO, WARNING, ERROR, FATAL (no DEBUG or VERBOSE)
- `--verbose` / `-v`: Enables VERBOSE_LOG messages + adds context (timestamp, file:line)
- `--debug` / `-d`: Enables DEBUG_LOG + VERBOSE_LOG + context (most verbose)
- `--quiet` / `-q`: Only WARNING, ERROR, FATAL (least verbose)

### Don't Use Conditional Logging

**Bad** (verbose, repetitive):
```c
if (get_verbose_format()) {
  INFO_LOG("Module initialized");
  INFO_LOG("  Param1 = %.3f", param1);
}
```

**Good** (clean, simple):
```c
VERBOSE_LOG("Module initialized");
VERBOSE_LOG("  Param1 = %.3f", param1);
```

The `VERBOSE_LOG()` macro handles the verbose check internally, following DRY and KISS principles.

---

## README Files

### Physics Module READMEs

Physics module README files should be lightweight, focusing on:
- Brief physics overview
- Key equations
- References
- Implementation notes

Keep them concise (typically 50-150 lines).

### Instruction-Focused READMEs

Instruction-focused READMEs (in `_system/`, `_shared/`, etc.) can be more detailed since they're teaching documents. They should still be:
- Well-organized with clear sections
- Focused on essential information
- Free of redundancy
- Fit for their specific purpose

---

## Variable Naming and Documentation

### Naming Conventions

- Use descriptive names: `baryon_frac` not `bf`
- Follow existing patterns: `snake_case` for C
- Units in comments for physical quantities: `// Msun/h`
- Prefix shared utilities with `mimic_`

### Documentation

```c
// Good: units and brief explanation
static double baryon_frac;        // Cosmic baryon fraction (dimensionless)
static double hubble_h;           // Hubble parameter h (H0 = 100h km/s/Mpc)

// Bad: verbose or missing units
static double baryon_frac;        // The fraction of baryons relative to total matter
static double hubble_h;           // Some parameter
```

---

## Documentation Maintenance

**Critical**:
- Update documentation when code changes
- Keep headers synchronized with implementation
- Remove outdated comments
- Don't let comments drift from reality

**Single Source of Truth**:
- Don't duplicate YAML info in headers
- Don't duplicate header info in implementation
- Reference, don't repeat

---

## Before/After Examples

### File Header Transformation

**Before** (verbose):
```c
/**
 * @file    sage_reincorporation.h
 * @brief   SAGE gas reincorporation module interface
 *
 * This module implements the reincorporation of ejected gas back into the hot
 * halo reservoir. This is a key component of the baryon cycle in the SAGE model.
 *
 * Physical Process:
 * Ejected gas (from supernova feedback) is gradually reincorporated back into
 * the hot halo gas reservoir. More massive halos with higher circular velocities
 * can recapture their ejected gas more efficiently.
 *
 * Key Features:
 * - Only operates on central galaxies (Type 0)
 * - Reincorporation rate depends on halo mass (Vvir relative to Vcrit)
 * - Critical velocity Vcrit = 445.48 km/s × ReIncorporationFactor
 * - Mass flow: EjectedMass → HotGas (with metals)
 *
 * Physics: dM_reinc/dt = (Vvir/Vcrit - 1) × M_ejected / t_dyn
 *          where Vcrit = 445.48 km/s × ReIncorporationFactor
 *
 * Dependencies:
 * - Properties: EjectedMass, MetalsEjectedMass, HotGas, MetalsHotGas
 * - Parameters: ReIncorporationFactor
 *
 * Reference: Croton et al. (2016), Guo et al. (2011),
 *            based on SAGE model_reincorporation.c
 */
```

**After** (concise):
```c
/**
 * @file    sage_reincorporation.h
 * @brief   SAGE gas reincorporation module interface
 *
 * Implements return of ejected gas (from supernova feedback) back to hot halo reservoir.
 * More massive halos (Vvir > Vcrit) recapture ejected gas more efficiently.
 *
 * Physics: dM_reinc/dt = (Vvir/Vcrit - 1) × M_ejected / t_dyn
 *          Vcrit = 445.48 km/s × ReIncorporationFactor
 *
 * Mass flow: EjectedMass → HotGas (with metals)
 *
 * Reference: Croton et al. (2016), Guo et al. (2011), based on SAGE model_reincorporation.c
 */
```

### Function Header Transformation

**Before**:
```c
/**
 * @brief   Process halos in a FOF group
 *
 * Called for each FOF group during tree processing. Computes reincorporation
 * of ejected gas back into hot halo for all central galaxies in the group.
 *
 * Algorithm:
 * 1. Iterate through all halos in FOF group
 * 2. Filter for central galaxies (Type 0)
 * 3. Check if halo is above critical mass threshold
 * 4. Compute reincorporation rate based on velocity and timescale
 * 5. Transfer mass from ejected reservoir to hot gas
 * 6. Transfer metals proportionally
 *
 * @param   ctx     Module execution context containing redshift, time, cosmology
 * @param   halos   Array of halos in the FOF group to process
 * @param   ngal    Number of halos in the array
 * @return  0 on success, non-zero on failure
 */
```

**After**:
```c
/**
 * @brief   Process halos in a FOF group
 *
 * Compute gas reincorporation for central galaxies.
 *
 * @param   ctx     Module execution context (redshift, time, params)
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
```

---

## Code Quality Principles

### Alignment with VISION.md

**All code must embody Mimic's 8 architectural principles** (see `docs/VISION.md` for complete details):

1. **Physics-Agnostic Core** - Core code has zero knowledge of galaxy physics
2. **Runtime Modularity** - Modules configured via YAML, not compile-time flags
3. **Metadata-Driven Architecture** - Properties defined in YAML, auto-generated to C
4. **Single Source of Truth** - One canonical definition for each concept (DRY)
5. **Unified Processing Model** - Consistent module interface and lifecycle
6. **Memory Efficiency** - Streaming architecture, immediate resource cleanup
7. **Format-Agnostic I/O** - Reader/writer abstraction for binary/HDF5
8. **Type Safety** - Strong typing, compile-time checks

**Common violations to watch for:**
- ❌ Physics calculations in core code (Principle #1)
- ❌ Hardcoded module names (Principle #2)
- ❌ Manually creating structs that should be generated (Principle #3)
- ❌ Duplicating information across files (Principle #4)
- ❌ Custom module interfaces instead of standard lifecycle (Principle #5)
- ❌ Loading entire simulation into memory (Principle #6)
- ❌ Format-specific logic in processing code (Principle #7)
- ❌ Unsafe type casts without justification (Principle #8)

### KISS Principle (Keep It Simple)

**Avoid over-engineering:**
- ❌ Don't add features "in case we need them later"
- ❌ Don't create abstractions for single-use code
- ❌ Don't generalize until you have 3+ use cases
- ✅ Solve the immediate problem simply
- ✅ Refactor when complexity becomes evident

**Signs of over-engineering:**
- Configuration options for unlikely scenarios
- Abstraction layers with only one implementation
- "Future-proof" code that anticipates hypothetical requirements
- Generic utilities used in only one place

### DRY Principle (Don't Repeat Yourself)

**Identify violations:**
- ❌ Copy-pasted code blocks (extract to function)
- ❌ Repeated validation logic (use helper macros)
- ❌ Duplicated constant definitions (use shared header)
- ❌ Same calculation in multiple modules (use `_shared/` utility)
- ✅ One canonical implementation, referenced everywhere

**Exceptions where duplication is acceptable:**
- Independent tests (test isolation is valuable)
- Similar but semantically different physics (intentional divergence)

---

## Error Handling

### When to Check for Errors

**Always check:**
- ✅ File I/O operations (fopen, fread, fwrite)
- ✅ Memory allocations (MALLOC, REALLOC)
- ✅ Parameter validation (ranges, required values)
- ✅ External library calls (HDF5, MPI)

**Don't check:**
- ❌ Internal function calls with guaranteed contracts
- ❌ Impossible conditions (defensive programming overkill)

### How to Handle Errors

**Use appropriate severity:**
```c
// Recoverable errors: Warn and continue
if (value < 0.0) {
  WARNING_LOG("Negative mass detected (%.2e), clamping to zero", value);
  value = 0.0;
}

// Fatal errors: Cannot continue
FILE *fp = fopen(filename, "r");
if (!fp) {
  FATAL_ERROR("Failed to open input file: %s", filename);
}
```

**Fail fast:**
- ✅ Validate parameters in `init()` functions
- ✅ Check preconditions early in functions
- ❌ Don't silently ignore errors and continue with invalid state

---

## Performance and Efficiency

### Memory Management

**Follow Mimic's memory system:**
```c
// Use categorized allocators for leak detection
float *data = MALLOC(n * sizeof(float), MEM_HALOS);

// Free immediately when done
FREE(data);

// Check for leaks during development
print_allocated_by_category();
```

**Avoid memory waste:**
- ❌ Don't allocate large temporary buffers repeatedly in loops
- ✅ Reuse buffers or allocate once outside loop
- ❌ Don't keep unused allocations around
- ✅ Free resources as soon as they're no longer needed

### Algorithmic Efficiency

**Be aware of complexity:**
```c
// Bad: O(n²) - nested loop over same array
for (int i = 0; i < ngal; i++) {
  for (int j = 0; j < ngal; j++) {
    // ...
  }
}

// Good: O(n) - single pass
for (int i = 0; i < ngal; i++) {
  // ...
}
```

**Optimize hot paths:**
- Profile before optimizing (measure, don't guess)
- Inner loops in `process()` functions are hot paths
- One-time `init()` code doesn't need optimization

---

## Code Complexity

### Function Length

**Guidelines:**
- ✅ Aim for <50 lines per function
- ✅ One clear purpose per function
- ⚠️ If >100 lines, consider refactoring
- ❌ If >200 lines, definitely refactor

**When to extract helper functions:**
- Repeated code blocks (DRY)
- Complex calculations that can be named
- Deeply nested logic (>3 levels)

### Cyclomatic Complexity

**Avoid deeply nested conditionals:**
```c
// Bad: deeply nested
if (condition1) {
  if (condition2) {
    if (condition3) {
      // deeply nested logic
    }
  }
}

// Good: early returns
if (!condition1) return;
if (!condition2) return;
if (!condition3) return;
// main logic at low nesting level
```

**Use early returns to reduce nesting:**
- ✅ Guard clauses at function start
- ✅ Fail fast, then proceed with main logic
- ❌ Don't nest 4+ levels deep

---

## Magic Numbers and Constants

### Use Named Constants

**Bad:**
```c
float critical_velocity = 445.48 * reinc_factor;  // What is 445.48?
if (mass > 1e10) {  // Why 1e10?
  // ...
}
```

**Good:**
```c
// Define physics constants with units and reference
static const double VCRIT_BASE = 445.48;  /* km/s - Croton et al. 2016 */
static const double MIN_MASS_THRESHOLD = 1e10;  /* Msun/h */

float critical_velocity = VCRIT_BASE * reinc_factor;
if (mass > MIN_MASS_THRESHOLD) {
  // ...
}
```

### Where to Define Constants

**Universal constants** → `_system/physical_constants.h`:
- Fundamental constants (G, c, h, k_B)
- Solar values (M_sun, L_sun, Z_sun)

**Module-specific calibrations** → Module file:
- Empirical coefficients specific to one physics prescription
- Module-local thresholds

**Model parameters** → Input YAML file:
- User-configurable values
- Efficiency factors, recipe switches

---

## Testing Standards

### Test Quality Indicators

**Good tests:**
- ✅ Test one thing clearly (focused)
- ✅ Descriptive assertion messages
- ✅ Independent (no interdependencies)
- ✅ Fast (<1s for unit tests)
- ✅ Deterministic (same input → same output)

**Bad tests:**
- ❌ Test multiple unrelated things
- ❌ Vague failures ("Test failed")
- ❌ Depend on execution order
- ❌ Slow (>10s for unit tests)
- ❌ Flaky (intermittent failures)

### Test Coverage

**What to test:**
- ✅ Module lifecycle (init, process, cleanup)
- ✅ Parameter validation (ranges, required values)
- ✅ Edge cases (zero, negative, very large values)
- ✅ Physics correctness (conservation laws, expected trends)
- ✅ Memory safety (no leaks, proper cleanup)

**What not to test:**
- ❌ Implementation details (test behavior, not internals)
- ❌ Third-party libraries (trust them)
- ❌ Obvious getters/setters (low value)

---

## Code Review Checklist

When reviewing code, check for:

### Architecture
- [ ] Follows appropriate VISION principle (physics-agnostic core, etc.)
- [ ] Module belongs in correct directory (`src/core/` vs `src/modules/`)
- [ ] No hardcoded module names in core code

### Quality
- [ ] Follows KISS (simple, not over-engineered)
- [ ] Follows DRY (no duplication, single source of truth)
- [ ] Clear, self-documenting code
- [ ] Appropriate use of comments (non-obvious only)

### Documentation
- [ ] Concise file headers (1-2 sentences + key equation)
- [ ] Minimal function headers (essential info only)
- [ ] No verbose explanations or obvious comments
- [ ] YAML metadata is bare (no comments)

### Error Handling
- [ ] Critical operations checked (I/O, allocation, validation)
- [ ] Appropriate error severity (WARNING vs FATAL)
- [ ] Fail fast (validate early)

### Performance
- [ ] No obvious inefficiencies (O(n²) where O(n) possible)
- [ ] Memory allocated once, not repeatedly in loops
- [ ] Resources freed when no longer needed

### Code Quality
- [ ] Functions <200 lines (ideally <100)
- [ ] Nesting <4 levels deep
- [ ] Named constants instead of magic numbers
- [ ] Descriptive variable names

### Testing
- [ ] Tests are focused and independent
- [ ] Good assertion messages
- [ ] Edge cases covered
- [ ] No memory leaks

---

## Summary

**Key Standards**:
1. **Brief descriptions** - 1-2 sentences, not paragraphs
2. **Essential information only** - Remove verbosity
3. **Minimal YAML** - No comments or section dividers
4. **Comment non-obvious code** - Not obvious operations
5. **Consistent structure** - Follow established patterns
6. **Single source of truth** - Don't duplicate information
7. **Let code speak** - Don't over-comment self-evident code

**Apply these standards to**:
- All new code
- Code being modified
- Template files
- Module metadata

**Result**: Clean, professional, maintainable codebase that's easy to read and understand.
