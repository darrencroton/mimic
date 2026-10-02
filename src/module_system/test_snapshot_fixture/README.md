# `test_snapshot_fixture`

**WARNING: This module is for TESTING INFRASTRUCTURE ONLY. DO NOT USE IN PRODUCTION RUNS.**

Infrastructure-only module that advertises only the snapshot callback family. It gives tests a real snapshot-only module, alongside the dual-mode [`test_fixture`](../test_fixture/README.md) and the FoF-only event fixtures, without coupling infrastructure tests to production physics.

## Contract

- Supported mode: `process_snapshot` only. Generated registration binds `test_snapshot_fixture_process_snapshot` and sets `.process = NULL`; the module has no FoF `process()` function.
- It is configured under `modules.post_snapshot` (horizontal runs only):

  ```yaml
  modules:
    post_snapshot:
      - test_snapshot_fixture: process_snapshot
  ```

- `process_snapshot()` checks the borrowed-view contract (non-NULL context, non-negative count, non-NULL population and galaxy pointers for a positive count) and that snapshot dispatch rejects FoF event emission: it calls `module_emit_event()` with a non-NULL local `struct ModuleContext` and fails the call (returns `-1`) unless that returns `-1`. Outside `post_snapshot` dispatch the emitter's direct-unit-test shortcut accepts the event, so a direct call of the callback is refused by design; tests dispatch it through `execute_post_snapshot()`.
- It then sets `TestDummyProperty = 0.5` on every entry through `halos[i].galaxy`.
- Each call logs one `TEST_SNAPSHOT_FIXTURE_EXEC: call=<n> snapshot=<s> count=<c> z=<z> time=<t> types=<n0>/<n1>/<n2> other_types=<n> foreign_snapnum=<n> seen_zero=<n> seen_max=<v> id_sum=<sum> emit=rejected` line, describing the population as it arrived: the context's redshift and lookback time, the Type 0/1/2 counts, entries of any other Type, entries whose `SnapNum` is not the context's snapshot, entries still at `TestDummyProperty`'s initial 0, the largest `TestDummyProperty` found, and the sum of `UniqueGalaxyID` modulo 2^64. `tests/integration/test_snapshot_phase.py` parses this format; keep the two in sync. Cleanup logs `TEST_SNAPSHOT_FIXTURE_CLEANUP: total_calls=<n>`.
- The rejected emission is logged by the core as an `ERROR` line on every call; that line is expected in fixture runs.
- It declares no events and reads no parameters.

The callback contract itself (borrowed storage, call-limited lifetime, permitted writes and the three patterns that compile silently but are forbidden) is documented on `struct Module.process_snapshot` in `src/core/module_interface.h` and in the [Developer Guide](../../../docs/DEVELOPER-GUIDE.md#snapshot-callback-contract).

## Build Membership

Like the other `src/module_system/test_*` fixtures, it is compiled and registered only in test builds: the `Makefile` adds its source to the `TEST_BUILD=yes` executable, and the generated `tests/generated/module_sources.txt` adds it to the unit-test runner. Production registration and the production executable exclude it.

## Tests

- `tests/unit/test_snapshot_module_contract.c` — registration matrix, real callback invocations, and `post_snapshot` lifecycle, validation and dispatch.
- `tests/integration/test_snapshot_phase.py` — `modules.post_snapshot` configuration and horizontal execution on the committed fixtures.
- `tests/integration/test_snapshot_module_schema.py` — generator/validator agreement and generated registration.

## Production Use

Do not use this module in production parameter files, scientific runs, or benchmarks.
