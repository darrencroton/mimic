# Physics Modules

**Purpose:** Runtime-configurable galaxy physics modules.

**Status:** Module system complete (Phase 4). Contains production modules with comprehensive testing.

**Directory Structure:**
```
src/modules/
├── _archive/          # Archived modules (historical reference)
├── _system/           # Framework infrastructure (don't modify)
│   ├── physical_constants.h  # Universal physical constants
│   ├── output_helpers.h      # Output formatting utilities
│   ├── generated/            # Auto-generated module registration
│   ├── template/             # Template for creating new modules
│   └── test_fixture/         # Infrastructure testing module
├── _shared/           # User physics utilities (can modify/add)
│   └── *.h                   # Reusable calculations and swappable models
├── module_a/          # Example: Physics module
├── module_b/          # Example: Physics module
└── module_c/          # Example: Physics module
```

**User-Facing Content:**
- **Physics Modules**: Individual module directories - each contains code, tests, and metadata
- **User Utilities** (`_shared/`): Reusable physics utilities you can modify/add
  - Header-only utilities for shared calculations
  - Swappable physics models
  - See `_shared/README.md` for creating and using shared utilities

**Framework Infrastructure** (underscore prefix = don't modify):
- **`_system/`**: Framework constants and infrastructure
  - `physical_constants.h` - Universal physical constants (G, c, Z_sun, etc.)
  - `output_helpers.h` - Output formatting utilities
  - `generated/` - Auto-generated module registration code
  - `template/` - Template for creating new modules
  - `test_fixture/` - Infrastructure testing module
- **`_archive/`**: Archived modules for historical reference

**Note:** Core halo physics (virial calculations, tracking) are in `src/core/halo_properties/` as they are core infrastructure, not modular galaxy physics.

---

## Creating a New Module

### Quick Start

1. **Copy template**:
   ```bash
   cp -r src/modules/_system/template src/modules/my_module
   cd src/modules/my_module
   ```

2. **Create module code** (see `_system/template/README.md` for detailed instructions)

3. **Create module metadata** (`module_info.yaml`) declaring:
   - Module dependencies (properties and parameters)
   - Test files
   - Module metadata (name, version, etc.)

4. **Create tests** (see Testing section below)

5. **Generate and build**:
   ```bash
   make generate  # Auto-generates registration and parameter validation code
   make           # Build
   ```

See **[docs/developer/module-developer-guide.md](../../docs/developer/module-developer-guide.md)** for comprehensive module development guide.

---

## Module Testing

**Critical**: Every module must have three test files that are **co-located with the module code** and declared in `module_info.yaml`.

### Required Test Files

Each module directory must contain:

1. **Unit test (C)**: `test_unit_MODULE_NAME.c`
   - Tests module lifecycle, initialization, parameters, memory safety
   - See existing module test files for examples

2. **Integration test (Python)**: `test_integration_MODULE_NAME.py`
   - Tests module in full pipeline, property output, configuration
   - See existing module test files for examples

3. **Scientific test (Python)**: `test_scientific_MODULE_NAME_validation.py`
   - Tests physics correctness, property ranges, conservation laws
   - See existing module test files for examples

### Test Declaration

Declare tests in `module_info.yaml`:

```yaml
module:
  name: my_module
  # ... other metadata ...

  tests:
    unit: test_unit_my_module.c
    integration: test_integration_my_module.py
    scientific: test_scientific_my_module_validation.py
```

### Creating Module Tests

**Copy existing module tests as examples** (don't use core test templates from `tests/framework/`):

```bash
# Copy from an existing module with similar physics
cp src/modules/existing_module/test_unit_existing_module.c \
   src/modules/my_module/test_unit_my_module.c

cp src/modules/existing_module/test_integration_existing_module.py \
   src/modules/my_module/test_integration_my_module.py

cp src/modules/existing_module/test_scientific_existing_module_validation.py \
   src/modules/my_module/test_scientific_my_module_validation.py

# Then adapt for your module's physics
```

### Test Discovery and Execution

Tests are **automatically discovered** from `module_info.yaml`:

```bash
# Regenerate test registry (discovers new tests)
make generate-test-registry

# Run all tests (includes module tests)
make tests

# Run specific test tier (includes module tests for that tier)
make test-unit          # Runs core + module unit tests
make test-integration   # Runs core + module integration tests
make test-scientific    # Runs core + module scientific tests
```

### Key Points

✅ **DO**:
- Co-locate tests with module code in `src/modules/MODULE_NAME/`
- Declare tests in `module_info.yaml` under `tests:` section
- Follow strict naming: `test_unit_*.c`, `test_integration_*.py`, `test_scientific_*_validation.py`
- Use existing module tests as templates (copy from existing modules)
- Test software quality in unit/integration tests
- Test physics correctness in scientific tests
- Check for memory leaks in C unit tests

❌ **DON'T**:
- Use core test templates from `tests/framework/` (those are for core infrastructure only)
- Put module tests in `tests/` directory (they must be co-located with module code)
- Hardcode module tests in test runners (they're auto-discovered)
- Skip test declaration in `module_info.yaml` (required for discovery)
- Test physics in unit tests (unit tests are for software quality only)

### Comprehensive Testing Guide

See **[docs/developer/testing.md](../../docs/developer/testing.md)** for:
- Complete module testing guide with full code examples
- Step-by-step instructions for creating each test type
- Best practices and common patterns
- Debugging guide for test failures
- Distinction between core tests and module tests

---

## Module Parameters

**Simple Parameter System**: Modules read parameters directly from the input YAML file using type-safe accessor functions:
- `model_get_double(name, &var)` - Read double parameter
- `model_get_int(name, &var)` - Read integer parameter
- `model_get_string(name, buf, len)` - Read string parameter

**Parameter Declaration**: Each module declares which parameters it uses in the `dependencies.parameters` section of its `module_info.yaml`:

```yaml
dependencies:
  # All model parameters accessed via model_get_*() functions
  parameters:
    - BaryonFrac
    - SfrEfficiency
```

**Helper Macros**: For convenient parameter loading and validation, use macros from `_system/parameter_helpers.h`:

```c
#include "../_system/parameter_helpers.h"

// Load and validate in one call
LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("BaryonFrac", baryon_frac, 0.0, 1.0,
                                  "cosmic baryon fraction must be physical");

// Or load first, validate later for complex validation
LOAD_PARAM_DOUBLE("SfrEfficiency", sfr_efficiency);
if (sfr_efficiency < 0.001) {
    WARN_LOG("SfrEfficiency unusually low: %.4f", sfr_efficiency);
}
VALIDATE_RANGE_INCLUSIVE("SfrEfficiency", sfr_efficiency, 0.0, 1.0, NULL);
```

**Validation**: The parameter linter (`make lint-parameters`) automatically verifies that declared parameters match actual usage.

---

## Module Development Resources

- **Template**: `_system/template/` - Boilerplate for new modules
- **Examples**: Existing module implementations - Working examples to study
- **Constants**: `_system/physical_constants.h` - Universal physical constants
- **Utilities**: `_shared/` - Reusable physics utilities (see `_shared/README.md`)
- **Developer Guide**: [docs/developer/module-developer-guide.md](../../docs/developer/module-developer-guide.md)
- **Testing Guide**: [docs/developer/testing.md](../../docs/developer/testing.md)
- **Architecture**: [docs/architecture/vision.md](../../docs/architecture/vision.md)

---

**Last Updated**: 2025-11-13 (Phase 4 - Module Testing Complete)
