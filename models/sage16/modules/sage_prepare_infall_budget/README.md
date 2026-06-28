# `sage_prepare_infall_budget`

Prepares the baryon infall budget for the current FoF system by consolidating satellite material where required and calculating the gas available for infall.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace and writes the central infall budget

## Ordering

`init()` enforces ordering: `sage_reionization` is optional (`HaloBaryonFraction` falls back to `GlobalBaryonFraction` when unset), but when it is configured in `pre_timestep` it must appear before this module. Misordering aborts the run because the infall budget would read `HaloBaryonFraction` before reionization sets it.

## Properties

- Reads: `Type`, `Mvir`, `HaloBaryonFraction`, `StellarMass`, `BlackHoleMass`, `ColdGas`, `HotGas`, `ICS`, `MetalsICS`, `EjectedGas`, `MetalsEjectedGas`
- Writes: `HaloBaryonFraction`, `InfallingGas`, `ICS`, `MetalsICS`, `EjectedGas`, `MetalsEjectedGas`

## Parameters

- `GlobalBaryonFraction`

## Notes

`InfallingGas` is a transport field consumed by `sage_apply_infall` during substeps.
