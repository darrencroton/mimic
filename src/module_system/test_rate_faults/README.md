# `test_rate_faults`

**WARNING: This module is for TESTING INFRASTRUCTURE ONLY. DO NOT USE IN PRODUCTION RUNS.**

The fault injector of the model-neutral rate fixture, beside [`test_rate_supply`](../test_rate_supply/README.md), [`test_rate_formation`](../test_rate_formation/README.md), [`test_rate_feedback`](../test_rate_feedback/README.md) and [`test_rate_stripping`](../test_rate_stripping/README.md); the fixture's state is described in `test_rate_supply`'s README. It declares one sink out of `RateFixtureCold` whose law a parameter selects, so a run can show how the rate phase fails: the failure message, the failure file (`rate_failure_task<N>.txt` in the output directory) and the purity probe (`modules.rate_integration.check_purity`). It also has a full-halo mode, which adds mass to a conserved total without declaring it, so a run can show the discrete-phase ledger refusing an undeclared change.

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

## Full-Halo Mode

Configured as `process_full_halo` in a discrete phase, the module adds `TestRateFaultsAddedMass` to the FoF central's `RateFixtureHot` on every call (nothing when the central row has no galaxy or is Type 3). It declares no `changes_totals`, so when a rate phase is configured the discrete-phase ledger ends the run at the first such call, naming the phase, this module and the total `rate_fixture_mass`. Without a rate phase the ledger does not run and the mass is simply added. The two modes may be configured together, the rate mode in the rate phase and the full-halo mode in a discrete phase.

## Parameters

`TestRateFaultsLaw` and `TestRateFaultsRate` are required under `modules.parameters` whenever the module is configured; `init()` refuses a missing or invalid value. `TestRateFaultsAddedMass` is optional, but the full-halo mode fails its first call (the module returns non-zero) when it is absent.

| Parameter | Meaning | Accepted values |
|---|---|---|
| `TestRateFaultsLaw` | The law the sink uses | `well_behaved`, `diverging`, `not_a_number`, `negative`, `impure` |
| `TestRateFaultsRate` | The law's rate scale | Finite, `> 0` |
| `TestRateFaultsAddedMass` | Mass the full-halo mode adds to the central's `RateFixtureHot` per call | Finite, `> 0` |

## Build Membership

Like the other `src/module_system/test_*` fixtures, it is compiled and registered only in test builds: the `Makefile` adds its source to the `TEST_BUILD=yes` executable, and the generated `tests/generated/module_sources.txt` adds it to the unit-test runner. Production registration and the production executable exclude it.

## Tests

- `tests/integration/test_rate_phase.py` — the failing laws with their failure files, the impure law under the purity switch, the switch's value check, and the full-halo mode's undeclared change, alone and beside `test_rate_jump` in one phase. No other test names this module.

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
