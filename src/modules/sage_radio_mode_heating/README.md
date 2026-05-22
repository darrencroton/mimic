# `sage_radio_mode_heating`

Suppresses cooling through AGN radio-mode heating. The selected accretion/heating prescription is controlled by `AGNrecipe`.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `phase_1`, after `sage_calculate_cooling_budget` and before `sage_apply_cooling`
- Receives one galaxy at a time

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Mvir`, `Vvir`, `Rvir`, `CoolingGas`, `BlackHoleMass`, `HotGas`, `MetalsHotGas`, `CoolingLambda`, `Rcool`
- Writes: `CoolingGas`, `BlackHoleMass`, `Rheat`, `Heating`

## Parameters

- `RadioModeEfficiency`
- `AGNrecipe`

## Notes

`CoolingGas` remains a transport budget after this module. Place `sage_apply_cooling` after radio-mode heating so it applies the suppressed cooling budget.
