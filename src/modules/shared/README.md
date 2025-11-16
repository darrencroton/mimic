# Shared Physics Utilities

This directory contains physics utilities shared across multiple modules to avoid code duplication while maintaining a single source of truth.

## Available Utilities

- **`metallicity.h`** - Metallicity calculation (metal mass fraction)
- **`disk_radius.h`** - Disk scale radius calculation (Mo, Mao & White 1998)

## How It Works

- Utilities are **header-only files** placed directly in `src/modules/shared/`
- Modules include them using **relative paths**: `#include "../shared/utility_name.h"`
- Make's dependency tracking (`-MMD -MP`) automatically rebuilds modules when utilities change
- NO copying, NO code generation, NO build system magic - just simple includes

## Using a Shared Utility

```c
// In your module's .c file
#include "../shared/metallicity.h"

// Use the utility function
float Z = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);
```

Then build normally:
```bash
make clean && make
```

Changes to shared utilities automatically propagate to all modules that use them.

## Creating a New Shared Utility

1. **Create header file**: `src/modules/shared/my_utility.h`

2. **Implement as header-only** (using `static inline`):
```c
#ifndef MIMIC_SHARED_MY_UTILITY_H
#define MIMIC_SHARED_MY_UTILITY_H

#include "constants.h"

/**
 * @brief Brief description
 * @param x Description of parameter
 * @return Description of return value
 */
static inline double mimic_my_function(double x) {
    return x * 2.0;  // Implementation
}

#endif
```

3. **Use in modules**:
```c
#include "../shared/my_utility.h"
```

4. **Write tests and register**: Follow testing instructions below

## Testing Shared Utilities

Shared utility tests are automatically discovered and run via the test registry system.

### Adding a Test

1. **Create test file** in `shared/` directory:
```c
// src/modules/shared/test_unit_my_utility.c
#include "../../../tests/framework/test_framework.h"
#include "my_utility.h"

int test_my_function(void) {
    double result = mimic_my_function(5.0);
    TEST_ASSERT_DOUBLE_EQUAL(result, 10.0, 1e-6, "5.0 * 2 should be 10.0");
    return TEST_PASS;
}

int main(void) {
    TEST_RUN(test_my_function);
    TEST_SUMMARY();
    return TEST_RESULT();
}
```

2. **Add to test registry** in `module_info.yaml`:
```yaml
tests:
  unit:
    - test_unit_disk_radius.c
    - test_unit_my_utility.c  # Add your test here
```

3. **Regenerate test registry**:
```bash
make generate-test-registry
```

4. **Run tests**:
```bash
make test-unit  # Runs all unit tests including shared utilities
```

### Test Organization

- **Tests co-located with utilities** - All in `src/modules/shared/`
- **Single registry file** - `module_info.yaml` lists all shared tests
- **Auto-discovered** - Test runners automatically find and execute tests
- **Naming convention** - `test_unit_UTILITY_NAME.c` etc (consistent with modules)

See `docs/developer/testing.md` for comprehensive testing guide.

## Design Principles

- **Prefix functions with `mimic_`** - Prevents namespace pollution
- **Keep utilities simple** - One focused purpose per utility
- **Header-only when possible** - Use `static inline` for simplicity
- **Document thoroughly** - Include `@brief`, `@param`, `@return` annotations
- **Test everything** - Unit tests for all functions and edge cases
- **Use relative paths** - Makes dependencies explicit and self-documenting

## Why Relative Paths?

Relative paths (`#include "../shared/..."`) make dependencies crystal clear:
- You see exactly what file is being included
- Only ONE version of each file exists (single source of truth)
- No need to understand Makefile include paths
- Self-documenting code

## Architecture Note

All physics code (including shared utilities) lives in `src/modules/`. The core (`src/core/`, `src/util/`, `src/io/`) remains physics-agnostic.

---

**Design Philosophy**: Keep it simple. Header-only utilities placed directly in `shared/` with no subdirectories. This maintains Mimic's architectural principles while eliminating code duplication through simple, explicit relative path includes.
