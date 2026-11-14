# `test_rate_feedback`

**WARNING: This module is for TESTING INFRASTRUCTURE ONLY. DO NOT USE IN PRODUCTION RUNS.**

One of the four `process_rates` modules of the model-neutral rate fixture, with [`test_rate_supply`](../test_rate_supply/README.md), [`test_rate_formation`](../test_rate_formation/README.md) and [`test_rate_stripping`](../test_rate_stripping/README.md); the fixture's state is described in `test_rate_supply`'s README. This module reheats cold into hot and returns a yield into the cold tracer as a slaved source, whose master, `test_rate_formation.star_formation`, another module declares.

## Transfers and Laws

The law returns one coefficient per transfer, in declaration order (the slots `TEST_RATE_FEEDBACK_TRANSFER_*` in the generated `rate_contracts.h`). Rates are in Mimic internal units.

| Slot | Transfer | Kind | Coefficient |
|---|---|---|---|
| 0 | `reheating`: `RateFixtureCold` to `RateFixtureHot` | Internal (specific rate) | `TestRateFeedbackReheatRate` |
| 1 | `yield`: `external` to `RateFixtureColdTracer`, slaved to `test_rate_formation.star_formation` | Slaved source (dimensionless) | `TestRateFeedbackYield` |

The `yield` coefficient is dimensionless: the source's flux is the coefficient times the master's flux, so the cold tracer gains `TestRateFeedbackYield` per unit mass formed into stars. The master's donor, `RateFixtureCold`, is in total `rate_fixture_mass` and the recipient in `rate_fixture_tracer`, so the slaved source makes the tracer total depend on the mass total, which is the order the tracer parents already impose. Neither coefficient depends on the row's state. A yield is a source into a tracer, which can overtake its parent once the parent's pristine part is nearly exhausted; this constant yield does not vanish as that happens, so a run at high enough rates can end with the cause `tracer_bound`, by design (see the tracer bound in the [Rate Callback Contract](../../../docs/DEVELOPER-GUIDE.md#rate-callback-contract)).

## Parameters

Both are required under `modules.parameters` whenever the module is configured; `init()` refuses a missing or invalid value.

| Parameter | Meaning | Accepted values |
|---|---|---|
| `TestRateFeedbackReheatRate` | Specific reheating rate from cold to hot | Finite, `>= 0` |
| `TestRateFeedbackYield` | Tracer yield per unit mass formed into stars | `[0, 1)` |

## Build Membership

Like the other `src/module_system/test_*` fixtures, it is compiled and registered only in test builds: the `Makefile` adds its source to the `TEST_BUILD=yes` executable, and the generated `tests/generated/module_sources.txt` adds it to the unit-test runner. Production registration and the production executable exclude it. Test builds dispatch the rate mode: the law runs in the rate phase through the core integrator, and unit tests also call it directly.

## Tests

- `tests/unit/test_rate_transfer_tables.c` — the generated transfer table, its slaved master resolved across modules, and the law against a table of hand-built rows.
- `tests/integration/test_rate_metadata.py` — the `rates:` rules shared by the generator and the validator.

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
