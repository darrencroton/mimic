# `sage26_radio_mode_heating`

Suppresses the hot-halo cooling mass with radio-mode AGN heating: the heating radius left by earlier events reduces the mass, and the central black hole accretes hot gas, whose heating ratchets the heating radius outward.

## SAGE26 Source

A port of SAGE26's `do_AGN_heating` and the Bondi branch of its private `agn_accretion_compute` (`src/model_cooling_heating.c:526-626`) as `cooling_recipe_hot` calls them on the R0 path (`:297-299`, `CGMrecipeOn 0`, `AGNrecipe 2`). The arithmetic is the shared `mimic_sage_do_agn_heating_hot` and `mimic_sage_agn_accretion_compute` (`shared/sage_cooling_physics.h`), called with `CGMrecipeOn 0`; with `CGMrecipeOn 1` the same helpers add the 0.99 saturation and the cap of `Rheat` to `Rvir`, and `mimic_sage_do_agn_heating_cgm` is the CGM call; `sage26_regime_aware_cooling`, the package's port of SAGE26's regime-aware cooling as one by-galaxy transaction, calls them there, and this module then writes nothing, as `sage26_calculate_cooling_budget` carries `CoolingGas` 0. The module runs its physics only where SAGE26 calls `do_AGN_heating`: `AGNrecipe > 0` and a positive post-cap cooling mass. Otherwise it leaves every field, the carried mass included, unchanged.

1. **Suppression.** With `Rheat < rcool` the mass becomes `(1 - Rheat / rcool) * coolingGas`; otherwise it is 0 (`:590-595`). The `CGMrecipeOn 1` `0.99 * rcool` saturation does not apply on R0-R3; with `CGMrecipeOn 1` it applies in `sage26_regime_aware_cooling`'s call of the same helper.
2. **Accretion** (only with `HotGas > 0`, `:599`). The Bondi rate `(2.5 pi G) (0.375 * 0.6 x) M_BH RadioModeEfficiency` uses SAGE26's `x`, carried exactly from `sage26_calculate_cooling_budget` (`:540`). It is capped at the Eddington rate `1.3e38 M_BH 1e10 / h` erg/s over `0.1 c^2` (`:553-557`), multiplied by `dt`, and capped at the hot gas. With `Vvir > 0` the heating mass is `(1.34e5 / Vvir)^2` times the accreted mass, capped (with `AGNcoeff > 0`) at the suppressed cooling mass, which lowers the accreted mass to match (`:562-572`).
3. **Update.** The black hole grows by the accreted mass, drawn from the hot gas with its metals at the metallicity of the hot gas before the draw (`:605-608`). `Rheat` ratchets to `(AGNheating / coolingGas) * rcool` when that is larger (`:612-616`). `Heating` gains `0.5 * AGNheating * Vvir^2`, undivided (`:618-620`); `sage26_finalize_cooling_heating` normalises it once per snapshot.

The returned mass is only the `Rheat`-suppressed one: SAGE26 does not subtract the new heating from the cooling mass a second time, and that is mirrored. The `CGMrecipeOn 1` cap of `Rheat` to `Rvir` (`:623-624`) does not apply on R0-R3; with `CGMrecipeOn 1` it applies in `sage26_regime_aware_cooling`. On this path `Vvir` (the heating coefficient and the `Heating` term) is read as the float SAGE26 holds; the Bondi rate reads no halo property.

SAGE26's empirical (`AGNrecipeOn 1`, `:545-551`) and cold-cloud (`3`, `:541-544`) accretion rates are not ported, and both values are refused at initialisation (`shared/sage_agn_recipe.h`); no configuration R0–R6 uses them.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_cooling_budget` and before `sage26_apply_cooling`
- Receives one galaxy at a time; every non-merged galaxy is heated by its own black hole, Type 2 orphans included

## Properties

- Reads: `HaloNr`, `SnapNum`, `dT`, `Vvir`, `Rvir`, `CoolingGas`, `CoolingX`, `Rcool`, `BlackHoleMass`, `HotGas`, `MetalsHotGas`, `Rheat`
- Writes: `CoolingGas` (the suppressed mass), `BlackHoleMass`, `HotGas`, `MetalsHotGas`, `Rheat`, `Heating`

