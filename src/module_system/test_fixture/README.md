# Test Fixture Module

**WARNING: This module is for TESTING INFRASTRUCTURE ONLY.**

**DO NOT USE IN PRODUCTION RUNS**

## Purpose

This minimal module exists solely to test the core module system functionality (configuration, registration, pipeline execution) without coupling infrastructure tests to production physics modules.

This maintains **Vision Principle #1: Physics-Agnostic Core Infrastructure**.

## Architecture Rationale

Infrastructure tests in `tests/unit/` and `tests/integration/` must not hardcode production module names. Doing so would violate the Physics-Agnostic Core principle: production module changes would break infrastructure tests, and archiving production modules would require updating core tests.

The `test_fixture` module provides a stable, physics-free module that keeps the module system contract testable without coupling infrastructure tests to any production physics implementation.

## Usage

### In Infrastructure Tests

**Use this module** for testing:
- Module configuration system
- Module registration and lifecycle
- Parameter parsing
- Pipeline execution
- Error handling

**Example (C unit test)**: add the module to a user-named substep phase with the
`test_phase_add()` helper from `tests/framework/test_phase_config.h` (the fixed
`pre_timestep`/`post_timestep` phases are set directly on `MimicConfig`).
```c
/* Run test_fixture once per galaxy in a named substep phase */
test_phase_add("galaxy_physics", "test_fixture", PROCESSING_MODE_BY_GALAXY);
MimicConfig.SubSteps = 1;

/* Configure module parameters */
strcpy(MimicConfig.ModuleParams[0].module_name, "TestFixture");
strcpy(MimicConfig.ModuleParams[0].param_name, "DummyParameter");
strcpy(MimicConfig.ModuleParams[0].value, "2.5");
```

**Example (Python integration test)**: phases are user-named keys in
`phase_config` (`pre_timestep`/`post_timestep` are the only reserved names).
```python
param_file = create_test_param_file(
    phase_config={"galaxy_physics": [("test_fixture", "process_by_galaxy")]},
    model_params={"TestFixtureDummyParameter": "2.5"}
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
- `TestFixtureDummyParameter` (double): Dummy parameter for testing parameter API
- `TestFixtureEnableLogging` (int): Enable verbose logging for test validation (0=minimal, 1=verbose)

**Properties Provided**:
- `TestDummyProperty` (float): Test property for infrastructure testing (not written to output)

**Dependencies**: None

## Implementation

The module performs minimal operations:
1. **Init**: Reads parameters, logs configuration
2. **Process**: Sets `TestDummyProperty = DummyParameter` on all galaxies
3. **Cleanup**: No resources to free

## Related Documentation

- **Testing Conventions**: [docs/DEVELOPER-GUIDE.md](../../../docs/DEVELOPER-GUIDE.md#testing)
- **Vision Principles**: [docs/VISION.md](../../../docs/VISION.md)

---

**Remember**: This module is a **test fixture**, not a physics module. It exists to make infrastructure tests physics-agnostic and future-proof.
