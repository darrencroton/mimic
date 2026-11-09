# `sage26_quasar_mode`

Implements SAGE26's black-hole growth and quasar-mode wind, `grow_black_hole` and `quasar_mode_wind`, for two triggers: the disk instability's gas branch and galaxy mergers. The arithmetic lives in `shared/sage_agn_physics.h` (`mimic_apply_black_hole_growth`, which calls `mimic_apply_quasar_mode_wind`); the same function serves the post-minor-merger instability recheck inside `sage26_starburst_feedback`.

## Physics

Black holes grow by cold-gas accretion triggered by mergers and disk instabilities (Kauffmann & Haehnelt 2000), driving an energy-limited quasar wind (Croton et al. 2016).

- Nothing happens with `AGNrecipe 0`, or unless the galaxy has `ColdGas > 0`. The trigger is the merger's mass ratio (a ratio of 0 included) or the instability's `UnstableDiskGasFraction`; a zero trigger accretes nothing but still runs the wind.
- `BHaccrete = BlackHoleGrowthRate * trigger / (1 + (280 / Vvir)^2) * ColdGas`, capped at `ColdGas`. The black hole gains it; the cold gas and its metals (at the cold-gas metallicity, floored at zero) lose it, and it is added to `QuasarModeBHaccretionMass`.
- With `SFprescription 1`, after the accretion and before the wind, `H2gas` and `H1gas` are each capped at 0.74 of the remaining cold gas.
- The wind energy is `QuasarModeEfficiency * 0.1 * BHaccrete * c^2`. If it exceeds the cold gas's binding energy `0.5 * ColdGas * Vvir^2`, all the cold gas and its metals are ejected into the galaxy's `EjectedGas`. If it also exceeds the cold binding energy plus the binding energy of a second reservoir, that reservoir is ejected too: with `CGMrecipeOn 1` and the host galaxy's `Regime` 0 (the CGM regime), its CGM; otherwise its hot gas. The host is the galaxy growing the black hole (the unstable galaxy or the merger target), not the FoF central. The hydrogen is not refreshed after the wind.

## Processing Contract

- Supported modes: `process_by_galaxy`, `process_per_event`
- `process_by_galaxy` acts on the disk instability's gas branch in `galaxy_physics`, after `sage26_disk_instability` and before `sage26_starburst_feedback`. It never acts on a feedback-free-burst galaxy (`FFBRegime` 1), whose `UnstableDiskGasFraction` stays 0.
- `process_per_event` consumes `merger` events from `sage26_resolve_mergers_and_disruption` in `satellite_mergers`, growing the target's black hole before the merger burst, on any target including an FFB one.

## Ordering

- `process_per_event` requires `sage26_resolve_mergers_and_disruption` as `process_full_halo` in the same substep phase; `init()` refuses a run without it.
- When this module and `sage26_starburst_feedback` are both `process_per_event` in one substep phase, this module must come first; `sage26_resolve_mergers_and_disruption`'s `init()` refuses the other order.
- When `process_by_galaxy` and `sage26_disk_instability` share a substep phase, the instability must come first; its `init()` refuses the other order. Without `sage26_disk_instability`, `UnstableDiskGasFraction` stays 0 and this entry writes nothing. Its position against `sage26_starburst_feedback` is not checked.
- Either mode alone is accepted. Without the `process_per_event` entry no merger grows a black hole; `sage26_starburst_feedback`'s `init()` then warns, because its post-minor-merger recheck also skips black-hole growth.

## Properties

- Reads: `ColdGas`, `MetalsColdGas`, `H1gas`, `H2gas`, `HotGas`, `MetalsHotGas`, `CGMgas`, `MetalsCGMgas`, `Regime` (the host's), `EjectedGas`, `MetalsEjectedGas`, `UnstableDiskGasFraction`, `Vvir`
- Writes: `BlackHoleMass`, `QuasarModeBHaccretionMass`, `ColdGas`, `MetalsColdGas`, `HotGas` and `MetalsHotGas` or `CGMgas` and `MetalsCGMgas` (the wind's selected reservoir), `EjectedGas`, `MetalsEjectedGas`, and with `SFprescription 1` `H1gas` and `H2gas`

## Parameters

- `BlackHoleGrowthRate` (required, in (0, 1]): the accretion efficiency.
- `QuasarModeEfficiency` (required, in [0, 1]): the wind's coupling efficiency.
- `SFprescription` (required, 0 or 1): 1 (the shipped value, the Blitz & Rosolowsky 2006 molecular prescription) applies the hydrogen cap; 0 does not.
- `CGMrecipeOn` (required, 0 or 1): 1 (the shipped value) selects the wind's second reservoir by the host's `Regime`; 0 always selects the hot gas.
- `AGNrecipe` (required, 0 or 2, validated by `shared/sage_agn_recipe.h`): 2 (the shipped value) grows black holes in both channels; 0 grows none. SAGE26's radio-mode options 1 and 3 are not supported; refused at initialisation.

Every parameter is required with no default; a missing key or a value outside its range fails initialisation naming the parameter.

## Tests

- `_tests/test_unit_sage26_quasar_mode.c`: the growth and wind arithmetic, the hydrogen cap, the CGM-or-hot wind selection, `AGNrecipe 0` in both channels, and the parameter refusals.
- `_tests/test_integration_sage26_quasar_mode.py`: the module as `process_by_galaxy` in a full run, with physical-range and parameter-monotonicity checks on the output.
