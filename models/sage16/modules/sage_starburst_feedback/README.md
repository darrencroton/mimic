# `sage_starburst_feedback`

Applies collisional starburst physics from disk-instability triggers and merger events, including associated feedback and reservoir updates.

## Processing Contract

- Supported modes: `process_by_galaxy`, `process_per_event`
- `process_by_galaxy` path consumes disk-instability triggers in `galaxy_physics`
- `process_per_event` path consumes merger events in `satellite_mergers`

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Mvir`, `Vvir`, `ColdGas`, `MetalsColdGas`, `StellarMass`, `BulgeMass`, `HotGas`, `EjectedGas`, `BlackHoleMass`, `DiskScaleRadius`, `UnstableDiskGasFraction`
- Writes: `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, `BulgeMass`, `MetalsBulgeMass`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `StarFormationRate`, `SupernovaOutflowRate`

## Parameters

- `FeedbackReheatingEpsilon`
- `FeedbackEjectionEfficiency`
- `RecycleFraction`
- `Yield`
- `FracZleaveDisk`
- `StarFormingDiskFactor`
- `BlackHoleGrowthRate`
- `QuasarModeEfficiency`
- `ThresholdMajorMerger`

## Events

Consumes `merger` events from `sage_resolve_mergers_and_disruption`.
