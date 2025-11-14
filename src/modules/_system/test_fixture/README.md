# Test Fixture Module

⚠️ **WARNING: This module is for TESTING INFRASTRUCTURE ONLY** ⚠️

**DO NOT USE IN PRODUCTION RUNS**

## Purpose

This minimal module exists solely to test the core module system functionality (configuration, registration, pipeline execution) without coupling infrastructure tests to production physics modules.

This maintains **Vision Principle #1: Physics-Agnostic Core Infrastructure**.

## Architecture Rationale

Infrastructure tests in `tests/unit/` and `tests/integration/` previously hardcoded production module names (`simple_cooling`, `simple_sfr`), creating an architectural violation:

- ❌ **Violated**: Physics-Agnostic Core principle
- ❌ **Problem**: Production module changes broke infrastructure tests
- ❌ **Problem**: Archiving production modules required updating core tests

The `test_fixture` module fixes this by providing a stable, minimal test module that:

- ✅ Never changes (stable test interface)
- ✅ Has no real physics (agnostic to production science)
- ✅ Provides minimal functionality for testing infrastructure
- ✅ Allows production modules to be archived with zero infrastructure test changes

## Usage

### In Infrastructure Tests

**Use this module** for testing:
- Module configuration system
- Module registration and lifecycle
- Parameter parsing
- Pipeline execution
- Error handling

**Example (C unit test)**:
```c
strcpy(MimicConfig.EnabledModules[0], "test_fixture");
strcpy(MimicConfig.ModuleParams[0].module_name, "TestFixture");
strcpy(MimicConfig.ModuleParams[0].param_name, "DummyParameter");
strcpy(MimicConfig.ModuleParams[0].value, "2.5");
```

**Example (Python integration test)**:
```python
param_file = create_test_param_file(
    enabled_modules=["test_fixture"],
    module_params={"TestFixture_DummyParameter": "2.5"}
)
```

### NEVER Use in Production

This module should **NEVER** appear in:
- Production parameter files
- Scientific validation runs
- Performance benchmarks
- Published results

## Module Specification

**Name**: `test_fixture`
**Version**: 1.0.0
**Category**: testing

**Parameters**:
- `TestFixture_DummyParameter` (double, default=1.0): Dummy parameter for testing parameter API
- `TestFixture_EnableLogging` (int, default=0): Enable verbose logging for test validation

**Properties Provided**:
- `TestDummyProperty` (float): Test property for infrastructure testing (not written to output)

**Dependencies**: None

## Implementation

The module performs minimal operations:
1. **Init**: Reads parameters, logs configuration
2. **Process**: Sets `TestDummyProperty = DummyParameter` on all galaxies
3. **Cleanup**: No resources to free

Total implementation: ~150 lines of well-documented code

## Related Documentation

- **Architecture Fix**: `docs/architecture/module-implementation-log.md` (2025-11-13 entry)
- **Testing Conventions**: `docs/developer/testing.md` (Infrastructure Testing section)
- **Vision Principles**: `docs/architecture/vision.md` (Principle #1)

## History

- **2025-11-13**: Created to fix architectural violation where infrastructure tests hardcoded production module names
- **Impact**: Eliminates 15 architectural violations across 9 test methods
- **Benefit**: Future module archives require zero infrastructure test changes

---

**Remember**: This module is a **test fixture**, not a physics module. It exists to make infrastructure tests physics-agnostic and future-proof.
