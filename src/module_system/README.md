# Framework Infrastructure (module_system/)

**DO NOT MODIFY** unless adding universal framework features.

## Contents

- **`physical_constants.h`** — Universal physical constants (G, c, Z_sun, etc.)
- **`parameter_helpers.h`** — Helper macros for parameter loading and validation
- **`output_helpers.h`** — Output conversion helpers referenced by property metadata
- **`template/`** — Module template with quick-start README and a commented `rates:` / `changes_totals` block for rate modules
- **`test_fixture/`** — Infrastructure testing module; provides a stable physics-agnostic module for core tests (dual-mode: FoF modes plus `process_snapshot`)
- **`test_snapshot_fixture/`** — Snapshot-only test module (`process_snapshot` alone) for the `modules.post_snapshot` phase tests; it deliberately probes `module_emit_event` during its callback, and the core registry logs one expected ERROR line per snapshot (`module_emit_event called during post_snapshot dispatch`) when it rejects the probe; the fixture itself logs only its INFO marker
- **`test_event_producer/`** — Synthetic event producer for event routing integration tests
- **`test_event_producer_b/`** — Second synthetic producer for multi-producer routing tests
- **`test_event_consumer_alpha/`** — Consumer subscribed to `test_event` from `test_event_producer`
- **`test_event_consumer_beta/`** — Consumer subscribed to `test_event_alt` from `test_event_producer`
- **`test_event_consumer_gamma/`** — Consumer subscribed to `test_event_b` from `test_event_producer_b`
- **`test_rate_supply/`** — Rate fixture (`process_rates`): sources into the hot reservoir and its tracer on central rows, and cooling from hot to cold
- **`test_rate_formation/`** — Rate fixture (`process_rates`): threshold star formation from cold to stars, observed by `RateFixtureFormed`
- **`test_rate_feedback/`** — Rate fixture (`process_rates`): reheating from cold to hot, and a yield into the cold tracer slaved to `test_rate_formation.star_formation`
- **`test_rate_stripping/`** — Rate fixture (`process_rates`): stripping of a satellite's hot reservoir onto the FoF central's (`to_record: central`), and an ejection sink from cold
- **`test_rate_faults/`** — Rate fixture (`process_rates`, `process_full_halo`): a sink from cold whose law a parameter selects (well behaved, diverging, NaN, negative or impure), for the failure-file and purity-probe tests; in its full-halo mode it adds mass to the central's hot reservoir without declaring the total, for the discrete-phase ledger's refusal
- **`test_rate_jump/`** — Rate fixture (`process_full_halo`): merges orphan satellites whose stars exceed a threshold into the FoF central, optionally destroying a fraction, and declares both fixture totals in `changes_totals`, for the discrete-phase ledger
- **`generated/`** — Auto-generated registration code (do not edit), including each rate module's transfer tables and `rate_contracts.h`

The `test_rate_*` fixtures, with `test_fixture`'s `RateFixture*` properties, are compiled and registered in test builds only (`make TEST_BUILD=yes`); a production build contains no rate module. A test build merges seven `double` `RateFixture*` output columns (three reservoirs, three tracers and one observer; zero unless a rate phase runs or `test_fixture`'s `TestFixtureCreatedContent` is set) into every package, beside `TestDummyProperty`. They are the working examples of the [rate callback contract](../../docs/DEVELOPER-GUIDE.md#rate-callback-contract), and the only rate modules in the tree.

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
