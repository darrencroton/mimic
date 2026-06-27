# Framework Infrastructure (module_system/)

**DO NOT MODIFY** unless adding universal framework features.

## Contents

- **`physical_constants.h`** — Universal physical constants (G, c, Z_sun, etc.)
- **`parameter_helpers.h`** — Helper macros for parameter loading and validation
- **`output_helpers.h`** — Output conversion helpers referenced by property metadata
- **`template/`** — Module template with quick-start README
- **`test_fixture/`** — Infrastructure testing module; provides a stable physics-agnostic module for core tests
- **`test_event_producer/`** — Synthetic event producer for event routing integration tests
- **`test_event_producer_b/`** — Second synthetic producer for multi-producer routing tests
- **`test_event_consumer_alpha/`** — Consumer subscribed to `test_event` from `test_event_producer`
- **`test_event_consumer_beta/`** — Consumer subscribed to `test_event_alt` from `test_event_producer`
- **`test_event_consumer_gamma/`** — Consumer subscribed to `test_event_b` from `test_event_producer_b`
- **`generated/`** — Auto-generated registration code (do not edit)

## Usage

Include constants and helpers in your module:
```c
#include "module_system/physical_constants.h"
#include "module_system/parameter_helpers.h"
```

See [docs/DEVELOPER-GUIDE.md](../../docs/DEVELOPER-GUIDE.md) for usage examples and module development guidance.

## What Belongs Here

**Add to module_system/** only if it is truly universal framework infrastructure:
- Fundamental constants (G, c, h, k_B)
- Solar values (M_sun, L_sun, Z_sun)
- Not model parameters — read those from input YAML
- Not module-specific code — keep that in the module directory
- Not model-specific reusable physics utilities — use `models/<model>/shared/`

## See Also

- [docs/DEVELOPER-GUIDE.md](../../docs/DEVELOPER-GUIDE.md) — Module development guide and metadata schema reference
- [docs/VISION.md](../../docs/VISION.md) — Architectural principles
