# Physics Modules

**Purpose:** Runtime-configurable galaxy physics modules.

**Status:** Module system complete (Phase 4). Contains production modules with comprehensive testing.

**Directory Structure:**
```
src/modules/
├── _archive/          # Archived modules (historical reference)
├── _system/           # System infrastructure (don't touch)
│   ├── generated/     # Auto-generated module registration
│   ├── template/      # Template for creating new modules
│   └── test_fixture/  # Infrastructure testing module
├── shared/            # Shared physics utilities for modules
├── sage_cooling/      # Production: SAGE gas cooling model
├── sage_infall/       # Production: SAGE infall and stripping
└── simple_sfr/        # Proof-of-concept: Simple star formation
```

**User-Facing Content:**
- **Physics Modules**: `sage_infall/`, `sage_cooling/`, `simple_sfr/`
- **Shared Utilities**: `shared/` - Reusable physics utilities (e.g., metallicity calculations)

**System Infrastructure** (underscore prefix = don't touch):
- `_system/template/` - Template for creating new modules
- `_system/generated/` - Auto-generated module registration code
- `_system/test_fixture/` - Infrastructure testing module (not for production use)
- `_archive/` - Archived modules for historical reference

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

3. **Create module metadata** (`module_info.yaml`) declaring tests

4. **Create tests** (see Testing section below)

5. **Generate and build**:
   ```bash
   make generate  # Auto-generates registration code
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
   - Example: `sage_infall/test_unit_sage_infall.c`

2. **Integration test (Python)**: `test_integration_MODULE_NAME.py`
   - Tests module in full pipeline, property output, configuration
   - Example: `sage_infall/test_integration_sage_infall.py`

3. **Scientific test (Python)**: `test_scientific_MODULE_NAME_validation.py`
   - Tests physics correctness, property ranges, conservation laws
   - Example: `sage_infall/test_scientific_sage_infall_validation.py`

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

**Do NOT use templates from `tests/framework/`** - those are for core infrastructure tests only.

**Instead, copy existing module tests as examples**:

```bash
# Copy from sage_infall (most complete example)
cp src/modules/sage_infall/test_unit_sage_infall.c \
   src/modules/my_module/test_unit_my_module.c

cp src/modules/sage_infall/test_integration_sage_infall.py \
   src/modules/my_module/test_integration_my_module.py

cp src/modules/sage_infall/test_scientific_sage_infall_validation.py \
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
- Use existing module tests as templates (copy from `sage_infall/`, `simple_cooling/`, etc.)
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

## Module Development Resources

- **Template**: `src/modules/_system/template/` - Boilerplate for new modules
- **Examples**: `sage_infall/`, `sage_cooling/`, `simple_sfr/` - Working examples
- **Shared Utilities**: `shared/` - Reusable physics utilities (metallicity, etc.)
- **Developer Guide**: [docs/developer/module-developer-guide.md](../../docs/developer/module-developer-guide.md)
- **Testing Guide**: [docs/developer/testing.md](../../docs/developer/testing.md)
- **Architecture**: [docs/architecture/vision.md](../../docs/architecture/vision.md)

---

**Last Updated**: 2025-11-13 (Phase 4 - Module Testing Complete)
