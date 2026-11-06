# `sage26_quasar_mode`

Ports SAGE26's black-hole growth and quasar-mode wind, `grow_black_hole` and `quasar_mode_wind` (`R/src/model_mergers.c:346-441`, `R` = the staged SAGE26 source), with `CGMrecipeOn` 1 (the run files' value, SAGE26's R4) or 0 (R0-R3) and `SFprescription` 1 (the run files' value, SAGE26's R2) or 0 (R0, R1), for disk-instability triggers and merger events.

## Processing Contract

- Supported modes: `process_by_galaxy`, `process_per_event`
- `process_by_galaxy` consumes the disk instability's gas branch in `galaxy_physics`: after `sage26_disk_instability`, which hands on `UnstableDiskGasFraction`, and before `sage26_starburst_feedback`, as SAGE26's `check_disk_instability` calls `grow_black_hole` and then the burst (`R/src/model_disk_instability.c:101-108`)
- `process_per_event` consumes merger events in `satellite_mergers`, growing the merger target's black hole before the burst (`R/src/model_mergers.c:262-264`)

## Physics

The arithmetic lives in `shared/sage_agn_physics.h` (`mimic_apply_black_hole_growth`, which calls `mimic_apply_quasar_mode_wind`). The same function serves the post-minor-merger instability recheck inside `sage26_starburst_feedback`'s burst transaction.

- Nothing happens unless the galaxy's `ColdGas > 0` (`:350`). There is no guard on the trigger, the growth rate or `Vvir`: a zero trigger accretes zero and still runs the wind, as in SAGE26.
- `BHaccrete = BlackHoleGrowthRate * trigger / (1 + (280 / Vvir)^2) * ColdGas`, formed left to right with the galaxy's float `Vvir`; SAGE26's object fuses `1.0 + SQR(280.0 / Vvir)` into one multiply-add, written as `fma()` (`:351-352`).
- The accretion is capped to `ColdGas` (`:354-357`). Then `BlackHoleMass += BHaccrete`, `ColdGas -= BHaccrete`, and `MetalsColdGas -= Z_cold * BHaccrete` as one fused multiply-subtract, floored at zero if negative (`:359-365`).
- With `SFprescription 1` the accretion's hydrogen cap follows, before the wind (`:366-372`): `H2gas` and `H1gas` are each capped, on its own and strictly, at the float `max_h_bh = (ColdGas > 0.0f) ? ColdGas * 0.74 : 0.0f` of the cold gas the accretion leaves; never their sum. The guard is written as SAGE26 writes it at this site, `SFprescription != 0 && SFprescription != 2` (the burst refresh tests `sf_prescription_tracks_h2` instead). With `SFprescription 0` it does not run.
- The wind runs with that accretion before it is recorded (`:374`); then `QuasarModeBHaccretionMass += BHaccrete` in float (`:376`).
- The wind energy is `QuasarModeEfficiency * 0.1 * BHaccrete * (C / UnitVelocity_in_cm_per_s)^2` with `C = 2.9979e10` cm/s (`:393`, `R/src/macros.h:30`). It ejects all the galaxy's cold gas into its own `EjectedGas` when it exceeds the cold binding energy `0.5 * ColdGas * Vvir^2` (`:394-403`), and all of its selected pair when it exceeds the original cold binding energy plus that pair's binding energy: with `CGMrecipeOn 1` and the WIND HOST's own `Regime` 0 its CGM pair, `0.5 * CGMgas * Vvir^2` (`:406-417`), otherwise its hot pair, `0.5 * HotGas * Vvir^2` (`:418-428`, `:430-440`); the unselected pair is not touched. The host is the galaxy growing the black hole (the unstable galaxy or the merger target), never the FoF central as such. The energy used by the cold ejection is not subtracted. A wind that ejects the whole cold gas leaves `H2gas` and `H1gas` as the cap left them: SAGE26 refreshes no hydrogen after the wind (`:397-403`), and neither does the package.

## Properties

- Reads: `ColdGas`, `MetalsColdGas`, `H1gas`, `H2gas`, `HotGas`, `MetalsHotGas`, `CGMgas`, `MetalsCGMgas`, `Regime` (the wind host's), `EjectedGas`, `MetalsEjectedGas`, `UnstableDiskGasFraction`, `Vvir`
- Writes: `BlackHoleMass`, `QuasarModeBHaccretionMass`, `ColdGas`, `MetalsColdGas`, `HotGas`, `MetalsHotGas` or `CGMgas`, `MetalsCGMgas` (the wind's selected pair), `EjectedGas`, `MetalsEjectedGas`, and with `SFprescription 1` `H1gas` and `H2gas` (the cap)

## Parameters

- `BlackHoleGrowthRate`
- `QuasarModeEfficiency`
- `SFprescription` (required, 0 or 1): 1, the run files' value (BR06, SAGE26's R2), applies the accretion's hydrogen cap (`:366-372`); 0 (R0, R1) does not. Any other value, or a missing key, fails initialisation with an error naming the parameter; there is no default.
- `CGMrecipeOn` (required, 0 or 1, campaign decision D33): 1, the run files' value (SAGE26's R4), selects the wind's second pair by the wind host's `Regime`; 0 (R0-R3) always the hot pair. Any other value, or a missing key, fails initialisation with an error naming the parameter; there is no default.

## Ordering

**Enforced at init by event subscription validation (fails with ERROR if violated):**

1. When used as `process_per_event`, `sage26_resolve_mergers_and_disruption` must be configured as `process_full_halo` in the same substep phase — it is the only producer of `merger` events that this module consumes.

## Events

Consumes `merger` events from `sage26_resolve_mergers_and_disruption` and grows the target's black hole for every merger, a ratio of 0 included, as SAGE26 does (`src/model_mergers.c:262-264`). In the merger phase it must precede `sage26_starburst_feedback`'s merger consumer, which the resolver enforces at init.

## Tests

`_tests/test_unit_sage26_quasar_mode_oracle.c` runs every SAGE26 oracle fixture under `_tests/oracle/` (`grow_black_hole` on its own and with its wind, `quasar_mode_wind` on its own) through the shared functions and compares the result bit for bit with SAGE26's own routines, with `SFprescription 1` also the cap's flags and bound (`bh_r2_*`, `qw_r2_*`); outside the oracle environment it reports SKIP. `_tests/test_unit_sage26_quasar_mode.c` asserts the cap's platform-independent invariants (each above, at and below the bound independently, the guard at `SFprescription 0`, no refresh after the wind) and the module's required `SFprescription`, passed on by both channels (`test_hydrogen_cap_module_wiring` for the disk instability, `test_hydrogen_cap_module_wiring_per_event` for a merger event).

`_tests/test_unit_sage26_quasar_mode.c`'s `test_cgm_wind_energy_tests` runs `CGMrecipeOn` 1 against 0 on the same input through both channels (the instability and the merger event), the wind host in each `Regime` and the FoF central in the other: the wind's two energy tests on each pair (ejecting both, only the cold gas because the second test keeps the original cold binding energy, or nothing), every reservoir an exact literal and each case's side of both tests checked with SAGE26's expressions at run time, the unselected pair and the central unchanged; `test_cgm_recipe_on_refused` checks the parameter's refusals.
