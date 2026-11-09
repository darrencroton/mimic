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
- `AGNrecipe` (0 to 3, the same key `sage_radio_mode_heating` and `sage_quasar_mode` read; loaded only when the post-minor-merger disk-instability recheck is active and `sage_quasar_mode` is configured as `process_per_event`): `0` stops the recheck's black-hole growth and quasar wind, as in SAGE (`sage-model/src/model_disk_instability.c:67-68`), so that run gets no warning about the quasar module; `1` to `3` leave behaviour unchanged. The shipped run files use `2`.

## Ordering

**Enforced at init by event subscription validation (fails with ERROR if violated):**

1. When used as `process_per_event`, `sage_resolve_mergers_and_disruption` must be configured as `process_full_halo` in the same substep phase — it is the only producer of `merger` events that this module consumes.

**Advisory (emits WARNING if violated):**

- `process_by_galaxy` mode: `sage_disk_instability` should precede this module in the same substep phase — without it, `UnstableDiskGasFraction` is zero and the disk-instability starburst channel fires on no galaxies.
- `process_per_event` mode (when the post-merger disk-instability recheck is active): `sage_quasar_mode` as `process_per_event` is recommended in the same substep phase to reproduce SAGE's ordering of BH growth and quasar winds before the burst.

## Events

Consumes `merger` events from `sage_resolve_mergers_and_disruption`.
