# `sage26_prepare_infall_budget`

Prepares the baryon infall budget for the current FoF system by consolidating satellite material where required and calculating the gas available for infall.

## SAGE26 Source

A port of SAGE26's `infall_recipe` (`src/model_infall.c:67-213`), with both settings of the parameter `CGMrecipeOn`, which is required with no default (a missing key fails `init()`):

- Sums every galaxy's stellar, black-hole, cold, hot, ejected, ICS and CGM mass (and the ejected, ICS and CGM metals) over the FoF group, with either setting.
- Each satellite surrenders its ejected gas and ICS to the central; hot gas is not consolidated. A satellite with `ICS > 0` also hands its ICS to the central's `ICS_accrete` and its `ICS_sum_mt` to the central's, and its three ICS history fields are zeroed. With `CGMrecipeOn 0` the satellite's CGM is zeroed; with `CGMrecipeOn 1` the satellite keeps its CGM and metals across snapshots (`src/model_infall.c:111-117`), losing it only through stripping.
- The budget is `HaloBaryonFraction * Mvir - (stars + cold + hot + ejected + BH + ICS + CGM)`, with `HaloBaryonFraction` the once-rounded `reionization_modifier * BaryonFrac` from `sage26_reionization` and `Mvir` the central's float halo mass; the finish stays one expression so it contracts to the fused multiply-subtract SAGE26's build performs.
- The central keeps the pooled ejected gas and ICS, each clamped in SAGE26's order: metals capped to mass, then a negative mass zeroed with its metals, then negative metals floored.
- `CGMrecipeOn 1` (R4): after the budget and the ejected clamps, the central migrates only its own gas by its `Regime` (set this snapshot by `sage26_determine_regime`): Regime 0 adds its hot gas and metals to its CGM and zeroes the hot pair, any other regime adds its CGM to its hot gas and zeroes the CGM pair (`src/model_infall.c:159-176`). Each migration is a float-plus-float sum rounded into the float field. No satellite CGM is added to the central, although the budget counts it.
- `CGMrecipeOn 0` (R0-R3): the group's original CGM totals are added to the central's hot gas and metals, and the central's CGM is **not** cleared (SAGE26's deliberate off-branch quirk, `src/model_infall.c:177-181`, mirrored and marked `// SAGE parity:`); R0-R3 keep CGM zero, so this is inert there. It is not R4 behaviour.
- With either setting the central's CGM is then clamped in SAGE26's order: metals capped to gas, a negative gas cleared with its metals, negative metals floored (`src/model_infall.c:183-194`). On the hot-regime path the CGM pair is zero by then, so none of these can bind.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace and writes the central infall budget

## Ordering

`init()` enforces ordering: `sage26_reionization` is optional (`HaloBaryonFraction` falls back to `GlobalBaryonFraction` when unset, SAGE26's `ReionizationOn 0`), but when it is configured in `pre_timestep` it must appear before this module. Misordering aborts the run because the infall budget would read `HaloBaryonFraction` before reionization sets it.

Besides its two parameters (see Parameters), `init()` refuses only one configuration: `sage26_reionization` configured in `pre_timestep` as `process_full_halo` but not before this module there. The precedence helper is false when either module is absent from the phase, so this module placed outside `pre_timestep` while `sage26_reionization` is in it is refused too.

`init()` does not refuse:

- `sage26_reionization` absent, or configured outside `pre_timestep`: the check is skipped, and a central whose `HaloBaryonFraction` still holds its unset value -1.0 takes `GlobalBaryonFraction`.
- this module's own placement or entry count: it does not check that it is in `pre_timestep`, or that it appears once. The core does refuse a mode `module_info.yaml` does not list (it lists only `process_full_halo`).
- `CGMrecipeOn 1` without `sage26_determine_regime`: the central's `Regime` then keeps its creation value -1 (only the classifier writes it), and any value other than 0 takes the hot-regime migration. The classifier's own `init()` refuses a `pre_timestep` entry of it that does not precede this module, when this module is in `pre_timestep`.
- the absence of `sage26_apply_infall`: the budget is then written to `InfallingGas` and never applied.

## Properties

- Reads: `Type`, `Mvir`, `HaloBaryonFraction`, `StellarMass`, `BlackHoleMass`, `ColdGas`, `HotGas`, `MetalsHotGas`, `ICS`, `MetalsICS`, `EjectedGas`, `MetalsEjectedGas`, `CGMgas`, `MetalsCGMgas`, `ICS_accrete`, `ICS_sum_mt`, `Regime` (the central's, `CGMrecipeOn 1`)
- Writes: `HaloBaryonFraction`, `InfallingGas`, `ICS`, `MetalsICS`, `EjectedGas`, `MetalsEjectedGas`, `HotGas`, `MetalsHotGas`, `CGMgas`, `MetalsCGMgas`, `ICS_disrupt`, `ICS_accrete`, `ICS_sum_mt`

## Parameters

- `GlobalBaryonFraction` (required, accepted (0, 1]; a value outside that, or a missing key, fails initialisation naming it; the run files hold 0.17): the central's `HaloBaryonFraction` when that still holds its unset value -1.0, as when `sage26_reionization` is not configured
- `CGMrecipeOn` (required, 0 or 1; any other value, or a missing key, fails initialisation naming it; the run files hold 1, SAGE26's R4, kept through R6)

## Tests

`_tests/test_unit_sage26_prepare_infall_budget_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`; the `cgm_*` fixtures are `CGMrecipeOn 1`, "The c10 Group, Part 2"); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_prepare_infall_budget.c` holds platform-independent tests of both settings on the same exact inputs (`test_cgm_central_migration_by_regime`) and of the parameter's refusal (`test_cgm_recipe_on_refused`). It also covers registration and the init/cleanup lifecycle, the refusal of `GlobalBaryonFraction` 0, -0.1 and 1.5, the budget of a lone central and of a zero-mass halo, the pooling of one and of several satellites' ejected gas and ICS, and an orphan keeping its hot gas. `_tests/test_integration_sage26_prepare_infall_budget.py` runs Mimic with `sage26_reionization` and this module in `pre_timestep` at `CGMrecipeOn 0`. It checks that each run succeeds, that the module logs its initialisation and cleanup, that a `GlobalBaryonFraction` of 0.20 appears in the log, that the hot, ejected and ICS fields are in the output, that no memory leak is reported, that the Type 0 rows' `HotGas`, `EjectedGas` and `ICS` are finite (`InfallingGas` is not an output field), and that the module runs alongside `sage26_satellite_stripping` and `sage26_apply_infall`.

## Notes

`InfallingGas` is a transport field consumed by `sage26_apply_infall` during substeps. `ICS_disrupt`, `ICS_accrete`, `ICS_sum_mt`, `CGMgas` and `MetalsCGMgas` are persistent state, never reset per snapshot.
