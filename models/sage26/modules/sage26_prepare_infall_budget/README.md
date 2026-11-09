# `sage26_prepare_infall_budget`

Prepares the baryon infall budget for each FoF system, following SAGE26's `infall_recipe`: it consolidates satellite ejected gas and intracluster stars (ICS) onto the central, computes the gas available for infall into `InfallingGas`, and, depending on `CGMrecipeOn`, moves the central's gas between its hot and circumgalactic (CGM) reservoirs.

## Physics

- The group's stellar, black-hole, cold, hot, ejected, ICS and CGM masses (and the ejected, ICS and CGM metals) are summed over every galaxy of the FoF.
- Each satellite surrenders its ejected gas and ICS to the central; hot gas is not consolidated. A satellite with `ICS > 0` also hands its ICS to the central's `ICS_accrete` (with its `ICS_sum_mt`), and its three ICS history fields are zeroed.
- The budget is `HaloBaryonFraction * Mvir - (stars + cold + hot + ejected + BH + ICS + CGM)`, with `Mvir` the central's halo mass and `HaloBaryonFraction` written by `sage26_reionization` (or `GlobalBaryonFraction` when that is unset).
- The central's pooled ejected gas and ICS are clamped: metals capped to mass, a negative mass zeroed with its metals, negative metals floored at zero.
- `CGMrecipeOn 1`: satellites keep their CGM across snapshots (losing it only through stripping), and the central migrates only its own gas by its `Regime`, set this snapshot by `sage26_determine_regime`: `Regime` 0 (the CGM regime) moves its hot gas and metals into its CGM, any other regime moves its CGM into its hot gas. Satellite CGM is counted in the budget but not added to the central.
- `CGMrecipeOn 0`: satellites' CGM is zeroed and the group's CGM totals are added to the central's hot gas, without clearing the central's CGM (SAGE26's own behaviour on this branch, marked `SAGE parity:`; inert while CGM stays zero).
- With either setting, the central's CGM is then clamped in the same order as the ejected gas.

## Processing Contract

- Supported mode: `process_full_halo`, expected in `pre_timestep`
- Receives the full FoF workspace and writes the central's budget

## Ordering

- `sage26_reionization` is optional; when it is configured in `pre_timestep`, `init()` refuses a run in which it does not precede this module (this module outside `pre_timestep` is refused too). Without it, a central whose `HaloBaryonFraction` holds its unset value -1.0 takes `GlobalBaryonFraction`.
- `init()` does not check this module's own placement or entry count.
- `CGMrecipeOn 1` without `sage26_determine_regime` is not refused: `Regime` keeps its creation value -1 and every central takes the hot-regime migration. The classifier's own `init()` refuses a `pre_timestep` entry that does not precede this module.
- Without `sage26_apply_infall`, the budget is written to `InfallingGas` and never applied.

## Properties

- Reads: `Type`, `Mvir`, `HaloBaryonFraction`, `StellarMass`, `BlackHoleMass`, `ColdGas`, `HotGas`, `MetalsHotGas`, `ICS`, `MetalsICS`, `EjectedGas`, `MetalsEjectedGas`, `CGMgas`, `MetalsCGMgas`, `ICS_accrete`, `ICS_sum_mt`, `Regime` (the central's, with `CGMrecipeOn 1`)
- Writes: `HaloBaryonFraction`, `InfallingGas`, `ICS`, `MetalsICS`, `EjectedGas`, `MetalsEjectedGas`, `HotGas`, `MetalsHotGas`, `CGMgas`, `MetalsCGMgas`, `ICS_disrupt`, `ICS_accrete`, `ICS_sum_mt`

## Parameters

- `GlobalBaryonFraction` (required, in (0, 1]): the cosmic baryon fraction, used for a central whose `HaloBaryonFraction` is unset. The shipped run files use 0.17.
- `CGMrecipeOn` (required, 0 or 1; any other value or a missing key fails initialisation): 1 enables the CGM reservoir and the regime-dependent migration; 0 folds all CGM into the central's hot gas. The shipped run files use 1.

## Notes

`InfallingGas` is a transport field consumed by `sage26_apply_infall` during the substeps. `ICS_disrupt`, `ICS_accrete`, `ICS_sum_mt`, `CGMgas` and `MetalsCGMgas` are persistent state, never reset per snapshot.

## Tests

- `_tests/test_unit_sage26_prepare_infall_budget.c`: registration and lifecycle, the parameter refusals, the budget and satellite pooling, and both `CGMrecipeOn` settings.
- `_tests/test_integration_sage26_prepare_infall_budget.py`: the module in a full run with `sage26_reionization`, and alongside `sage26_satellite_stripping` and `sage26_apply_infall`.
