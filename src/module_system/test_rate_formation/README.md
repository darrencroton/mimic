# `test_rate_formation`

**WARNING: This module is for TESTING INFRASTRUCTURE ONLY. DO NOT USE IN PRODUCTION RUNS.**

One of the four `process_rates` modules of the model-neutral rate fixture, with [`test_rate_supply`](../test_rate_supply/README.md), [`test_rate_feedback`](../test_rate_feedback/README.md) and [`test_rate_stripping`](../test_rate_stripping/README.md); the fixture's state is described in `test_rate_supply`'s README. This module forms stars from the cold reservoir with a threshold law and observes the amount formed.

## Transfers and Laws

The law returns one coefficient per transfer, in declaration order (the slot `TEST_RATE_FORMATION_TRANSFER_STAR_FORMATION` in the generated `rate_contracts.h`). Rates are in Mimic internal units; `Cold` is the row's `RateFixtureCold` at the trial state.

| Slot | Transfer | Kind | Coefficient |
|---|---|---|---|
| 0 | `star_formation`: `RateFixtureCold` to `RateFixtureStars` | Internal (specific rate) | `((Cold - TestRateFormationThreshold) / Cold) * TestRateFormationEfficiency` when `Cold > TestRateFormationThreshold`, else 0 |

The flux, the coefficient times `Cold`, is `TestRateFormationEfficiency * max(0, Cold - TestRateFormationThreshold)`: zero at and below the threshold and continuous across it, with a kink at the threshold. `Cold` above a non-negative threshold is positive, so the coefficient never divides by zero. The ratio `(Cold - TestRateFormationThreshold) / Cold` lies in (0, 1] and is formed before the efficiency multiplies it, so any finite efficiency gives a finite coefficient (an efficiency of `1e308` with `Cold = 2` and threshold 1 gives `5e307`).

## Observers

| Property | Transfer | Accumulates |
|---|---|---|
| `RateFixtureFormed` | `star_formation` | The committed amount `star_formation` moves |

## Parameters

Both are required under `modules.parameters` whenever the module is configured; `init()` refuses a missing or invalid value.

| Parameter | Meaning | Accepted values |
|---|---|---|
| `TestRateFormationEfficiency` | Formation rate per unit cold content above the threshold, per unit time | Finite, `>= 0` |
| `TestRateFormationThreshold` | Cold content at and below which no stars form, in `RateFixtureCold`'s units | Finite, `>= 0` |

## Build Membership

Like the other `src/module_system/test_*` fixtures, it is compiled and registered only in test builds: the `Makefile` adds its source to the `TEST_BUILD=yes` executable, and the generated `tests/generated/module_sources.txt` adds it to the unit-test runner. Production registration and the production executable exclude it. The rate mode is declared but not yet dispatched (every phase refuses `process_rates` at startup), so today only unit tests call its law.

## Tests

- `tests/unit/test_rate_transfer_tables.c` — the generated transfer and observer tables and the law against a table of hand-built rows.
- `tests/integration/test_rate_metadata.py` — the `rates:` rules shared by the generator and the validator.

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
