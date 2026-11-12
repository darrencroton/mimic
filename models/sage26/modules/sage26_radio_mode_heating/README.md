# `sage26_radio_mode_heating`

Suppresses the hot-halo cooling mass with radio-mode AGN heating, following SAGE26's `do_AGN_heating` on the hot-gas cooling path (`CGMrecipeOn 0`): the heating radius left by earlier episodes reduces the cooling mass, and the central black hole accretes hot gas at the Bondi-Hoyle rate, whose heating pushes the heating radius outward. The arithmetic is the shared `mimic_sage_do_agn_heating_hot` and `mimic_sage_agn_accretion_compute` (`shared/sage_cooling_physics.h`).

## Physics

The radio mode follows Croton et al. (2006, 2016). The module acts only with `AGNrecipe 2` and a positive cooling mass; otherwise it leaves every field unchanged.

1. **Suppression.** With `Rheat < Rcool` the cooling mass becomes `(1 - Rheat / Rcool) * CoolingGas`; otherwise it is 0.
2. **Accretion** (only with `HotGas > 0`). The Bondi-Hoyle rate `2.5 pi G (0.375 * 0.6 x) BlackHoleMass * RadioModeEfficiency` uses the cooling coefficient `x` carried in `CoolingX` from `sage26_calculate_cooling_budget`. It is capped at the Eddington rate, multiplied by the timestep, and capped at the hot gas. With `Vvir > 0` the heating mass is `(1.34e5 / Vvir)^2` times the accreted mass, capped at the suppressed cooling mass, which lowers the accreted mass to match.
3. **Update.** The black hole grows by the accreted mass, drawn from the hot gas with its metals at the hot gas's metallicity. `Rheat` ratchets to `(heating / CoolingGas) * Rcool` when that is larger. `Heating` gains `0.5 * heating * Vvir^2`, undivided; `sage26_finalize_cooling_heating` normalises it once per snapshot.

The returned cooling mass is only the `Rheat`-suppressed one; the new heating is not subtracted a second time, as in SAGE26. With `CGMrecipeOn 1` the AGN heating runs inside `sage26_regime_aware_cooling` through the same helpers (adding a 0.99 saturation of `Rheat / Rcool` and a cap of `Rheat` at `Rvir`), the budget carries `CoolingGas` 0, and this module writes nothing.

## Processing Contract

- Supported mode: `process_by_galaxy`, in `galaxy_physics`, after `sage26_calculate_cooling_budget` and before `sage26_apply_cooling`
- Receives one galaxy at a time; every non-merged galaxy is heated by its own black hole, Type 2 orphans included

## Ordering

- When `sage26_calculate_cooling_budget` is configured, `init()` refuses a run in which it does not precede this module in the same substep phase (both `process_by_galaxy`).
- When `sage26_apply_cooling` is configured, `init()` refuses a run in which this module does not precede it in the same substep phase.
- Not refused: running without either partner (without the budget `CoolingGas` stays 0 and the module writes nothing; without the apply step the suppressed mass is never applied), or with `CGMrecipeOn 1`, which this module does not read.

## Properties

- Reads: `HaloNr`, `SnapNum`, `dT`, `Vvir`, `Rvir`, `CoolingGas`, `CoolingX`, `Rcool`, `BlackHoleMass`, `HotGas`, `MetalsHotGas`, `Rheat`
- Writes: `CoolingGas` (the suppressed mass), `BlackHoleMass`, `HotGas`, `MetalsHotGas`, `Rheat`, `Heating`

## Parameters

Both are required with no default; a missing key or a value outside the accepted range fails `init()` naming the parameter.

- `RadioModeEfficiency` (in [0, 1]): the radio-mode accretion efficiency. The shipped run files use 0.08.
- `AGNrecipe` (0 or 2, validated by `shared/sage_agn_recipe.h`): 2 (the shipped value) selects Bondi-Hoyle accretion; 0 switches the radio mode off and, read by `sage26_regime_aware_cooling`, `sage26_quasar_mode` and `sage26_starburst_feedback` too, also stops black-hole growth at mergers and disk instabilities. SAGE26's empirical (1) and cold-cloud (3) accretion rates are not supported; refused at initialisation.

## Tests

- `_tests/test_unit_sage26_radio_mode_heating.c`: suppression, the Eddington cap, the `Rheat` ratchet and `Heating` accumulation, the off and empty cases, and the `CGMrecipeOn 1` AGN helpers.
- `_tests/test_integration_sage26_radio_mode_heating.py`: the module in a full run behind a pipeline that supplies gas, with a clean run, finiteness and range checks on the output and checks that `AGNrecipe 0` heats nothing and that `RadioModeEfficiency` changes `Heating` and `BlackHoleMass`.
- The `AGNrecipe` refusals are covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c`.
