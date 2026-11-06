# `sage26_radio_mode_heating`

Suppresses the hot-halo cooling mass with radio-mode AGN heating: the heating radius left by earlier events reduces the mass, and the central black hole accretes hot gas, whose heating ratchets the heating radius outward.

## SAGE26 Source

A port of SAGE26's `do_AGN_heating` and the Bondi branch of its private `agn_accretion_compute` (`src/model_cooling_heating.c:526-626`) as `cooling_recipe_hot` calls them on the R0 path (`:297-299`, `CGMrecipeOn 0`, `AGNrecipe 2`). The arithmetic is the shared `mimic_sage_do_agn_heating_hot` and `mimic_sage_agn_accretion_compute` (`shared/sage_cooling_physics.h`), called with `CGMrecipeOn 0`; with `CGMrecipeOn 1` the same helpers add the 0.99 saturation and the cap of `Rheat` to `Rvir`, and `mimic_sage_do_agn_heating_cgm` is the CGM call, none of which a production module calls yet. The module runs its physics only where SAGE26 calls `do_AGN_heating`: `AGNrecipe > 0` and a positive post-cap cooling mass. Otherwise it leaves every field, the carried mass included, unchanged.

1. **Suppression.** With `Rheat < rcool` the mass becomes `(1 - Rheat / rcool) * coolingGas`; otherwise it is 0 (`:590-595`). The CGM-only `0.99 * rcool` saturation does not apply on R0.
2. **Accretion** (only with `HotGas > 0`, `:599`). The Bondi rate `(2.5 pi G) (0.375 * 0.6 x) M_BH RadioModeEfficiency` uses SAGE26's `x`, carried exactly from `sage26_calculate_cooling_budget` (`:540`). It is capped at the Eddington rate `1.3e38 M_BH 1e10 / h` erg/s over `0.1 c^2` (`:553-557`), multiplied by `dt`, and capped at the hot gas. With `Vvir > 0` the heating mass is `(1.34e5 / Vvir)^2` times the accreted mass, capped (with `AGNcoeff > 0`) at the suppressed cooling mass, which lowers the accreted mass to match (`:562-572`).
3. **Update.** The black hole grows by the accreted mass, drawn from the hot gas with its metals at the metallicity of the hot gas before the draw (`:605-608`). `Rheat` ratchets to `(AGNheating / coolingGas) * rcool` when that is larger (`:612-616`). `Heating` gains `0.5 * AGNheating * Vvir^2`, undivided (`:618-620`); `sage26_finalize_cooling_heating` normalises it once per snapshot.

The returned mass is only the `Rheat`-suppressed one: SAGE26 does not subtract the new heating from the cooling mass a second time, and that is mirrored. The CGM-only cap of `Rheat` to `Rvir` (`:623-624`) does not apply on R0. On this path `Vvir` (the heating coefficient and the `Heating` term) is read as the float SAGE26 holds; the Bondi rate reads no halo property.

The empirical (`AGNrecipe 1`) and cold-cloud (`3`) accretion rates are the earlier package's, not ported from SAGE26; R0 does not use them.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_cooling_budget` and before `sage26_apply_cooling`
- Receives one galaxy at a time; every non-merged galaxy is heated by its own black hole, Type 2 orphans included

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Mvir`, `Vvir`, `Rvir`, `CoolingGas`, `CoolingX`, `Rcool`, `BlackHoleMass`, `HotGas`, `MetalsHotGas`, `Rheat`
- Writes: `CoolingGas` (the suppressed mass), `BlackHoleMass`, `HotGas`, `MetalsHotGas`, `Rheat`, `Heating`

## Parameters

- `RadioModeEfficiency` (R0: `0.08`)
- `AGNrecipe` (R0: `2`, Bondi-Hoyle)

## Ordering

**Advisory (no `init()` enforcement):**

1. Run after `sage26_calculate_cooling_budget`, so `CoolingGas`, `CoolingX` and `Rcool` hold this call's values.
2. Run before `sage26_apply_cooling`, so the apply step commits the suppressed mass.

## Tests

`_tests/test_unit_sage26_radio_mode_heating_oracle.c` runs every standalone `do_AGN_heating` fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`); the c11 fixtures (`agn_cgmon_*`, `cgm_agn_*`: `do_AGN_heating` with `CGMrecipeOn 1` and the CGM AGN reached through `cooling_recipe_cgm`) run through the shared helpers directly ("The c11 Group"). Outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_radio_mode_heating.c` also tests the `CGMrecipeOn 1` AGN helpers on every platform: the 0.99 saturation either side, the hot call's `Rvir` cap outside its reservoir block and the CGM call's inside it, and the CGM call's re-cap to the post-accretion CGM.
