# Shared Module Tests (_tests/)

**Purpose**: Shared cross-module regression tests for the physics-module system.

**DO NOT PUT MODULE CODE HERE** - This directory is for tests only.

---

## What Goes Here

This directory holds the package's shared tests, the C headers that module tests include, the shared test registry and the SAGE26 function-level oracle. Module-specific tests belong in the owning module directory, `models/sage26/modules/<module>/_tests/`, and each module's `module_info.yaml` registers them. Shared core and simulation fixtures are generated under `build/generated/test_inputs/`.

Every file and directory directly under this directory:

- `README.md`: this file.
- `module_info.yaml`: the registry of the shared tests below; module-specific tests are not registered here.
- `test_unit_mixed_dt_parity.c`: per-object timesteps in a FoF whose galaxies hold different `dT`: the merger timestamp, reincorporation, star formation, cooling and radio-mode heating each use the object's own interval, a boundary row (`SnapNum < 0`, `dT <= 0`) is a no-op for reincorporation, and a non-boundary `dT <= 0` fails.
- `test_unit_sage_dependency_contracts.c`: the placement and partner checks of the modules' `init()`: each case configures one pipeline and asserts that initialisation fails (an error) or succeeds with the expected warning.
- `test_unit_sage26_bulge_size_parameter.c`: `BulgeSizeOn` in every module that calls a bulge-size helper: a missing key fails initialisation, values other than 0 and 3 are refused naming the parameter, and the mode-0 size helpers keep SAGE26's `BulgeSizeOn 0` behaviour.
- `test_unit_sage26_sf_prescription_parameters.c`: `SFprescription`, `H2RadialIntegrationOn`, `H2RadialNBins`, `H2RadialRMaxFactor` and `StarburstColdGasOn` in every module that reads them: a missing key fails initialisation and every unsupported value is refused naming the parameter.
- `test_unit_sage26_oracle_reader.c`: the oracle fixture reader's self-test: its limits, its read errors and its `i64` field type, on scratch files; platform independent.
- `test_integration_processing_mode_contracts.py`: startup rejects a module configured in a processing mode it does not support.
- `test_scientific_sage26_physics_baseline.py`: the package's full-physics regression baseline in the scientific tier, run under `MODEL=sage26 SIMULATION=mini-millennium` only.
- `test_scientific_compare_sage26.py`: a scientific-tier self-test that runs the galaxy-by-galaxy comparator's 23 synthetic controls, S0 to S7c (the instrument README, *Controls*), through the instrument's own scripts.
- `input/`: `test_physics_binary.yaml`, the run file of the full-physics regression baseline.
- `baseline/`: `physics-binary/`, the baseline's committed reference output.
- `sage_test_fixtures.h`: the standard fixture of the package's C unit tests: the `TEST_RUN` counters, `reset_config()`, module registration and `set_test_model_parameters()`, which sets the parameters a module test initialises from (of the six flags that define the configurations R0 to R6, only `BulgeSizeOn`, at 0).
- `sage26_oracle_fixture.h`: the oracle fixture reader and bit-exact comparator every oracle unit test uses, including `oracle_environment_mismatch()`, which makes those tests report SKIP outside the oracle environment.
- `sage26_oracle_join.h`: progenitor joining for the oracle tests: the core's inheritance, then the package's joining modules in the run files' order.
- `sage26_oracle_sizes.h`, `sage26_oracle_h2.h`, `sage26_oracle_hydrogen.h`, `sage26_oracle_fire.h`, `sage26_oracle_cgm.h` and `sage26_oracle_cooling.h`: the helpers of the oracle tests of the bulge-size, molecular-gas, merger-family hydrogen, FIRE feedback, `CGMrecipeOn 1` merger-family and `CGMrecipeOn 1` cooling fixtures (the oracle README's "The c07 Group", "The c08 Group", "The c09 Group", "The c10 Group, Part 3" and "The c11 Group").
- `oracle/`: the SAGE26 function-level oracle: the mains that call SAGE26's own routines, `build.sh`, `regenerate.sh`, `fixtures.manifest` and its [README](oracle/README.md), which defines the fixture format.

The package's tests register only for the simulations in `FULL_MODEL_TEST_SIMULATIONS` (`scripts/discovery.py`), which include `mini-millennium` and not `mini-millennium-horizontal`, so every command below names `SIMULATION=mini-millennium`.

---

## Adding a Test

### 1. Create Test File

Place your test in this directory:

```bash
# Unit test (C)
models/sage26/modules/_tests/test_unit_my_module.c

# Integration test (Python)
models/sage26/modules/_tests/test_integration_my_module.py

# Scientific test (Python)
models/sage26/modules/_tests/test_scientific_my_module.py
```

### 2. Register in module_info.yaml

Edit `models/sage26/modules/_tests/module_info.yaml` (paths are relative to this directory, so no `_tests/` prefix):

```yaml
tests:
  unit:
    - test_unit_my_module.c
  integration:
    - test_integration_my_module.py
  scientific:
    - test_scientific_my_module.py
```

### 3. Regenerate Test Registry

```bash
make MODEL=sage26 SIMULATION=mini-millennium generate
```

### 4. Run Tests

```bash
# Run all tests
make MODEL=sage26 SIMULATION=mini-millennium tests

# Run specific tier
make MODEL=sage26 SIMULATION=mini-millennium tests-unit
make MODEL=sage26 SIMULATION=mini-millennium tests-integration
make MODEL=sage26 SIMULATION=mini-millennium tests-scientific
```

---

## Test Templates

### Unit Test Template (C)

```c
/**
 * @file    test_unit_my_module.c
 * @brief   Cross-module tests for <what is being tested>
 */
#include "framework/test_framework.h"
#include "modules/_tests/sage_test_fixtures.h"

int test_my_physics_calculation(void) {
  /* SETUP */
  init_memory_system(0);
  reset_config();
  set_test_model_parameters();

  /* EXECUTE */
  double result = my_physics_function(100.0);

  /* VALIDATE */
  TEST_ASSERT(result > 0.0, "Expected positive result");
  TEST_ASSERT_DOUBLE_EQUAL(result, 42.0, 1e-6, "Result must match expected value");

  /* CLEANUP */
  check_memory_leaks();
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: My Module Tests\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_my_physics_calculation);

  TEST_SUMMARY();
  return TEST_RESULT();
}
```

See `tests/framework/c_unit_test_template.c` for the complete framework template.

### Integration Test Template (Python)

```python
#!/usr/bin/env python3
"""
One-line description of what this test validates.
"""

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).parent.parent.parent.parent.parent
sys.path.insert(0, str(REPO_ROOT / "tests"))

from framework import (
    BLUE, NC, MIMIC_EXE, TestNotApplicable, TestSkipped,
    result_error, result_fail, result_na, result_pass, result_skip,
)


def test_my_module_behavior():
    """Test description — what contract is being verified."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    # ... setup, run Mimic, assert on output ...


def main():
    tests = [test_my_module_behavior]

    if not MIMIC_EXE.exists():
        for test in tests:
            result_skip(test.__name__, "Mimic not built")
        return 0

    passed = failed = 0
    for test in tests:
        try:
            test()
            result_pass(test.__name__)
            passed += 1
        except TestSkipped as e:
            result_skip(test.__name__, str(e))
        except TestNotApplicable as e:
            result_na(test.__name__, str(e))
        except AssertionError as e:
            result_fail(test.__name__, str(e).splitlines()[0])
            failed += 1
        except Exception as e:
            result_error(test.__name__, str(e).splitlines()[0])
            failed += 1

    return 0 if failed == 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
```

See `tests/framework/python_integration_test_template.py` for the complete framework template.

---

## Test Discovery

Tests are discovered via `make MODEL=sage26 SIMULATION=mini-millennium generate`, which:

1. Scans `module_info.yaml` for test declarations
2. Generates test registries in `build/generated/`
3. Test runners use these registries to find and execute tests

---

## Directory Structure

```text
models/sage26/modules/
├── _tests/                          # This directory
│   ├── README.md                    # This file
│   ├── module_info.yaml             # Shared test registry
│   ├── input/                       # Shared module-pipeline input fixtures
│   ├── baseline/                    # Committed reference output for scientific tests
│   ├── test_unit_*.c                # Shared unit tests
│   ├── test_integration_*.py        # Shared integration tests
│   └── test_scientific_*.py         # Shared scientific tests
└── my_directory_module/             # Runtime module (owns its own tests)
    ├── my_directory_module.c
    ├── module_info.yaml
    └── _tests/
        ├── test_unit_*.c
        └── test_integration_*.py

Retired tests go to `archive/src-modules/_archive/` at the project root.
```

---

## Best Practices

### Naming Conventions
- **Unit tests**: `test_unit_MODULE_NAME.c` (matches module filename); a module's oracle unit test is `test_unit_MODULE_NAME_oracle.c`
- **Integration tests**: `test_integration_MODULE_NAME.py`
- **Scientific tests**: `test_scientific_MODULE_NAME.py`

### Test Organization
- **One test file per tier and concern** - A runtime module owns its own unit, oracle unit and integration tests; a shared test here covers one cross-module contract or shared parameter
- **Test all functions** - Cover edge cases and error conditions
- **Document test purpose** - Explain what physics is being validated

### When to Test Here vs. Module Directory
- **Module-specific runtime behavior**: `models/<model>/modules/<module>/_tests/`
- **Cross-module contracts and shared regressions**: `models/<model>/modules/_tests/`
- **Shared utilities**: `models/<model>/shared/_tests/`
- **Retired code/tests**: `archive/src-modules/_archive/`

---

## Architecture Note

This directory follows Mimic's standardized test directory naming:
- `_tests/` (with underscore) indicates a fixed infrastructure directory
- Distinguishes test directories from user code
- Consistent with `module_system/`, model-local `shared/`, and archive naming

---

## See Also

- [docs/DEVELOPER-GUIDE.md](../../../../docs/DEVELOPER-GUIDE.md) - Complete testing guide
- [tests/README.md](../../../../tests/README.md) - Test suite overview
- `models/<model>/shared/_tests/` - Shared utility tests
- `tests/unit/` - Core infrastructure tests
