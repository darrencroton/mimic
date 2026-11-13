# `test_rate_faults`

**WARNING: This module is for TESTING INFRASTRUCTURE ONLY. DO NOT USE IN PRODUCTION RUNS.**

The fault injector of the model-neutral rate fixture, beside [`test_rate_supply`](../test_rate_supply/README.md), [`test_rate_formation`](../test_rate_formation/README.md), [`test_rate_feedback`](../test_rate_feedback/README.md) and [`test_rate_stripping`](../test_rate_stripping/README.md); the fixture's state is described in `test_rate_supply`'s README. It declares one sink out of `RateFixtureCold` whose law a parameter selects, so a run can show how the rate phase fails: the failure message, the failure file (`rate_failure_task<N>.txt` in the output directory) and the purity probe (`modules.rate_integration.check_purity`).

## Transfers and Laws

The law returns one coefficient (the slot `TEST_RATE_FAULTS_TRANSFER_FAULT` in the generated `rate_contracts.h`).

| Slot | Transfer | Kind | Coefficient |
|---|---|---|---|
| 0 | `fault`: `RateFixtureCold` to `external` | Sink (specific rate) | The law `TestRateFaultsLaw` selects, below |

| `TestRateFaultsLaw` | Coefficient | What a run does |
|---|---|---|
| `well_behaved` | `TestRateFaultsRate` | Completes: a constant specific rate |
| `diverging` | `TestRateFaultsRate / RateFixtureCold` (0 on an empty donor) | The sink's rate grows without bound as the donor empties, so the step shrinks until the segment fails |
| `not_a_number` | NaN | The segment fails with cause `bad_coefficient` |
| `negative` | -1 | The segment fails with cause `bad_coefficient` |
| `impure` | `TestRateFaultsRate` plus 0.001 times the number of calls since `init()` | Completes unless `check_purity` is `true`, which fails the run naming this module and `fault`: a static counter is state a law may not read |

## Parameters

Both are required under `modules.parameters` whenever the module is configured; `init()` refuses a missing or invalid value.

| Parameter | Meaning | Accepted values |
|---|---|---|
| `TestRateFaultsLaw` | The law the sink uses | `well_behaved`, `diverging`, `not_a_number`, `negative`, `impure` |
| `TestRateFaultsRate` | The law's rate scale | Finite, `> 0` |

## Build Membership

Like the other `src/module_system/test_*` fixtures, it is compiled and registered only in test builds: the `Makefile` adds its source to the `TEST_BUILD=yes` executable, and the generated `tests/generated/module_sources.txt` adds it to the unit-test runner. Production registration and the production executable exclude it.

## Tests

- `tests/integration/test_rate_phase.py` — the failing laws with their failure files, the impure law under the purity switch, and the switch's value check.
- `tests/unit/test_rate_transfer_tables.c` — the generated transfer table and the law.
- `tests/integration/test_rate_metadata.py` — the `rates:` rules shared by the generator and the validator.

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
