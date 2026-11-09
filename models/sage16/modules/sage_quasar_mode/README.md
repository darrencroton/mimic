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
- `AGNrecipe` (0 to 3, the same key `sage_radio_mode_heating` reads): `0` stops black-hole growth, and with it the quasar wind, on every merger event and every disk instability, as in SAGE (`sage-model/src/model_mergers.c:75-76`, `sage-model/src/model_disk_instability.c:67-68`); `1` to `3` leave this module's behaviour unchanged. A run file that configures this module must set it; the shipped run files use `2`.

## Ordering

**Enforced at init by event subscription validation (fails with ERROR if violated):**

1. When used as `process_per_event`, `sage_resolve_mergers_and_disruption` must be configured as `process_full_halo` in the same substep phase — it is the only producer of `merger` events that this module consumes.
2. When used as `process_by_galaxy` with `sage_disk_instability` configured, that module must precede this one in the same substep phase (checked by `sage_disk_instability`'s `init()`), so the instability trigger read here is this substep's.

## Events

Consumes `merger` events from `sage_resolve_mergers_and_disruption`.
