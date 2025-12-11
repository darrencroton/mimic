# Framework Infrastructure (_system/)

**DO NOT MODIFY** unless adding universal framework features.

## Contents

- **`physical_constants.h`** - Universal physical constants (G, c, Z_sun, etc.)
- **`parameter_helpers.h`** - Helper macros for parameter loading/validation
- **`output_helpers.h`** - Output formatting macros
- **`template/`** - Module template with step-by-step README
- **`test_fixture/`** - Infrastructure testing module (see README for purpose)
- **`generated/`** - Auto-generated registration code (do not edit)

## Usage

Include constants and helpers in your module:
```c
#include "../_system/physical_constants.h"
#include "../_system/parameter_helpers.h"
```

See `docs/DEVELOPER-GUIDE.md` for usage examples and module development guide.

## What Belongs Here

**Add to _system/** only if it's truly universal framework infrastructure:
- ✅ Fundamental constants (G, c, h, k_B)
- ✅ Solar values (M_sun, L_sun, Z_sun)
- ❌ Model parameters → read from input YAML
- ❌ Module-specific code → keep in your module directory
- ❌ Reusable physics utilities → use `_shared/` directory

## See Also

- `_shared/` - Reusable physics utilities
- `docs/DEVELOPER-GUIDE.md` - Module development guide
- `docs/VISION.md` - Architectural principles
