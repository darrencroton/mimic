# `sage_quasar_mode`

Models black-hole growth and quasar-mode feedback from disk-instability triggers and merger events.

## Processing Contract

- Supported modes: `process_by_galaxy`, `process_per_event`
- `process_by_galaxy` path consumes disk-instability triggers in `galaxy_physics`
- `process_per_event` path consumes merger events in `satellite_mergers`

## Properties

- Reads: `ColdGas`, `MetalsColdGas`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `UnstableDiskGasFraction`, `Vvir`
- Writes: `BlackHoleMass`, `QuasarModeBHaccretionMass`, `ColdGas`, `MetalsColdGas`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`

## Parameters

- `BlackHoleGrowthRate`
- `QuasarModeEfficiency`

## Ordering

**Enforced at init by event subscription validation (fails with ERROR if violated):**

1. When used as `process_per_event`, `sage_resolve_mergers_and_disruption` must be configured as `process_full_halo` in the same substep phase — it is the only producer of `merger` events that this module consumes.

## Events

Consumes `merger` events from `sage_resolve_mergers_and_disruption`.
