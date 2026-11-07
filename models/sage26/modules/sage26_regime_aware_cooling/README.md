# `sage26_regime_aware_cooling`

SAGE26's regime-aware cooling as one by-galaxy transaction (campaign decision D38): with `CGMrecipeOn 1` it owns every galaxy's cooling, the hot regime's cold streams and bulk CGM cooling, both radio-mode AGN calls, the cooling diagnostics, the application of both cooled masses to the cold gas and the final `CoolingRate`. The run files hold `CGMrecipeOn 1` (R4, kept through R6). With `CGMrecipeOn 0` (R0-R3) it writes nothing and the classic modules cool.

## SAGE26 Source

A port of `cooling_recipe_regime_aware` (`src/model_cooling_heating.c:420-506`), which SAGE26 calls instead of `cooling_recipe_hot` when `CGMrecipeOn` is 1 (`:125-132`, from `src/core_build_model.c:503-509`), with its static `reset_cgm_diagnostics` (`:413-418`). The nested recipes are the shared composed helpers of `shared/sage_cooling_physics.h`: `mimic_sage_cooling_recipe_hot` (`cooling_recipe_hot` with `CGMrecipeOn 1`, its cold streams and `do_AGN_heating`) and `mimic_sage_cooling_recipe_cgm` (`cooling_recipe_cgm` and the static `do_AGN_heating_cgm`). On each call, with its interval `dt`:

1. the wrapper's locals start at `cgm_cooling = hot_cooling = 0`, the float `hot_tcool = -1` and `hot_diagnostics_valid = 0`, and the raw mdot rates are reset to 0 (`:422-434`);
2. a `Regime 0` galaxy runs the CGM recipe when `CGMgas > 0`, otherwise the diagnostic reset (`:436-444`);
3. any other `Regime` runs the hot recipe and its AGN when `HotGas > 0`, then saves the float `tcool` and marks the hot diagnostics valid when `Vvir > 0 && tcool > 0` after that call (`:451-457`); then runs the CGM recipe and its AGN when `CGMgas > 0`, reading the black-hole mass and heating radius the hot AGN changed, or else the reset only when the hot diagnostics are invalid (`:460-464`); then, when they are valid, restores the saved `tcool` and sets `tff = -1` (`:466-469`);
4. the CGM mass is applied, then the hot mass, each capped to its reservoir as the AGN left it and moved with that reservoir's metallicity (`get_metallicity`), each metal credit and debit formed from its double mass and double metallicity, without the classic transfer's "move all metals on equality" branch (`:475-496`);
5. `CoolingRate` is overwritten with the total applied, `(total / dt) * 1e10 / Hubble_h * SEC_PER_GIGAYEAR / UnitTime_in_s` in float (`:498-502`), even where a nested recipe returned early.

The reset writes exactly `tcool = 0`, `tff = -1` and `RcoolToRvir = -1`. A positive `CGMgas` whose `Vvir` or `lambda` guard fails does not invoke the reset, so the earlier diagnostics survive. SAGE26's closing `XASSERT(total_cooling >= 0.0)` (`:503-504`) is not ported: from a valid state each recipe returns a non-negative mass and each application caps a positive mass to its non-negative reservoir, so the total is non-negative and the assertion cannot fire. `Cooling` and `Heating` accumulate inside the recipes, before the application caps; `sage26_finalize_cooling_heating` normalises them once per snapshot.

The hot recipe's float stores of `hot_halo_cooling / dt` and `cold_stream_cooling / dt` (`:282-283`), made before the reservoir cap and the AGN suppression, go to the raw state properties `mdot_cool_raw` and `mdot_stream_raw` (campaign decision D39); `sage26_finalize_cooling_heating` writes the output `mdot_cool` and `mdot_stream` from them in SAGE26's writer arithmetic (`src/io/save_gals_hdf5.c:996-997`).

Quirks kept, each marked `// SAGE parity:` in the source: the mdot rates describe pre-cap, pre-AGN hot-reservoir components; CGM cooling never sets `RcoolToRvir`; the hot and CGM `tcool` and `tff` use different conversions; residual CGM gas cools in a hot-regime galaxy after the hot AGN changed the black hole and `Rheat`; "valid" hot diagnostics need only `Vvir > 0` and `tcool > 0`; the CGM-on application lacks the equality branch; and whether the reset runs depends on the reservoirs and the call path, not on the regime alone.

