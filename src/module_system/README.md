# Framework Infrastructure (module_system/)

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
#include "module_system/physical_constants.h"
#include "module_system/parameter_helpers.h"
```

See [docs/DEVELOPER-GUIDE.md](../../docs/DEVELOPER-GUIDE.md) for usage examples and module development guidance.

## What Belongs Here

**Add to module_system/** only if it's truly universal framework infrastructure:
- Fundamental constants (G, c, h, k_B)
- Solar values (M_sun, L_sun, Z_sun)
- Not model parameters; read those from input YAML
- Not module-specific code; keep that in your module directory
- Not model-specific reusable physics utilities; use `models/<model>/shared/`

## See Also

- [docs/DEVELOPER-GUIDE.md](../../docs/DEVELOPER-GUIDE.md#module-communication) - Model-local shared utility guidance
- [docs/DEVELOPER-GUIDE.md](../../docs/DEVELOPER-GUIDE.md) - Module development guide
- [docs/VISION.md](../../docs/VISION.md) - Architectural principles
