# `test_rate_supply`

**WARNING: This module is for TESTING INFRASTRUCTURE ONLY. DO NOT USE IN PRODUCTION RUNS.**

One of the four `process_rates` modules of the model-neutral rate fixture, with [`test_rate_formation`](../test_rate_formation/README.md), [`test_rate_feedback`](../test_rate_feedback/README.md) and [`test_rate_stripping`](../test_rate_stripping/README.md). Their state is the test-only `RateFixture*` properties in [`test_fixture/test_properties.yaml`](../test_fixture/test_properties.yaml): the reservoirs `RateFixtureHot`, `RateFixtureCold` and `RateFixtureStars` (total `rate_fixture_mass`), one tracer of each (total `rate_fixture_tracer`) and the observer `RateFixtureFormed`. This module feeds the hot reservoir from outside on central rows, feeds the hot tracer from outside alongside it, and cools hot into cold.

## Transfers and Laws

The law returns one coefficient per transfer, in declaration order (the slots `TEST_RATE_SUPPLY_TRANSFER_*` in the generated `rate_contracts.h`). Rates are in Mimic internal units; `Type` and `Mvir` are the row's halo fields.

| Slot | Transfer | Kind | Coefficient |
|---|---|---|---|
| 0 | `infall`: `external` to `RateFixtureHot` | Source (absolute rate) | `TestRateSupplyInfallRate * Mvir` when `Type == 0` and `Mvir > 0`, else 0 |
| 1 | `infall_tracer`: `external` to `RateFixtureHotTracer` | Source (absolute rate) | `TestRateSupplyTracerFraction` times the `infall` coefficient |
| 2 | `cooling`: `RateFixtureHot` to `RateFixtureCold` | Internal (specific rate) | `TestRateSupplyCoolingRate` |

A satellite or orphan row receives no infall, and so no tracer infall either; cooling acts on every row.

## Parameters

All three are required under `modules.parameters` whenever the module is configured; `init()` refuses a missing or invalid value.

| Parameter | Meaning | Accepted values |
|---|---|---|
| `TestRateSupplyInfallRate` | Infall per unit virial mass on a central row, per unit time | Finite, `>= 0` |
| `TestRateSupplyTracerFraction` | Tracer infall as a fraction of infall | `[0, 1)` |
| `TestRateSupplyCoolingRate` | Specific cooling rate from hot to cold | Finite, `>= 0` |

## Build Membership

Like the other `src/module_system/test_*` fixtures, it is compiled and registered only in test builds: the `Makefile` adds its source to the `TEST_BUILD=yes` executable, and the generated `tests/generated/module_sources.txt` adds it to the unit-test runner. Production registration and the production executable exclude it. The rate mode is declared but not yet dispatched (every phase refuses `process_rates` at startup), so today only unit tests call its law.

## Tests

- `tests/unit/test_rate_transfer_tables.c` — the generated transfer table and the law against a table of hand-built rows.
- `tests/integration/test_rate_metadata.py` — the `rates:` rules shared by the generator and the validator.

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
