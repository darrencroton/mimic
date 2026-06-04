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

## Events

Consumes `merger` events from `sage_resolve_mergers_and_disruption`.
