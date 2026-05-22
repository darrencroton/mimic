# `sage_apply_star_formation_supernova`

Applies star-formation and supernova transport fields to the persistent galaxy reservoirs. This is an infrastructure commit step, not a swappable star-formation or feedback prescription.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `phase_1`, after `sage_star_formation` and usually after `sage_supernova_feedback`
- Receives one galaxy at a time and uses the FoF central for feedback reservoir transfers

## Properties

- Reads: `dT`, `Mvir`, `ColdGas`, `MetalsColdGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`
- Writes: `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `StarFormationRate`, `SupernovaOutflowRate`

## Parameters

- `RecycleFraction`
- `Yield`
- `FracZleaveDisk`

## Notes

This module consumes transport fields and should remain after the modules that calculate them. Disabling it means calculated star formation or feedback budgets are not committed to galaxy reservoirs.
