# `sage26_apply_infall`

Distributes the prepared infall budget into the central hot reservoir during substeps, including metal transport from ejected gas when relevant.

## SAGE26 Source

A port of SAGE26's `add_infall_to_hot`, non-CGM branch (`src/model_infall.c:351-369`, `:400-413`), called with the per-substep share `infallingGas / effective_steps` (`src/core_build_model.c:488`). A negative share is taken from the ejected reservoir first, with a float metallicity, then from the hot reservoir; a positive share is added to the hot gas. The arithmetic already matched SAGE26 and was not changed.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `galaxy_physics`, after `sage26_prepare_infall_budget`
- Receives the full FoF workspace and acts on the FoF central

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_prepare_infall_budget` must be in `pre_timestep` as `process_full_halo` — `InfallingGas` is zero without it and no gas is added to the hot reservoir.

## Properties

- Reads: `Type`, `InfallingGas`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`
- Writes: `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`

## Parameters

None.

## Tests

`_tests/test_unit_sage26_apply_infall_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.

## Notes

`InfallingGas` is a snapshot-scoped transport property prepared before substeps. Keep this module before cooling so newly available hot gas can participate in downstream baryonic physics.
