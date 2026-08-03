# Shared Module Tests (_tests/)

**Purpose**: Shared cross-module regression tests for the physics-module system.

**DO NOT PUT MODULE CODE HERE** - This directory is for tests only.

---

## What Goes Here

This directory contains **shared tests only**:

- **Unit Tests** (`test_unit_*.c`): Cross-module or framework-facing regressions
- **Integration Tests** (`test_integration_*.py`): Startup/dispatch/pipeline contract tests spanning multiple modules
- **Scientific Tests** (`test_scientific_*.py`): Shared scientific validations, if needed
- **Input Fixtures** (`input/*.yaml`): Module-pipeline-specific run files, such as the full SAGE physics baseline input

Module-specific tests belong in the owning module directory:
`models/<model>/modules/<module>/_tests/`.
Broad model-level fixtures belong beside the owning model tests, typically in
`models/<model>/modules/_tests/input/`. Shared core and simulation fixtures are
generated under `build/generated/test_inputs/`.
Retired tests belong in `archive/src-modules/_archive/`.

---

## Adding a Test

### 1. Create Test File

Place your test in this directory:

```bash
# Unit test (C)
models/sage16/modules/_tests/test_unit_my_module.c

# Integration test (Python)
models/sage16/modules/_tests/test_integration_my_module.py

# Scientific test (Python)
models/sage16/modules/_tests/test_scientific_my_module.py
```

### 2. Register in module_info.yaml

Edit `models/sage16/modules/_tests/module_info.yaml` (paths are relative to this directory, so no `_tests/` prefix):

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
make MODEL=sage16 generate
```

### 4. Run Tests

```bash
# Run all tests
make MODEL=sage16 tests

# Run specific tier
make MODEL=sage16 tests-unit
make MODEL=sage16 tests-integration
make MODEL=sage16 tests-scientific
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
    BLUE, NC, MIMIC_EXE, TestSkipped,
    result_error, result_fail, result_pass, result_skip,
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

Tests are discovered via `make MODEL=sage16 generate`, which:

1. Scans `module_info.yaml` for test declarations
2. Generates test registries in `build/generated/`
3. Test runners use these registries to find and execute tests

---

## Directory Structure

```text
models/sage16/modules/
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
