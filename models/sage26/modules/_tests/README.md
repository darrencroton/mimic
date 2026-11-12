# Shared Module Tests (_tests/)

**Purpose**: Shared cross-module regression tests for the physics-module system.

**DO NOT PUT MODULE CODE HERE** - This directory is for tests only.

---

## What Goes Here

This directory contains **shared tests only**:

- **Unit Tests** (`test_unit_*.c`): Cross-module or framework-facing regressions
- **Integration Tests** (`test_integration_*.py`): Startup/dispatch/pipeline contract tests spanning multiple modules
- **Scientific Tests** (`test_scientific_*.py`): Shared scientific validations, if needed
- **Input Fixtures** (`input/*.yaml`): Module-pipeline-specific run files, such as the full sage26 physics baseline input

Module-specific tests belong in the owning module directory:
`models/<model>/modules/<module>/_tests/`.
Broad model-level fixtures belong beside the owning model tests, typically in
`models/<model>/modules/_tests/input/`. Shared core and simulation fixtures are
generated under `build/generated/test_inputs/`.
Retired tests belong in `archive/src-modules/_archive/`.

The shared tests of this package, registered in `module_info.yaml`:

- `test_unit_mixed_dt_parity.c`: per-object timesteps in a FoF whose galaxies hold different `dT`.
- `test_unit_sage_dependency_contracts.c`: the placement and partner checks of the modules' `init()`, one pipeline per table row, each refusal asserted with the refusing module and the wording of the check.
- `test_unit_sage26_parameter_refusals.c`: the run-file parameter refusals of every module that reads parameters (missing keys, unsupported values, out-of-range numbers), one table row per change to the complete default configuration, plus the accepted values that must still initialise.
- `test_unit_sage26_bulge_size_parameter.c` and `test_unit_sage26_sf_prescription_parameters.c`: what `BulgeSizeOn` and the supported `SFprescription` values select (the mode-0 size helpers and the disk-radius module's mode-3 requirement; whether the hydrogen caps track H2); their refusals are in the parameter-refusal table.
- `test_integration_processing_mode_contracts.py`: startup rejects a module configured in a processing mode it does not support.
- `test_scientific_sage26_physics_baseline.py`: the package's full-physics regression baseline under `MODEL=sage26 SIMULATION=mini-millennium` (`input/test_physics_binary.yaml` against `baseline/physics-binary/`), the input's agreement with the run file, and that the fresh output holds only finite values inside the declared property ranges.
- The package's plotting tests under `models/sage26/plots/tests/` are registered here in the scientific tier because the plots directory has no registry of its own.

`sage_test_fixtures.h` is the standard fixture of the package's C unit tests (including the full-physics run file's parameter table that the parameter-refusal and dependency-contract tests share), and `sage26_run_helpers.py` holds the shared machinery of the integration tests that drive real runs.

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
make MODEL=sage26 tests

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

from framework import MIMIC_EXE, SharedRuns, TestSkipped, run_test_suite

# One Mimic run per distinct configuration, shared by the cases that assert on it
RUNS = SharedRuns()

# PHASES: the module pipeline; PARAMS: every parameter the configured modules read
PHASES = {"galaxy_physics": [("my_module", "process_by_galaxy")]}
PARAMS = {"Param1Name": 1.0}


def test_my_module_behavior():
    """Test description: what contract is being verified."""
    if not MIMIC_EXE.exists():
        raise TestSkipped("Mimic not built")
    run = RUNS.run("my_module", phase_config=PHASES, model_params=PARAMS, snapshot_list=[63])
    assert run.returncode == 0, run.stderr
    # ... load run.output_dir and assert on the output ...


def main():
    try:
        return run_test_suite([test_my_module_behavior], "My module integration tests")
    finally:
        RUNS.cleanup()


if __name__ == "__main__":
    sys.exit(main())
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
- **Unit tests**: `test_unit_MODULE_NAME.c` (matches module filename)
- **Integration tests**: `test_integration_MODULE_NAME.py`
- **Scientific tests**: `test_scientific_MODULE_NAME.py`

### Test Organization
- **One test file per module** - Keep tests focused
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
