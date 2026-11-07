# `sage26_prepare_infall_budget`

Prepares the baryon infall budget for the current FoF system by consolidating satellite material where required and calculating the gas available for infall.

## SAGE26 Source

A port of SAGE26's `infall_recipe` (`src/model_infall.c:67-213`), with both settings of the required parameter `CGMrecipeOn` (campaign decision D33):

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

## Properties

- Reads: `Type`, `Mvir`, `HaloBaryonFraction`, `StellarMass`, `BlackHoleMass`, `ColdGas`, `HotGas`, `MetalsHotGas`, `ICS`, `MetalsICS`, `EjectedGas`, `MetalsEjectedGas`, `CGMgas`, `MetalsCGMgas`, `ICS_accrete`, `ICS_sum_mt`, `Regime` (the central's, `CGMrecipeOn 1`)
- Writes: `HaloBaryonFraction`, `InfallingGas`, `ICS`, `MetalsICS`, `EjectedGas`, `MetalsEjectedGas`, `HotGas`, `MetalsHotGas`, `CGMgas`, `MetalsCGMgas`, `ICS_disrupt`, `ICS_accrete`, `ICS_sum_mt`

## Parameters

- `GlobalBaryonFraction`
- `CGMrecipeOn` (required, 0 or 1; any other value, or a missing key, fails initialisation naming it; the run files hold 1, SAGE26's R4, kept through R6)

## Tests

`_tests/test_unit_sage26_prepare_infall_budget_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`; the `cgm_*` fixtures are `CGMrecipeOn 1`, "The c10 Group"); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_prepare_infall_budget.c` holds platform-independent tests of both settings on the same exact inputs (`test_cgm_central_migration_by_regime`) and of the parameter's refusal (`test_cgm_recipe_on_refused`).

## Notes

`InfallingGas` is a transport field consumed by `sage26_apply_infall` during substeps. `ICS_disrupt`, `ICS_accrete`, `ICS_sum_mt`, `CGMgas` and `MetalsCGMgas` are persistent state, never reset per snapshot.
