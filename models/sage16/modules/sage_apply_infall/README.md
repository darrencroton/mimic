# `sage_apply_infall`

Distributes the prepared infall budget into the central hot reservoir during substeps, including metal transport from ejected gas when relevant.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `galaxy_physics`, after `sage_prepare_infall_budget`
- Receives the full FoF workspace and acts on the FoF central

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage_prepare_infall_budget` must be in `pre_timestep` as `process_full_halo` — `InfallingGas` is zero without it and no gas is added to the hot reservoir.

## Properties

- Reads: `Type`, `InfallingGas`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`
- Writes: `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`

## Parameters

None.

## Notes

`InfallingGas` is a snapshot-scoped transport property prepared before substeps. Keep this module before cooling so newly available hot gas can participate in downstream baryonic physics.
