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

**Example (C unit test)**: add the module to a user-named substep phase with the `test_phase_add()` helper from `tests/framework/test_phase_config.h` (the fixed `pre_timestep` and `post_snapshot` phases have `test_pre_timestep_add()` and `test_post_snapshot_add()`; `post_timestep` is set directly on `MimicConfig`).
```c
/* Run test_fixture once per galaxy in a named substep phase */
test_phase_add("galaxy_physics", "test_fixture", PROCESSING_MODE_BY_GALAXY);
MimicConfig.SubSteps = 1;

/* Configure module parameters */
snprintf(MimicConfig.ModelParams[0].param_name, MAX_STRING_LEN, "TestFixtureDummyParameter");
snprintf(MimicConfig.ModelParams[0].value, MAX_STRING_LEN, "2.5");
snprintf(MimicConfig.ModelParams[1].param_name, MAX_STRING_LEN, "TestFixtureEnableLogging");
snprintf(MimicConfig.ModelParams[1].value, MAX_STRING_LEN, "0");
MimicConfig.NumModelParams = 2;
```

**Example (Python integration test)**: phases are user-named keys in `phase_config` (`pre_timestep`, `post_timestep` and `post_snapshot` are the reserved fixed keys; `post_snapshot` runs only under the horizontal driver).
```python
param_file = create_test_param_file(
    phase_config={"galaxy_physics": [("test_fixture", "process_by_galaxy")]},
    model_params={"TestFixtureDummyParameter": "2.5"}
)

# Dual mode: the same module as a snapshot-wide callback (horizontal packages)
param_file = create_test_param_file(
    phase_config={"post_snapshot": [("test_fixture", "process_snapshot")]},
    model_params={"TestFixtureDummyParameter": "0.25", "TestFixtureEnableLogging": "1"}
)

# Record creation: with TestFixtureCreateRecords set the fixture must be process_full_halo
param_file = create_test_param_file(
    phase_config={"pre_timestep": [("test_fixture", "process_full_halo")]},
    model_params={
        "TestFixtureDummyParameter": "0.25",
        "TestFixtureEnableLogging": "0",
        "TestFixtureCreateRecords": "2",
    },
)
# Add "TestFixtureCreatedContent": "0.001" to give every created record that mass in
# RateFixtureHot and RateFixtureCold (without it the created records hold none)
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

**Supported modes** (dual mode): the three FoF modes `process_by_galaxy`, `process_per_event` and `process_full_halo`, bound to `test_fixture_process`, plus `process_snapshot`, bound to `test_fixture_process_snapshot`. Generated registration therefore sets both typed callbacks. The snapshot-only counterpart is [`test_snapshot_fixture`](../test_snapshot_fixture/README.md); the event fixtures are FoF-only.

**Parameters**:
- `TestFixtureDummyParameter` (double): Dummy parameter for testing parameter API
- `TestFixtureEnableLogging` (int): Enable verbose logging for test validation (0=minimal, 1=verbose)
- `TestFixtureCreateRecords` (int, **optional**): Records to create per Type 0 host on every `process()` call through `module_create_record()`. Absent means `0`, so every configuration written before the parameter existed is unchanged; a negative value fails `init()`. Creation is legal only from `process_full_halo`, so a configuration that sets it must place the fixture there: any other placement fails at the first creation call. See the [Record Creation Contract](../../../docs/DEVELOPER-GUIDE.md#record-creation-contract)
- `TestFixtureCreatedContent` (double, **optional**): the value `RateFixtureHot` and `RateFixtureCold` take on every record the fixture creates (the rate fixture's reservoirs, test builds only). Absent means `0`, which leaves a created record exactly as `module_create_record()` returned it, so every configuration written before the parameter existed is unchanged; a negative or non-finite value fails `init()`. A non-zero value is a discrete change to the conserved total `rate_fixture_mass`, which the module declares in `changes_totals` (a run with a rate phase refuses an undeclared change), so it lets a rate-phase run carry created rows with non-zero reservoir content through the rate pass and the write-back

**Properties Read and Written**: `RateFixtureHot` and `RateFixtureCold`, only on created records and only with `TestFixtureCreatedContent` above zero (see [`test_properties.yaml`](test_properties.yaml)). **Declared total change**: `rate_fixture_mass` (`changes_totals`).

**Properties Provided**:
- `TestDummyProperty` (float): Test property for infrastructure testing (not written to output)
- Seven `RateFixture*` properties, declared in [`test_properties.yaml`](test_properties.yaml) beside `TestDummyProperty` and written to output (`output: true`), so every test build of every package writes them. Each is a `double` in `1e10 Msun/h` with range `[0, 100000]`, and each is zero unless a rate phase runs or `TestFixtureCreatedContent` is set. Three are reservoirs of the conserved total `rate_fixture_mass` (`RateFixtureHot`, `RateFixtureCold`, `RateFixtureStars`), three are tracers of the total `rate_fixture_tracer` riding on them (`RateFixtureHotTracer`, `RateFixtureColdTracer`, `RateFixtureStarsTracer`), and one is an observer of the cumulative mass formed into stars (`RateFixtureFormed`). `test_fixture` itself reads and writes only `RateFixtureHot` and `RateFixtureCold`.

**Dependencies**: None

## Implementation

The module performs minimal operations:
1. **Init**: Reads parameters, logs configuration
2. **Process** (FoF family): Sets `TestDummyProperty = DummyParameter` on every Type 0 galaxy in the array. When `TestFixtureCreateRecords > 0` it also creates that many records on each of those Type 0 rows, sets `TestDummyProperty = DummyParameter` and `RateFixtureHot = RateFixtureCold = CreatedContent` on each, and logs `TEST_FIXTURE_CREATE: host=<UniqueGalaxyID> created=<n>` once per host (whatever `TestFixtureEnableLogging` says); a refused creation makes the call fail
3. **Process snapshot** (snapshot family, dispatched from `modules.post_snapshot`): Checks the borrowed-view contract, then sets `TestDummyProperty = DummyParameter` on every entry of any Type through `halos[i].galaxy`. When `TestFixtureEnableLogging=1` it logs `TEST_FIXTURE_SNAPSHOT_EXEC: count=<n> snapshot=<s> n=<count> z=<z> seen_min=<v> seen_max=<v>` per call, where `seen_min`/`seen_max` are the smallest and largest `TestDummyProperty` it found before writing (what an earlier `post_snapshot` entry wrote). A non-empty population with `DummyParameter` outside `TestDummyProperty`'s declared `[0, 1]` range is refused with return code 2 before anything is written — the fixture's way to make a real run's snapshot callback fail; an empty population succeeds
4. **Cleanup**: No resources to free

## Related Documentation

- **Testing Conventions**: [docs/DEVELOPER-GUIDE.md](../../../docs/DEVELOPER-GUIDE.md#testing)
- **Vision Principles**: [docs/VISION.md](../../../docs/VISION.md)

---

**Remember**: This module is a **test fixture**, not a physics module. It exists to make infrastructure tests physics-agnostic and future-proof.
