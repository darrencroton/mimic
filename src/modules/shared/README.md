# Shared Physics Utilities

This directory contains physics utilities shared across multiple modules to avoid code duplication while maintaining a single source of truth.

## How It Works

- Utilities live in `src/modules/shared/UTILITY_NAME/`
- Modules include them using **relative paths**: `#include "../shared/UTILITY_NAME/utility.h"`
- Make's dependency tracking (`-MMD -MP`) automatically rebuilds modules when utilities change
- NO copying, NO code generation, NO build system magic

## Using a Shared Utility

```c
// In your module's .c file
#include "../shared/metallicity/metallicity.h"

// Use the utility function
float Z = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);
```

Then build normally:
```bash
make clean && make
```

Changes to shared utilities automatically propagate to all modules that use them.

## Creating a New Shared Utility

1. **Create directory**: `mkdir src/modules/shared/my_utility`

2. **Implement header** (`my_utility.h`):
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
#include "../shared/my_utility/my_utility.h"
```

4. **Write tests**: Create `test_unit_my_utility.c` in the utility directory

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

*Shared utilities maintain Mimic's architectural principles while eliminating code duplication through simple, explicit relative path includes.*
