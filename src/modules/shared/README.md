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

4. **Write tests**: Create `test_unit_my_utility.c` in `shared/` directory

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