Precision: the reservoirs, `Mvir`, `Rvir` and `Vvir` are SAGE26's floats (the core's doubles narrowed); every wrapper local is a double except the float `hot_tcool`. SAGE26's object fuses the four metal credits and debits of the applications (`:481`, `:483`, `:493`, `:495`); each is one contractible compound assignment here (`../_tests/oracle/README.md`, "The c11 Group").

## Initial Boundary and the Stored Snapshot

Without a positive interval (the core's snapshot-zero birth, stored `SnapNum` -1 and `dT` <= 0), where SAGE26 is undefined (`Age[-1]`, `src/core_build_model.c:479`), the module reads no table and writes nothing, so the galaxy holds exactly what the classic trio writes there with `CGMrecipeOn 0` (only the budget's no-cooling transport). With a positive interval the hot regime's cold streams read `ZZ` of the galaxy's stored snapshot (`:205`); a stored `SnapNum` outside the redshift table is refused, `process()` returning -1 (campaign decision D35).

## Processing Contract

- Supported mode: `process_by_galaxy`
- Required phase: with `CGMrecipeOn 1`, exactly once, in the substep phase that holds the classic cooling modules (`galaxy_physics` in the run files), immediately after `sage26_apply_cooling`; `init()` refuses any other placement. With `CGMrecipeOn 0` no placement is checked.
- `sage26_calculate_cooling_budget` refuses `CGMrecipeOn 1` unless this module is in its substep phase and `sage26_determine_regime` in `pre_timestep`, and writes only its no-cooling transport there, so the classic radio and apply modules write nothing.
- Every non-merged galaxy cools, Type 2 orphans included (`src/core_build_model.c:473-516`).

## Properties

- Reads: `HaloNr`, `SnapNum`, `dT`, `Mvir`, `Rvir`, `Vvir`, `Regime`
- Reads and writes: `HotGas`, `MetalsHotGas`, `CGMgas`, `MetalsCGMgas`, `ColdGas`, `MetalsColdGas`, `BlackHoleMass`, `Rheat`, `Cooling`, `Heating`, `tcool`, `tff`, `RcoolToRvir`, `CoolingRate`
- Writes: `mdot_cool_raw`, `mdot_stream_raw` (package-private state, never output)

## Parameters

- `CGMrecipeOn` — required (campaign decision D33), 0 or 1; any other value or a missing key fails `init()` naming the parameter.
- `MShockMsun` — required (D33), the streams' Dekel & Birnboim shock mass in physical Msun, positive and finite; SAGE26's default 6.0e11 (`src/core_read_parameter_file.c:114`).
- `AGNrecipe`, `RadioModeEfficiency` — the radio-mode AGN configuration both nested AGN calls use, read as `sage26_radio_mode_heating` reads them (SAGE26's `AGNrecipeOn` and `RadioModeEfficiency`).

## Tests

`_tests/test_unit_sage26_regime_aware_cooling.c` tests the transaction on every platform, each expectation derived independently of the code under test (a literal chosen so the result is exact, or SAGE26's expression inline with its line): `CGMrecipeOn 0` writes nothing; the mdot reset and the final `CoolingRate` overwrite; the streams' raw rates; the hot AGN's heating radius read by the CGM AGN; the CGM application before the hot one; the hot application's cap to the post-AGN reservoir; the CGM application after the CGM AGN (Bondi), moving the post-AGN CGM pair with its post-AGN metallicity; the missing equality branch with metals above the proportional split; every diagnostic path (reset, survival on failed guards, restoration); the initial boundary against the classic trio; the D35 refusal with a positive interval; the parameter refusals; and the configuration guard of this module and the budget. `_tests/test_unit_sage26_regime_aware_cooling_oracle.c` runs the wrapper fixtures under `_tests/oracle/` (direct calls of SAGE26's public `cooling_recipe_regime_aware`, `../_tests/oracle/README.md`, "The c11 Group") through `sage26_regime_aware_cooling_run` and asserts every recorded field, flag and local bit for bit; outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.
