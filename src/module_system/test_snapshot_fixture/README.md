# `test_snapshot_fixture`

**WARNING: This module is for TESTING INFRASTRUCTURE ONLY. DO NOT USE IN PRODUCTION RUNS.**

Infrastructure-only module that advertises only the snapshot callback family. It gives tests a real snapshot-only module, alongside the dual-mode [`test_fixture`](../test_fixture/README.md) and the FoF-only event fixtures, without coupling infrastructure tests to production physics.

## Contract

- Supported mode: `process_snapshot` only. Generated registration binds `test_snapshot_fixture_process_snapshot` and sets `.process = NULL`; the module has no FoF `process()` function.
- `process_snapshot()` checks the borrowed-view contract (non-NULL context, non-negative count, non-NULL population and galaxy pointers for a positive count), then sets `TestDummyProperty = 0.5` on every entry through `halos[i].galaxy`.
- Each call logs one `TEST_SNAPSHOT_FIXTURE_EXEC: call=<n> snapshot=<s> count=<c> z=<z>` line; cleanup logs `TEST_SNAPSHOT_FIXTURE_CLEANUP: total_calls=<n>`.
- It declares no events and reads no parameters.

The callback contract itself (borrowed storage, call-limited lifetime, permitted writes and the three patterns that compile silently but are forbidden) is documented on `struct Module.process_snapshot` in `src/core/module_interface.h` and in the [Developer Guide](../../../docs/DEVELOPER-GUIDE.md#snapshot-callback-contract).

## Build Membership

Like the other `src/module_system/test_*` fixtures, it is compiled and registered only in test builds: the `Makefile` adds its source to the `TEST_BUILD=yes` executable, and the generated `tests/generated/module_sources.txt` adds it to the unit-test runner. Production registration and the production executable exclude it.

## Tests

- `tests/unit/test_snapshot_module_contract.c` — registration matrix and real callback invocations.
- `tests/integration/test_snapshot_module_schema.py` — generator/validator agreement and generated registration.

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
