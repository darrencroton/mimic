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

Edit `models/sage16/modules/_tests/module_info.yaml`:

```yaml
tests:
  unit:
    - _tests/test_unit_my_module.c
  integration:
    - _tests/test_integration_my_module.py
  scientific:
    - _tests/test_scientific_my_module.py
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
// test_unit_my_module.c
#include <stdio.h>
#include <assert.h>
#include <math.h>

void test_my_physics_calculation(void) {
    // Setup
    double input = 100.0;

    // Execute
    double result = my_physics_function(input);

    // Verify
    assert(fabs(result - expected) < 1e-6);
    printf("test_my_physics_calculation passed\n");
}

int main(void) {
    printf("Running my_module unit tests...\n");
    test_my_physics_calculation();
    printf("All tests passed!\n");
    return 0;
}
```

### Integration Test Template (Python)

```python
#!/usr/bin/env python3
"""Integration test for my_module"""

import subprocess
import h5py
import numpy as np
from pathlib import Path
import sys

REPO_ROOT = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO_ROOT / "tests"))
from framework import core_input_file

def test_my_module_integration():
    """Test my_module in full pipeline"""

    # Run Mimic
    result = subprocess.run(
        ['./mimic', str(core_input_file('test_hdf5.yaml'))],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True
    )

    # Verify success
    assert result.returncode == 0, f"Mimic failed: {result.stderr}"

    # Verify output
    with h5py.File('output/test/model_000.hdf5', 'r') as f:
        halos = f['Snap063/Galaxies'][:]

        # Check property exists and is valid
        assert 'MyProperty' in halos.dtype.names
        assert np.all(halos['MyProperty'] >= 0.0)

    print("Integration test passed")

if __name__ == '__main__':
    test_my_module_integration()
```

---

## Test Discovery

Tests are auto-discovered via `make MODEL=sage16 generate`, which:

1. Scans `module_info.yaml` for test declarations
2. Generates test registries in `build/generated/`
3. Test runners use these registries to find and execute tests

**No manual registration needed** - just add to `module_info.yaml` and run `make MODEL=sage16 generate`.

---

## Directory Structure

```
models/sage16/modules/
├── _tests/                          # This directory
│   ├── README.md                    # This file
│   ├── module_info.yaml             # Shared test registry
│   ├── input/                       # Shared module-pipeline input fixtures
│   ├── test_unit_*.c                # Shared unit tests
│   ├── test_integration_*.py        # Shared integration tests
│   └── test_scientific_*.py         # Shared scientific tests
├── _archive/                        # Retired modules/tests
└── my_directory_module/             # Runtime module (owns its own tests)
    ├── my_directory_module.c
    ├── module_info.yaml
    └── _tests/
        ├── test_unit_*.c
        └── test_integration_*.py
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
