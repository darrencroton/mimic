# Mimic Developer Guide

**Complete guide to extending Mimic: architecture, module development, and testing**

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Property System](#property-system)
3. [Module Development](#module-development)
4. [Testing](#testing)
5. [Development Workflow](#development-workflow)
6. [Advanced Topics](#advanced-topics)

---

## Architecture Overview

### Core Principles

Mimic is built on **8 architectural principles** (see [VISION.md](VISION.md) for details):

1. **Physics-Agnostic Core**: Core infrastructure has zero knowledge of specific physics implementations
2. **Runtime Modularity**: Module configuration via YAML files, no recompilation needed
3. **Metadata-Driven**: Properties and modules defined once in YAML, auto-generated into C code
4. **Single Source of Truth**: Galaxy data has one authoritative representation (GalaxyData struct)
5. **Unified Processing Model**: One consistent method for processing merger trees
6. **Memory Efficiency**: Bounded, predictable, safe memory usage
7. **Format-Agnostic I/O**: Multiple input/output formats via unified interfaces
8. **Type Safety**: Compile-time validation with auto-generated type-safe accessors

These principles guide all development decisions.

### System Components

```
┌─────────────────────────────────────────────────────────────┐
│                    Mimic Application                       │
├─────────────────────────────────────────────────────────────┤
│  Configuration System     │  Module System                  │
│  - YAML configuration     │  - Runtime loading              │
│  - Schema validation      │  - Dependency resolution        │
├─────────────────────────────────────────────────────────────┤
│                  Physics-Agnostic Core                     │
│  ┌─────────────────┬─────────────────┬─────────────────┐   │
│  │ Memory Mgmt     │ Property System │ I/O System      │   │
│  │ - Scoped alloc  │ - Type-safe     │ - Format unified│   │
│  │ - Auto cleanup  │ - Generated     │ - Cross-platform│   │
│  └─────────────────┴─────────────────┴─────────────────┘   │
│  ┌─────────────────┬─────────────────┬─────────────────┐   │
│  │ Tree Processing │ Pipeline Exec   │ Test Framework  │   │
│  │ - Unified model │ - Configurable  │ - Multi-level   │   │
│  │ - Inheritance   │ - Module phases │ - Scientific    │   │
│  └─────────────────┴─────────────────┴─────────────────┘   │
├─────────────────────────────────────────────────────────────┤
│                    Physics Modules                         │
│  ┌─────────────────┬─────────────────┬─────────────────┐   │
│  │ SAGE Modules    │ Custom Modules  │ Test Modules    │   │
│  └─────────────────┴─────────────────┴─────────────────┘   │
└─────────────────────────────────────────────────────────────┘
```

**Key subsystems**:

**src/core/**: Physics-agnostic core execution
- `main.c`: Entry point, command-line parsing
- `init.c`: Initialization and cleanup
- `build_model.c`: Halo processing and module execution pipeline
- `read_parameter_file.c`: YAML configuration loading
- `module_registry.c`: Module registration and lifecycle management

**src/io/**: Input/output
- `io/tree/`: Merger tree readers (binary, HDF5 formats)
- `io/output/`: Output writers (binary, HDF5 formats)

**src/modules/**: Physics modules
- `_system/`: Framework infrastructure (templates, test fixtures)
- `_shared/`: Reusable physics utilities (user-modifiable)
- `sage_*/`: SAGE physics implementation
- Each module: `module.c`, `module_info.yaml`

**src/util/**: Utilities
- `memory.c`: Category-tracked memory management
- `error.c`: Logging and error handling
- `numeric.c`: Safe division, numerical utilities

### Data Flow

**Multi-phase pipeline execution**:

1. **Configuration Loading**: YAML file parsed, validated
2. **Module Registration**: Modules auto-register from metadata
3. **Tree Processing**: Core loads merger trees
4. **Multi-Phase Execution**:
   - `pre_timestep`: Setup (runs once)
   - Loop over SubSteps:
     - `phase_1`: Main physics (each substep)
     - `phase_2`: Secondary physics (each substep)
   - `post_timestep`: Finalization (runs once)
5. **Output**: Property-based output adapts to available data

**Three-tier halo architecture**:
- **InputTreeHalos**: Raw merger tree input (immutable)
- **FoFWorkspace**: Temporary processing workspace (dynamic array)
- **ProcessedHalos**: Final processed halos (written to output)

**Module communication**:
- Modules do **NOT** call each other directly
- Communication exclusively through galaxy property system
- Modules declare `requires:` and `provides:` in metadata
- Core calls modules in dependency-resolved order

---

## Property System

### What Are Properties?

Properties are galaxy/halo attributes stored in C structs and defined in YAML metadata.

**Two types**:
- **Halo properties** (`src/core/halo_properties.yaml`): Defined by merger tree (Mvir, Rvir, Vmax, etc.)
- **Galaxy properties** (`src/modules/model_properties.yaml`): Calculated by physics modules (ColdGas, StellarMass, etc.)

### Adding a Property

**Step-by-step**:

1. **Edit metadata YAML**:

For galaxy property, edit `src/modules/model_properties.yaml`:
```yaml
properties:
  - name: NewProperty
    type: float
    units: "1e10 Msun/h"
    description: "Brief description"
    output: true
    default_value: 0.0
```

For halo property, edit `src/core/halo_properties.yaml` (similar structure).

2. **Regenerate code**:
```bash
make generate
```

This auto-generates:
- C struct field in `GalaxyData`
- Type-safe accessor macros
- Output code (binary and HDF5 writers)
- Python dtypes for reading output

3. **Use in module**:
```c
// Access property
float value = gal->NewProperty;

// Modify property
gal->NewProperty = calculated_value;
```

4. **Rebuild**:
```bash
make clean && make
```

**Property metadata reference**: See [REFERENCE.md](REFERENCE.md) for complete schema specification.

---

## Module Development

### Module Patterns

Mimic supports **three complexity tiers** for modules:

**Tier 1 - Standalone** (Simplest, ~80% of modules):
```
src/modules/
  my_module.c              # Just the .c file!
```
- No `module_info.yaml` needed
- Auto-discovered by build system
- Perfect for prototyping and simple physics

**Tier 2 - Minimal Directory** (Standard, ~15% of modules):
```
src/modules/my_module/
  my_module.c
  module_info.yaml         # Minimal metadata
  README.md
```
- Simplified `module_info.yaml` with optional fields
- Tests and validation recommended but optional

**Tier 3 - Full-Featured** (Complex, ~5% of modules):
```
src/modules/my_module/
  my_module.c
  helper.c                 # Multiple source files
  module_info.yaml         # Complete metadata
  README.md
  tests/
    test_unit.c
    test_integration.py
```
- Complete metadata with validation
- Full test coverage
- Production-quality module

### Module Structure (Directory Pattern)

For directory-based modules, the structure is:

**Required**:
- `module_name.c`: Implementation
- `module_info.yaml`: Metadata (optional for standalone)

**Optional**:
- Additional `.c` files (listed in `files:`)
- `README.md`: Physics documentation
- `tests/`: Test files

**Location**: `src/modules/module_name/` OR `src/modules/module_name.c`

### Creating a Module

Mimic supports **two patterns** for creating modules:

#### Pattern 1: Standalone Module (Simplest)

For simple modules, just create a single `.c` file:

```bash
# Create the .c file
touch src/modules/my_module.c

# Implement the three required functions:
# - my_module_init()
# - my_module_process()
# - my_module_cleanup()

# Add to input YAML configuration:
modules:
  phase_1:
    - my_module: process_by_galaxy
```

**That's it!** No `module_info.yaml` needed. The system auto-discovers standalone modules.

**Use standalone modules when:**
- Single .c file
- No tests, docs, or validation needed
- Quick prototyping

#### Pattern 2: Directory Module (Full-featured)

For complex modules, use the traditional directory structure:

```bash
cp -r src/modules/_system/template src/modules/my_module
cd src/modules/my_module
mv template_module.c my_module.c
mv template_module_info.yaml module_info.yaml
```

**Use directory modules when:**
- Multiple source files
- Need tests and documentation
- Want property/parameter validation
- Production-quality module

**2. Implement module** (`my_module.c`):

You only need to implement three functions - registration is automatic.

```c
#include <stdio.h>
#include <math.h>
#include "core/module_interface.h"
#include "core/model_parameters.h"
#include "util/error.h"

// ============================================================================
// MODEL PARAMETERS
// ============================================================================

static double my_efficiency;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static float compute_physics(float input1, float input2) {
  return my_efficiency * input1 * input2;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

static int my_module_init(void) {
  /* Load parameters */
  LOAD_PARAM_DOUBLE("MyEfficiency", my_efficiency);

  /* Validate */
  if (my_efficiency < 0.0 || my_efficiency > 1.0) {
    ERROR_LOG("MyEfficiency = %.3f out of valid range [0.0, 1.0]", my_efficiency);
    return -1;
  }

  VERBOSE_LOG("My Module initialized");
  VERBOSE_LOG("  MyEfficiency = %.3f", my_efficiency);
  return 0;
}

static int my_module_process(struct ModuleContext *ctx,
                              struct Halo *halos, int ngal) {
  /* Process halos */
  for (int i = 0; i < ngal; i++) {
    /* Access galaxy data */
    struct GalaxyData *gal = halos[i].galaxy;

    /* Read properties (inputs) */
    float mass = gal->StellarMass;

    /* Compute physics */
    float result = compute_physics(mass, ctx->dt);

    /* Write properties (outputs) */
    gal->NewProperty = result;
  }

  return 0;
}

static int my_module_cleanup(void) {
  VERBOSE_LOG("My Module cleaned up");
  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS (auto-generated from module_info.yaml)
// ============================================================================
// Registration is automatic - no code needed here.
// The generator creates module struct and registration in module_init.c
```

**IMPORTANT**: Function names must follow the convention `{module_name}_{init|process|cleanup}`.
This is enforced by the code generator.

**3. Create metadata** (`module_info.yaml`) - **Ultra-Simplified Format**:

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

  # my_module.c is implicit (always auto-included)
  # No additional_files needed for single-file module

  dependencies:
    properties:
      - StellarMass
      - NewProperty
    parameters:
      - MyEfficiency

  tests:
    unit: tests/test_unit_my_module.c
```

**Multi-file module**:
```yaml
module:
  name: my_module
  description: "Brief description"
  supported_processing_modes: [process_by_galaxy]

  # my_module.c is implicit (always auto-included)
  additional_files:
    - helper.c
    - helper.h

  dependencies:
    properties:
      - ColdGas
    parameters:
      - MyParam
```

**What's new:**
- ✅ Only `name` and `supported_processing_modes` are required
- ✅ `{name}.c` is always implicit - never declare it
- ✅ `additional_files` for multi-file modules (helper files only)
- ✅ `display_name`, `version`, `author` are optional (auto-generated)
- ✅ `dependencies` optional but recommended for validation

**5. Generate registration code**:
```bash
make generate  # Auto-generates module_init.c with registration
```

This generates all registration code in `module_init.c` from your `module_info.yaml`.

**6. Build and test**:
```bash
make clean && make
./mimic input/test.yaml
```

### Processing Modes

**Two modes available**:

**`process_full_halo`**: Module receives entire galaxy array
- Use for: Operations needing access to all galaxies simultaneously
- Better for: Vectorized operations, snapshot-level calculations
- Example: Reionization, infall budget calculation

**`process_by_galaxy`**: Core loops over galaxies, module processes one at a time
- Use for: Per-galaxy physics, time integration
- Better for: Cache locality, matching SAGE behavior
- Example: Cooling, star formation, feedback

Specify in `module_info.yaml`:
```yaml
processing_modes:
  - process_by_galaxy  # or process_full_halo
```

### Multi-Phase Pipeline

**Choose appropriate phase**:

**pre_timestep**: Setup calculations (runs once before substeps)
- Examples: Reionization, infall budget, snapshot-level setup
- No time integration

**phase_1**: Main baryonic physics (runs each substep)
- Examples: Cooling, star formation, feedback, reincorporation
- Time integration with dt = timestep / SubSteps

**phase_2**: Secondary physics (runs each substep)
- Examples: Mergers, disruption, satellite tracking
- Typically depends on phase_1 results

**post_timestep**: Finalization (runs once after substeps)
- Examples: Converting accumulators to rates
- No time integration

**User configuration** (in YAML):
```yaml
modules:
  pre_timestep:
    - my_setup_module: process_full_halo
  phase_1:
    - my_physics_module: process_by_galaxy
```

### Module Best Practices

**Module independence**:
- Modules cannot call other module functions directly
- Communicate only through property system
- Declare dependencies in `module_info.yaml`

**Parameter handling**:
- Load parameters in `init()` using `LOAD_PARAM_*` macros
- Validate parameter values (physics-based validation)
- All parameters required (no defaults in code)

**Property access**:
- Read inputs from `gal->PropertyName`
- Write outputs to `gal->PropertyName`
- Don't modify read-only halo properties

**Memory management**:
- Use `mymalloc_cat()` / `myfree()` for allocations
- Category-track allocations (e.g., `MEM_PHYSICS`)
- Free all allocations in `cleanup()`

**Error handling**:
- Return 0 on success, non-zero on failure
- Use `ERROR_LOG()` for errors
- Use `VERBOSE_LOG()` for initialization/configuration info

**Shared utilities**:
- Place reusable physics code in `src/modules/_shared/`
- Header-only utilities for fast compilation
- Include via relative path: `#include "../_shared/my_utility.h"`

---

## Testing

### Test Framework Overview

Mimic uses **three-tier testing**:

**Tier 1: Unit Tests** (C, <10s)
- Test individual functions and modules in isolation
- Located in `tests/unit/`
- Example: `test_memory_system.c`, `test_property_reset.c`, `test_numeric_utilities.c`

**Tier 2: Integration Tests** (Python, <1min)
- Test end-to-end workflows and format compatibility
- Located in `tests/integration/`
- Example: `test_full_pipeline.py`, `test_output_formats.py`

**Tier 3: Scientific Tests** (Python, <5min)
- Validate physics accuracy against published results
- Located in `tests/scientific/`
- Example: `test_scientific.py`

### Running Tests

**Run all tests**:
```bash
make tests  # Validates metadata, runs all tiers
```

**Run specific tiers**:
```bash
make test-unit          # C unit tests only
make test-integration   # Python integration tests only
make test-scientific    # Physics validation only
```

**Run individual tests**:
```bash
cd tests/unit && ./test_memory_system.test
cd tests/integration && python test_full_pipeline.py
cd tests/scientific && python test_scientific.py
```

### Writing Unit Tests

**Create test file** (`tests/unit/test_unit_my_module.c`):
```c
#include <stdio.h>
#include <assert.h>
#include <math.h>
#include "core/module_registry.h"

void test_compute_physics(void) {
  /* Test physics calculation */
  float result = compute_physics_function(1.0, 2.0);
  assert(fabs(result - 2.0) < 1e-6);
  printf("✓ test_compute_physics passed\n");
}

int main(void) {
  printf("Running my_module unit tests...\n");
  test_compute_physics();
  printf("All tests passed!\n");
  return 0;
}
```

**Add to build system**: Tests are auto-discovered from `module_info.yaml`.

### Writing Integration Tests

**Create test file** (`tests/integration/test_integration_my_module.py`):
```python
#!/usr/bin/env python3
"""Integration test for my_module"""

import subprocess
import sys

def test_my_module_integration():
    """Test my_module in full pipeline"""
    # Run Mimic with test configuration
    result = subprocess.run(
        ['./mimic', 'tests/data/test_config.yaml'],
        capture_output=True,
        text=True
    )

    # Verify success
    assert result.returncode == 0, f"Mimic failed: {result.stderr}"

    # Verify output properties exist
    # (Add specific validation)

    print("✓ Integration test passed")

if __name__ == '__main__':
    test_my_module_integration()
```

### Writing Scientific Tests

Use existing framework in `tests/scientific/test_scientific.py`. Add validation functions for your module's physics.

---

## Development Workflow

### Building

**Standard build**:
```bash
make
```

**Clean build**:
```bash
make clean && make
```

**Parallel build** (faster):
```bash
make -j$(nproc)
```

**With optional features**:
```bash
make USE-HDF5=no USE-MPI=yes   # Disable HDF5 or enable MPI as needed (HDF5 on by default)
```

**Show build configuration**:
```bash
make info  # Shows compiler, libraries, features
```

### Code Generation

**When to regenerate**:
- After editing `halo_properties.yaml` or `model_properties.yaml`
- After editing any `module_info.yaml`
- After adding/removing modules

**Regenerate**:
```bash
make generate  # Smart - only regenerates changed files
```

**Verify generated code is current** (CI check):
```bash
make check-generated
```

### Code Formatting

**Format all code**:
```bash
./scripts/beautify.sh
```

**Format only C code**:
```bash
./scripts/beautify.sh --c-only
```

**Format only Python code**:
```bash
./scripts/beautify.sh --py-only
```

### Git Workflow

**Before committing**:
1. Format code: `./scripts/beautify.sh`
2. Run tests: `make tests`
3. Verify generated code: `make check-generated`
4. Ask before committing (per CLAUDE.md)

**Commit messages**:
- Meaningful description
- List every changed file, grouped logically
- Brief reason for each change

### Code Standards

**Follow professional standards**:
- Brief file/function headers (1-2 sentences)
- Comment only non-obvious logic
- Use `VERBOSE_LOG()` for initialization messages
- Minimal YAML (no comments in metadata)
- Consistent naming (`snake_case` for C)

---

## Advanced Topics

### Memory Management

**Category-tracked allocation**:
```c
#include "util/memory.h"

/* Allocate with category tracking */
float *data = mymalloc_cat(size * sizeof(float), MEM_PHYSICS);

/* Free */
myfree(data);
```

**Categories**: `MEM_HALOS`, `MEM_TREES`, `MEM_IO`, `MEM_UTILITY`, `MEM_PHYSICS`

**Check for leaks**:
```c
print_allocated_by_category();  /* Shows allocated memory by category */
```

Memory is automatically cleaned up at scope boundaries (per-forest).

### Unit System

**Code units** (internal):
- Mass: `1e10 Msun/h`
- Length: `Mpc/h`
- Velocity: `km/s`
- Time: `sec` (internal), `Myr` (output)

**Converting to physical units**:
```c
/* Remove h dependence */
float mass_physical = mass_code / hubble_h;  /* Msun */
float length_physical = length_code / hubble_h;  /* Mpc */
```

**Best practices**:
- Keep calculations in code units
- Convert only for output or display
- Document units in comments

### Execution Flow

**High-level flow**:
```
main()
  ↓
init_mimic()
  ↓
process_trees()
  ↓ (for each tree)
  load_tree_table()
    ↓
  build_halo_tree()
    ↓
  process_halo_evolution()
    ├─ execute_phase(pre_timestep)
    ├─ for each substep:
    │   ├─ execute_phase(phase_1)
    │   └─ execute_phase(phase_2)
    └─ execute_phase(post_timestep)
      ↓
  save_halos()
    ↓
  free_halos_and_tree()
  ↓
cleanup_mimic()
```

**Module execution**:
```
execute_phase(modules, num_modules, ctx, halos, ngal)
  ↓ (for each module in phase)
  module->process(ctx, halos, ngal)
```

### Debugging

**Debug build with verbose output**:
```bash
make clean && make
./mimic --debug input/millennium.yaml 2>&1 | tee debug.log
```

**Check memory leaks**:
```bash
./mimic --debug input/millennium.yaml
# Check final memory report in output
```

**Use Valgrind** (if needed):
```bash
valgrind --leak-check=full ./mimic input/millennium.yaml
```

**GDB debugging**:
```bash
gdb ./mimic
(gdb) run input/millennium.yaml
(gdb) bt  # Backtrace on crash
```

---

**Need more detail?**
- Complete vision: [VISION.md](VISION.md)
- Module schema reference: [REFERENCE.md](REFERENCE.md)
- User guide: [USER-GUIDE.md](USER-GUIDE.md)
