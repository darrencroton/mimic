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

## Ordering

**Enforced at init by event subscription validation (fails with ERROR if violated):**

1. When used as `process_per_event`, `sage_resolve_mergers_and_disruption` must be configured as `process_full_halo` in the same substep phase — it is the only producer of `merger` events that this module consumes.

**Advisory (emits WARNING if violated):**

- `process_by_galaxy` mode: `sage_disk_instability` should precede this module in the same substep phase — without it, `UnstableDiskGasFraction` is zero and the disk-instability starburst channel fires on no galaxies.
- `process_per_event` mode (when the post-merger disk-instability recheck is active): `sage_quasar_mode` as `process_per_event` is recommended in the same substep phase to reproduce SAGE's ordering of BH growth and quasar winds before the burst.

## Events

Consumes `merger` events from `sage_resolve_mergers_and_disruption`.
