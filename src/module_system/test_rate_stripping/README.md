# `test_rate_stripping`

**WARNING: This module is for TESTING INFRASTRUCTURE ONLY. DO NOT USE IN PRODUCTION RUNS.**

One of the four `process_rates` modules of the model-neutral rate fixture, with [`test_rate_supply`](../test_rate_supply/README.md), [`test_rate_formation`](../test_rate_formation/README.md) and [`test_rate_feedback`](../test_rate_feedback/README.md); the fixture's state is described in `test_rate_supply`'s README. This module strips a satellite's hot reservoir onto the FoF central's hot reservoir, the fixture's only cross-record transfer, and ejects cold out of every row through a sink.

## Transfers and Laws

The law returns one coefficient per transfer, in declaration order (the slots `TEST_RATE_STRIPPING_TRANSFER_*` in the generated `rate_contracts.h`). Rates are in Mimic internal units; `Type` is the row's halo field.

| Slot | Transfer | Kind | Coefficient |
|---|---|---|---|
| 0 | `stripping`: `RateFixtureHot` to `RateFixtureHot`, `to_record: central` | Internal (specific rate) | `TestRateStrippingRate` when `Type != 0`, else 0 |
| 1 | `ejection`: `RateFixtureCold` to `external` | Sink (specific rate) | `TestRateStrippingEjectionRate` |

`to_record: central` puts the recipient on the FoF central's record. On the central itself the transfer would move hot onto its own hot reservoir and contribute nothing, and the law returns 0 there. A transfer whose two ends are one reservoir is legal only in this `to_record: central` form; with `to_record: self` the metadata is refused.

## Parameters

Both are required under `modules.parameters` whenever the module is configured; `init()` refuses a missing or invalid value.

| Parameter | Meaning | Accepted values |
|---|---|---|
| `TestRateStrippingRate` | Specific rate at which a satellite's hot reservoir moves onto the central's | Finite, `>= 0` |
| `TestRateStrippingEjectionRate` | Specific rate at which cold leaves the row | Finite, `>= 0` |

## Build Membership

Like the other `src/module_system/test_*` fixtures, it is compiled and registered only in test builds: the `Makefile` adds its source to the `TEST_BUILD=yes` executable, and the generated `tests/generated/module_sources.txt` adds it to the unit-test runner. Production registration and the production executable exclude it. Test builds dispatch the rate mode: the law runs in the rate phase through the core integrator, and unit tests also call it directly.

## Tests

- `tests/unit/test_rate_transfer_tables.c` — the generated transfer table and the law against a table of hand-built rows.
- `tests/integration/test_rate_metadata.py` — the `rates:` rules shared by the generator and the validator.

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
