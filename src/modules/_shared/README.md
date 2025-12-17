# User-Created Physics Utilities (_shared/)

**Purpose**: Reusable physics utilities and models created by module developers.

**YOU CAN MODIFY** - This is the user domain for shared code.

---

## What Goes Here

This directory contains **reusable physics utilities** that multiple modules can share:

- **Utility Functions**: Self-contained calculations (e.g., metallicity, disk radii)
- **Swappable Physics Models**: Physics models that can be replaced entirely (e.g., reionization suppression)
- **Shared Calculations**: Common astrophysical calculations used across modules

**Note**: This directory's contents will evolve as you develop modules. It's intentionally user-modifiable - add what you need!

---

## Usage in Modules

### Include a Shared Utility
```c
// In your module's .c file
#include "../_shared/my_utility.h"

// Use the utility functions
double result = mimic_my_calculation(param1, param2);
```

Then build normally:
```bash
make clean && make
```

Changes to shared utilities automatically propagate to all modules that use them (via Make's dependency tracking).

---

## Creating a New Shared Utility

### When to Create a Shared Utility

**Create a shared utility when**:
- ✅ Multiple modules need the same physics calculation
- ✅ The calculation is self-contained (no complex state)
- ✅ You want a single source of truth

**Don't create a shared utility when**:
- ❌ Only one module uses it (keep it in the module)
- ❌ It's a universal constant (use `_system/physical_constants.h`)
- ❌ It's a model parameter (read from input YAML via `model_get_*()` functions)

### Steps

1. **Create header file**: `src/modules/_shared/my_utility.h`

2. **Implement as header-only** (using `static inline`):
```c
#ifndef MIMIC_SHARED_MY_UTILITY_H
#define MIMIC_SHARED_MY_UTILITY_H

#include "constants.h"  // For EPSILON_SMALL, etc.

/**
 * @brief Brief description
 * @param x Description of parameter
 * @return Description of return value
 */
static inline double mimic_my_function(double x) {
    if (x <= EPSILON_SMALL)
        return 0.0;
    return x * 2.0;
}

#endif /* MIMIC_SHARED_MY_UTILITY_H */
```

3. **Use in modules**:
```c
#include "../_shared/my_utility.h"
```

4. **Write tests** (optional but recommended):
   - Create `src/modules/_shared/_tests/test_unit_my_utility.c`
   - Follow testing guide in `docs/DEVELOPER-GUIDE.md`

---

## Swappable Physics Models

Some utilities are actually **physics models** that can be swapped entirely. Design utilities to be swappable when appropriate.

### Swapping a Model

If you have a utility header that implements a physics model (e.g., a specific prescription for a process):

```bash
# Archive current implementation
mv src/modules/_shared/my_model.h src/modules/_shared/my_model_v1.h

# Install alternative implementation
cp alternative_implementation.h src/modules/_shared/my_model.h

# Rebuild
make clean && make
```

**No code changes needed** - all modules automatically use the new implementation!

---

## Testing Shared Utilities

### Adding Tests

1. **Create test file** in `_shared/_tests/` directory:
```c
// src/modules/_shared/_tests/test_unit_my_utility.c
#include "../../../tests/framework/test_framework.h"
#include "../my_utility.h"

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
    - _tests/test_unit_reionization.c
    - _tests/test_unit_my_utility.c  # Add here
```

3. **Run tests**:
```bash
make test-unit
```

See `docs/DEVELOPER-GUIDE.md` for comprehensive testing guide.

---

## Design Principles

### Naming Conventions
- **Prefix functions with `mimic_`** - Prevents namespace pollution
- **Use descriptive names** - `mimic_get_metallicity()` not `get_Z()`

### Code Quality
- **Keep utilities simple** - One focused purpose per utility
- **Header-only when possible** - Use `static inline` for simplicity
- **Document thoroughly** - Include `@brief`, `@param`, `@return` annotations
- **Test everything** - Unit tests for all functions and edge cases

### Include Paths
- **Use relative paths** - `#include "../_shared/..."` makes dependencies explicit
- **Self-documenting** - You see exactly what file is being included

---

## Difference from _system/

| Aspect | `_system/` | `_shared/` |
|--------|-----------|-----------|
| **Purpose** | Framework infrastructure | User utilities |
| **Ownership** | Framework (don't modify) | Users (can modify) |
| **Contents** | Constants, templates, generated code | Physics utilities, models |
| **Examples** | `physical_constants.h`, `output_helpers.h` | User-created utility headers |
| **Mutability** | Immutable | Mutable/swappable |

---

## Architecture Note

All physics code (including shared utilities) lives in `src/modules/`. The core (`src/core/`, `src/util/`, `src/io/`) remains **physics-agnostic**.

This maintains Mimic's architectural vision:
- **Physics-Agnostic Core** - Core has zero knowledge of physics
- **Single Source of Truth** - One place for each utility
- **Runtime Modularity** - Swap models without recompilation

---

## See Also

- `src/modules/_system/` - Framework infrastructure (constants, templates)
- `docs/DEVELOPER-GUIDE.md` - How to create modules and comprehensive testing
- `docs/VISION.md` - Framework architecture principles
