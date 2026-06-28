# `sage_radio_mode_heating`

Suppresses cooling through AGN radio-mode heating. The selected accretion/heating prescription is controlled by `AGNrecipe`.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage_calculate_cooling_budget` and before `sage_apply_cooling`
- Receives one galaxy at a time

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Mvir`, `Vvir`, `Rvir`, `CoolingGas`, `BlackHoleMass`, `HotGas`, `MetalsHotGas`, `CoolingLambda`, `Rheat`, `Rcool`
- Writes: `CoolingGas`, `BlackHoleMass`, `HotGas`, `MetalsHotGas`, `Rheat`, `Heating`

## Parameters

- `RadioModeEfficiency`
- `AGNrecipe`

## Ordering

**Advisory (no `init()` enforcement):**

1. Run after `sage_calculate_cooling_budget` so `CoolingGas`, `CoolingLambda`, `Rcool`, and the previous `Rheat` are available.
2. Run before `sage_apply_cooling` so the apply step commits the suppressed cooling budget rather than the raw cooling budget.

## Notes

`CoolingGas` remains a transport budget after this module.
