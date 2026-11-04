# `sage26_quasar_mode`

Ports SAGE26's black-hole growth and quasar-mode wind, `grow_black_hole` and `quasar_mode_wind` (`R/src/model_mergers.c:346-441`, `R` = the staged SAGE26 source), on the R0 path, for disk-instability triggers and merger events.

## Processing Contract

- Supported modes: `process_by_galaxy`, `process_per_event`
- `process_by_galaxy` consumes the disk instability's gas branch in `galaxy_physics`: after `sage26_disk_instability`, which hands on `UnstableDiskGasFraction`, and before `sage26_starburst_feedback`, as SAGE26's `check_disk_instability` calls `grow_black_hole` and then the burst (`R/src/model_disk_instability.c:101-108`)
- `process_per_event` consumes merger events in `satellite_mergers`, growing the merger target's black hole before the burst (`R/src/model_mergers.c:262-264`)

## Physics

The arithmetic lives in `shared/sage_agn_physics.h` (`mimic_apply_black_hole_growth`, which calls `mimic_apply_quasar_mode_wind`). The same function serves the post-minor-merger instability recheck inside `sage26_starburst_feedback`'s burst transaction.

- Nothing happens unless the galaxy's `ColdGas > 0` (`:350`). There is no guard on the trigger, the growth rate or `Vvir`: a zero trigger accretes zero and still runs the wind, as in SAGE26.
- `BHaccrete = BlackHoleGrowthRate * trigger / (1 + (280 / Vvir)^2) * ColdGas`, formed left to right with the galaxy's float `Vvir`; SAGE26's object fuses `1.0 + SQR(280.0 / Vvir)` into one multiply-add, written as `fma()` (`:351-352`).
- The accretion is capped to `ColdGas` (`:354-357`). Then `BlackHoleMass += BHaccrete`, `ColdGas -= BHaccrete`, and `MetalsColdGas -= Z_cold * BHaccrete` as one fused multiply-subtract, floored at zero if negative (`:359-365`).
- The wind runs with that accretion before it is recorded (`:374`); then `QuasarModeBHaccretionMass += BHaccrete` in float (`:376`).
- The wind energy is `QuasarModeEfficiency * 0.1 * BHaccrete * (C / UnitVelocity_in_cm_per_s)^2` with `C = 2.9979e10` cm/s (`:393`, `R/src/macros.h:30`). It ejects all the galaxy's cold gas into its own `EjectedGas` when it exceeds the cold binding energy `0.5 * ColdGas * Vvir^2` (`:394-403`), and all its hot gas when it exceeds the original cold binding energy plus the hot binding energy `0.5 * HotGas * Vvir^2` (`:430-440`, `CGMrecipeOn 0`). The energy used by the cold ejection is not subtracted.

## Properties

- Reads: `ColdGas`, `MetalsColdGas`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `UnstableDiskGasFraction`, `Vvir`
- Writes: `BlackHoleMass`, `QuasarModeBHaccretionMass`, `ColdGas`, `MetalsColdGas`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`

## Parameters

- `BlackHoleGrowthRate`
- `QuasarModeEfficiency`

## Ordering

**Enforced at init by event subscription validation (fails with ERROR if violated):**

1. When used as `process_per_event`, `sage26_resolve_mergers_and_disruption` must be configured as `process_full_halo` in the same substep phase — it is the only producer of `merger` events that this module consumes.

## Events

Consumes `merger` events from `sage26_resolve_mergers_and_disruption`. A merger event with a non-positive ratio is still skipped here, where SAGE26 grows the black hole with that ratio; the merger transaction's port removes that return.

## Tests

`_tests/test_unit_sage26_quasar_mode_oracle.c` runs every SAGE26 oracle fixture under `_tests/oracle/` (`grow_black_hole` on its own and with its wind, `quasar_mode_wind` on its own) through the shared functions and compares the result bit for bit with SAGE26's own routines; outside the oracle environment it reports SKIP.
