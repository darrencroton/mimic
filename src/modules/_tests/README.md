# Standalone Module Tests (_tests/)

**Purpose**: Tests for standalone physics modules (single `.c` files in `src/modules/`).

**DO NOT PUT MODULE CODE HERE** - This directory is for tests only.

---

## What Goes Here

This directory contains **tests for standalone modules**:

- **Unit Tests** (`test_unit_*.c`): Test individual module functions and physics calculations
- **Integration Tests** (`test_integration_*.py`): Test modules in full pipeline execution
- **Scientific Tests** (`test_scientific_*.py`): Validate physics accuracy against published results

**Standalone modules** are single `.c` files directly in `src/modules/` (e.g., `sage_add_cooling.c`, `sage_reionization.c`).

**Directory modules** (modules with their own subdirectory like `sage_calculate_cooling/`) should place tests in their own `_tests/` subdirectory instead.

---

## Adding a Test

### 1. Create Test File

Place your test in this directory:

```bash
# Unit test (C)
src/modules/_tests/test_unit_my_module.c

# Integration test (Python)
src/modules/_tests/test_integration_my_module.py

# Scientific test (Python)
src/modules/_tests/test_scientific_my_module.py
```

### 2. Register in module_info.yaml

Edit `src/modules/_tests/module_info.yaml`:

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
make generate
```

### 4. Run Tests

```bash
# Run all tests
make tests

# Run specific tier
make test-unit
make test-integration
make test-scientific
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
    printf("✓ test_my_physics_calculation passed\n");
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

def test_my_module_integration():
    """Test my_module in full pipeline"""

    # Run Mimic
    result = subprocess.run(
        ['./mimic', 'tests/data/test_config.yaml'],
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

    print("✓ Integration test passed")

if __name__ == '__main__':
    test_my_module_integration()
```

---

## Test Discovery

Tests are auto-discovered via `make generate`, which:

1. Scans `module_info.yaml` for test declarations
2. Generates test registries in `build/generated/`
3. Test runners use these registries to find and execute tests

**No manual registration needed** - just add to `module_info.yaml` and run `make generate`.

---

## Directory Structure

```
src/modules/
├── _tests/                          # This directory
│   ├── README.md                    # This file
│   ├── module_info.yaml             # Test registry
│   ├── test_unit_*.c                # Unit tests
│   ├── test_integration_*.py        # Integration tests
│   └── test_scientific_*.py         # Scientific tests
├── my_standalone_module.c           # Standalone module (tested here)
├── another_standalone_module.c      # Another standalone module
└── my_directory_module/             # Directory module (has own _tests/)
    ├── my_directory_module.c
    ├── module_info.yaml
    └── _tests/                      # Module's own tests (NOT here)
        └── test_unit_*.c
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
- **Standalone modules** → Test in `_tests/`
- **Directory modules** → Test in module's own `_tests/` subdirectory
- **Shared utilities** → Test in `_shared/_tests/`

---

## Architecture Note

This directory follows Mimic's standardized test directory naming:
- `_tests/` (with underscore) indicates a fixed infrastructure directory
- Distinguishes test directories from user code
- Consistent with `_system/`, `_shared/`, `_archive/` naming

---

## See Also

- `docs/DEVELOPER-GUIDE.md` - Complete testing guide
- `tests/README.md` - Test suite overview
- `src/modules/_shared/_tests/` - Shared utility tests
- `tests/unit/` - Core infrastructure tests
