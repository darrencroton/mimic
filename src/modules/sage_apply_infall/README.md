# `sage_apply_infall`

Distributes the prepared infall budget into the central hot reservoir during substeps, including metal transport from ejected gas when relevant.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `phase_1`, after `sage_prepare_infall_budget`
- Receives the full FoF workspace and acts on the FoF central

## Properties

- Reads: `Type`, `InfallingGas`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`
- Writes: `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`

## Parameters

None.

## Notes

`InfallingGas` is a snapshot-scoped transport property prepared before substeps. Keep this module before cooling so newly available hot gas can participate in downstream baryonic physics.
