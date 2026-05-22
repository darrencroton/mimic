# `sage_star_formation`

Calculates the stellar mass formed during the current substep and stores it in a transport field for the apply step.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `phase_1`, before `sage_supernova_feedback` and `sage_apply_star_formation_supernova`
- Receives one galaxy at a time

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Vvir`, `ColdGas`, `DiskScaleRadius`
- Writes: `NewStellarMass`

## Parameters

- `SfrEfficiency`
- `StarFormingDiskFactor`

## Notes

`NewStellarMass` is a transport field. The persistent stellar reservoir is updated later by `sage_apply_star_formation_supernova`.
