# `sage26_regime_aware_cooling`

SAGE26's regime-aware cooling (`cooling_recipe_regime_aware`) as one by-galaxy transaction. With `CGMrecipeOn 1` (the shipped configuration) it owns every galaxy's cooling: the hot regime's cold streams and hot-halo cooling, bulk CGM cooling, both radio-mode AGN calls, the cooling diagnostics, the application of the cooled mass to the cold gas, and the final `CoolingRate`. With `CGMrecipeOn 0` it writes nothing and the classic modules (`sage26_calculate_cooling_budget`, `sage26_radio_mode_heating`, `sage26_apply_cooling`) cool instead. The recipes are the shared helpers of `shared/sage_cooling_physics.h`.

## Physics

A galaxy's `Regime` is set each snapshot by [`sage26_determine_regime`](../sage26_determine_regime/README.md), which classifies halos against the shock mass, using a keyed random draw where SAGE26 uses libc `rand()`.

- **CGM regime (`Regime` 0, below the shock mass).** The circumgalactic gas cools in bulk (Carr et al. 2023) when `CGMgas > 0`, followed by its AGN heating.
- **Hot-halo regime (`Regime` 1, or any value other than 0).** When `HotGas > 0`, a fraction `f_stream` of the hot gas cools as cold streams on the cooling time, `f_stream * HotGas / tcool * dt`, and the rest by classic hot-halo cooling, `(1 - f_stream) * (HotGas / Rvir) * (rcool / (2 tcool)) * dt`, followed by the hot AGN heating. The stream fraction follows Dekel & Birnboim (2006): `f_stream = 1 / (1 + ratio^(1/0.15))` with `ratio = (3 M_* / Mvir)^(2/3) (Mvir / M_shock)^(4/3)`, `M_*` the clustering mass at the galaxy's stored snapshot redshift and `M_shock` from `MShockMsun`; streams are switched off for a halo above the shock mass once `3 M_*` reaches it. Any residual CGM then cools in bulk, with its AGN reading the black hole and heating radius the hot AGN left.
- **Application.** The CGM cooling mass is applied, then the hot one, each capped to its reservoir as the AGN left it and moved to the cold gas with that reservoir's metallicity. `CoolingRate` is overwritten with the total applied mass per unit time.
- **Diagnostics.** `tcool`, `tff` and `RcoolToRvir` are reset (to 0, -1 and -1) where no recipe sets them and restored to the hot values where those are valid. The hot recipe's stream and hot-halo rates, before the cap and AGN, are stored in `mdot_cool_raw` and `mdot_stream_raw`, from which `sage26_finalize_cooling_heating` writes the outputs `mdot_cool` and `mdot_stream`. `Cooling` and `Heating` accumulate inside the recipes and are normalised once per snapshot by the same finaliser.

Several SAGE26 behaviours are kept deliberately and marked `SAGE parity:` in the source: the mdot rates describe pre-cap, pre-AGN components; CGM cooling never sets `RcoolToRvir`; the hot and CGM `tcool` and `tff` use different conversions; and the application lacks the classic "move all metals when the mass equals the reservoir" branch.

## Processing Contract

- Supported mode: `process_by_galaxy`. With `CGMrecipeOn 1`, exactly once, in the substep phase that holds the classic cooling modules (`galaxy_physics` in the shipped run files), immediately after `sage26_apply_cooling`; `init()` refuses any other placement. With `CGMrecipeOn 0` no placement is checked.
- Every non-merged galaxy cools, Type 2 orphans included.
- Without a positive interval (the core's snapshot-zero birth) the module writes nothing. A stored `SnapNum` outside the redshift table is refused with an error naming it, and `process()` returns -1.

## Ordering

- `sage26_calculate_cooling_budget` refuses `CGMrecipeOn 1` unless this module is in its substep phase and `sage26_determine_regime` is in `pre_timestep`; it then writes only a no-cooling transport, so the classic radio and apply modules write nothing.
- This module's `init()` does not itself check the budget, `sage26_radio_mode_heating` or `sage26_determine_regime`, and does not refuse `AGNrecipe 0`, with which neither AGN call is made.

## Properties

- Reads: `HaloNr`, `SnapNum`, `dT`, `Mvir`, `Rvir`, `Vvir`, `Regime`
- Reads and writes: `HotGas`, `MetalsHotGas`, `CGMgas`, `MetalsCGMgas`, `ColdGas`, `MetalsColdGas`, `BlackHoleMass`, `Rheat`, `Cooling`, `Heating`, `tcool`, `tff`, `RcoolToRvir`, `CoolingRate`
- Writes: `mdot_cool_raw`, `mdot_stream_raw` (package-private state, never output)

## Parameters

All four are required with no default and are validated whatever `CGMrecipeOn` holds; a missing key or invalid value fails `init()` naming the parameter.

- `CGMrecipeOn` (0 or 1): 1 runs the transaction; 0 makes the module a no-op.
- `MShockMsun` (positive and finite): the Dekel & Birnboim shock mass in physical Msun; SAGE26's default is 6.0e11.
- `AGNrecipe` (0 or 2, validated by `shared/sage_agn_recipe.h`): 2 selects Bondi-Hoyle radio-mode accretion in both AGN calls; 0 skips them. SAGE26's 1 and 3 are not supported; refused at initialisation.
- `RadioModeEfficiency` (in [0, 1]): the radio-mode accretion efficiency, as `sage26_radio_mode_heating` reads it.

## Tests

- `_tests/test_unit_sage26_regime_aware_cooling.c`: each branch of the transaction, the application order and caps, the diagnostic paths, and the initial boundary.
- `_tests/test_integration_sage26_regime_aware_cooling.py`: runs the module in Mimic and checks the no-op with `CGMrecipeOn 0`; its `CGMrecipeOn 1` output on the run file's full pipeline is covered by the physics baseline, `../_tests/test_scientific_sage26_physics_baseline.py`.
- The parameter refusals and the placement guard of this module and the budget are covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c` and `../_tests/test_unit_sage_dependency_contracts.c`.
