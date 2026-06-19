# `sage_calculate_star_formation`

Calculates the stellar mass formed during the current substep and stores it in a transport field for the apply step.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, before `sage_calculate_supernova_feedback` and `sage_apply_star_formation_supernova`
- Receives one galaxy at a time

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage_apply_star_formation_supernova` must be present somewhere in the pipeline — without it, `NewStellarMass` is computed each substep but never committed to galaxy reservoirs (silent output loss).

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Vvir`, `ColdGas`, `DiskScaleRadius`
- Writes: `NewStellarMass`

## Parameters

- `SfrEfficiency`
- `StarFormingDiskFactor`

## Notes

`NewStellarMass` is a transport field. The persistent stellar reservoir is updated later by `sage_apply_star_formation_supernova`.
