# `test_rate_jump`

**WARNING: This module is for TESTING INFRASTRUCTURE ONLY. DO NOT USE IN PRODUCTION RUNS.**

The discrete jump of the model-neutral rate fixture, beside the rate modules [`test_rate_supply`](../test_rate_supply/README.md), [`test_rate_formation`](../test_rate_formation/README.md), [`test_rate_feedback`](../test_rate_feedback/README.md), [`test_rate_stripping`](../test_rate_stripping/README.md) and [`test_rate_faults`](../test_rate_faults/README.md); the fixture's state is described in `test_rate_supply`'s README. It is a `process_full_halo` module that merges orphan satellites into the FoF central between rate segments, so a run can show the discrete-phase ledger: a lossless merge leaves every conserved total unchanged, and a lossy one changes both totals under a declaration.

## Behaviour

On every call, each orphan satellite row (`Type` 2, with a galaxy) whose `RateFixtureStars` exceeds `TestRateJumpStarsThreshold` is merged into the FoF central (the context's `central_index` row; nothing happens when that row has no galaxy or is `Type` 3):

- each of `RateFixtureHot`, `RateFixtureCold`, `RateFixtureStars` and their three tracers is added to the central's same property, multiplied by `1 - TestRateJumpLossFraction`, and set to zero on the satellite;
- the satellite row is retired (`Type` 3), so it leaves the active rows and the output.

A `Type` 1 satellite is never merged: it hosts its subhalo's orphans, and the next snapshot's inheritance needs every subhalo slice to keep a `Type` 0 or 1 row. The observer `RateFixtureFormed` is not moved. `cleanup()` logs `Test rate jump fixture: <N> satellite(s) merged`, the count since `init()` on this task.

## Declared Total Changes

The module lists both of the fixture's conserved totals, `rate_fixture_mass` and `rate_fixture_tracer`, in `changes_totals`. With a loss fraction of zero a merge moves content between active rows and changes no total beyond rounding, so the discrete-phase ledger records nothing; with a positive loss each merge removes that fraction of the moved content from both totals, and the ledger counts the change against each total in `RunProperties/RateIntegration/DeclaredDiscreteChanges` instead of ending the run. The ledger runs only when a rate phase is configured.

## Parameters

Both are required under `modules.parameters` whenever the module is configured; `init()` refuses a missing or invalid value.

| Parameter | Meaning | Accepted values |
|---|---|---|
| `TestRateJumpStarsThreshold` | A satellite merges when its `RateFixtureStars` exceeds this | Finite, `>= 0` |
| `TestRateJumpLossFraction` | Fraction of every moved reservoir and tracer the merge destroys | `[0, 1)` |

## Build Membership

Like the other `src/module_system/test_*` fixtures, it is compiled and registered only in test builds: the `Makefile` adds its source to the `TEST_BUILD=yes` executable, and the generated `tests/generated/module_sources.txt` adds it to the unit-test runner. Production registration and the production executable exclude it.

## Tests

- `tests/integration/test_rate_phase.py` — the lossless merge (no change recorded), the lossy merge (a non-zero declared-change count), and this module beside `test_rate_faults`'s full-halo mode in one phase.
- `tests/integration/test_rate_provenance.py` — the declared-change counts in `RunProperties/RateIntegration`.

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
