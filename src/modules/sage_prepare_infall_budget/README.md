# `sage_prepare_infall_budget`

Prepares the baryon infall budget for the current FoF system by consolidating satellite material where required and calculating the gas available for infall.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace and writes the central infall budget

## Properties

- Reads: `Type`, `Mvir`, `HaloBaryonFraction`, `StellarMass`, `BlackHoleMass`, `ColdGas`, `HotGas`, `ICS`, `EjectedGas`
- Writes: `InfallingGas`, `HotGas`, `MetalsHotGas`, `ICS`, `MetalsICS`, `EjectedGas`, `MetalsEjectedGas`

## Parameters

- `GlobalBaryonFraction`

## Notes

`InfallingGas` is a transport field consumed by `sage_apply_infall` during substeps.