## Parameters

Both are required, with no default: a missing key, or a value outside the accepted range, fails `init()` naming the parameter (`LOAD_AND_VALIDATE_RANGE_INCLUSIVE`, `src/module_system/parameter_helpers.h`, and `mimic_sage_validate_agn_recipe`, `shared/sage_agn_recipe.h`).

- `RadioModeEfficiency`: accepted values [0, 1]. The run files hold 0.08, SAGE26's value (`input/millennium.par:74`), which no configuration R0–R6 changes.
- `AGNrecipe` (SAGE26's `AGNrecipeOn`): accepted values 0 and 2; 1 and 3 are refused (above). 0 switches the module off (it then writes nothing); 2 is SAGE26's Bondi-Hoyle rate, ported exactly. The run files hold 2, SAGE26's value (`input/millennium.par:49`), which no configuration R0–R6 changes. `sage26_regime_aware_cooling`, `sage26_quasar_mode` and `sage26_starburst_feedback` read it through the same validator; as in SAGE26, 0 also stops the black-hole growth at disk instabilities and mergers (`src/model_mergers.c:262-264`, `src/model_disk_instability.c:101-103`).

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. When `sage26_calculate_cooling_budget` is configured, it must precede this module (both `process_by_galaxy`) in the same substep phase, so `CoolingGas`, `CoolingX` and `Rcool` hold this call's values rather than the previous substep's.
2. When `sage26_apply_cooling` is configured, this module must precede it (both `process_by_galaxy`) in the same substep phase, so the apply step commits the suppressed mass.

**Not refused:** running without either partner (without the budget `CoolingGas` keeps its per-snapshot reset value 0 and the module writes nothing; without the apply step the suppressed mass is never applied), more than once, or with `CGMrecipeOn 1`, which it does not read (the budget then carries `CoolingGas` 0, so the module writes nothing). `sage26_apply_cooling`'s `init()` also requires the budget before it.

## Tests

`_tests/test_unit_sage26_radio_mode_heating_oracle.c` runs every standalone `do_AGN_heating` fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`); the c11 fixtures (`agn_cgmon_*`, `cgm_agn_*`: `do_AGN_heating` with `CGMrecipeOn 1` and the CGM AGN reached through `cooling_recipe_cgm`) run through the shared helpers directly ("The c11 Group"). Outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_radio_mode_heating.c` tests the module on every platform: the Eddington cap (reached by the Bondi rate from a large carried `x`), the suppression by `Rheat`, the `Rheat` ratchet and the `Heating` accumulation, the Bondi rate from the carried `x` with `Heating` undivided and no second subtraction of the heating; an unchanged black hole and hot gas with `AGNrecipe 0` or a zero cooling mass, and an unchanged black hole without hot gas; a zero black hole handled without error; a Type 2 orphan processed like a central; initialisation with `RadioModeEfficiency` 0, 0.02, 0.8 and 1; `AGNrecipe` 1 and 3 refused with an error naming the parameter, 0 and 2 accepted; initialisation through `module_system_init()`; and an init/cleanup cycle without leaks. It also tests the `CGMrecipeOn 1` AGN helpers on every platform: the 0.99 saturation either side, the hot call's `Rvir` cap outside its reservoir block and the CGM call's inside it, and the CGM call's re-cap to the post-accretion CGM. `_tests/test_integration_sage26_radio_mode_heating.py` runs Mimic with `sage26_calculate_cooling_budget` and this module in `galaxy_physics` (`CGMrecipeOn 0`) and checks that the run succeeds and writes finite, non-negative `BlackHoleMass`, `HotGas` and `Heating`; that black holes in halos with `Mvir` above 10 hold under 10% of it and hot gas in halos with `Mvir` above 1 at most all of it; that runs with `AGNrecipe` 0 and with `RadioModeEfficiency` 0.02 and 0.20 succeed, the higher efficiency giving at least the lower's total black-hole mass and heating; and that a run leaks no memory and finishes within 30 s.
