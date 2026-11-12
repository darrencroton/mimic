# `sage26_apply_infall`

Adds the snapshot's infall budget, prepared by `sage26_prepare_infall_budget`, to the FoF central in equal shares over the substeps. The share goes to the central's hot reservoir, or with `CGMrecipeOn 1` and a central in the CGM regime (`Regime` 0) to its CGM reservoir; a negative share (net mass loss) is taken from the ejected reservoir first.

## Physics

This implements SAGE26's `add_infall_to_hot`, called each substep with `InfallingGas / num_substeps`. A negative share is taken first from `EjectedGas`, with metals at the ejected gas's metallicity; whatever remains (including the negative `EjectedGas` left when the ejected gas runs out) is applied to the selected reservoir. There a negative amount removes metals at the reservoir's metallicity while they are positive, flooring them at zero, and if the gas becomes negative both gas and metals are cleared. A positive share adds pristine gas and no metals. The reservoir not selected is untouched.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `galaxy_physics`, after `sage26_prepare_infall_budget`
- Receives the full FoF workspace and acts on the FoF central only

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_prepare_infall_budget` must be in `pre_timestep` as `process_full_halo` — without it `InfallingGas` keeps its per-snapshot reset value 0 and no gas is added.

**Not refused:** this module's own position or a second entry (only the budget's placement is checked); `CGMrecipeOn 1` without `sage26_determine_regime`, in which case the central's `Regime` keeps its creation value -1 and every share goes to the hot reservoir.

## Properties

- Reads: `Type`, `InfallingGas`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `CGMgas`, `MetalsCGMgas`, `Regime` (`CGMrecipeOn 1`)
- Writes: `HotGas` and `MetalsHotGas`, or (`CGMrecipeOn 1`, `Regime` 0) `CGMgas` and `MetalsCGMgas`; `EjectedGas`, `MetalsEjectedGas`

## Parameters

- `CGMrecipeOn`: required, 0 or 1; 1 (the run files' value) routes the share by the central's `Regime`, 0 always to the hot reservoir. Any other value, or a missing key, fails initialisation naming the parameter.

## Notes

`InfallingGas` is a snapshot-scoped transport property prepared before the substeps. Keep this module before cooling so the new hot gas can cool in the same substep.

## Tests

- `_tests/test_unit_sage26_apply_infall.c`: registration and lifecycle, positive, negative and zero infall over one to the maximum number of substeps (with a total independent of the substep count), each case at `CGMrecipeOn 1` in both regimes and at `CGMrecipeOn 0`, and a workspace with no central.
- `_tests/test_integration_sage26_apply_infall.py`: a `CGMrecipeOn 0` infall-only run (reionization, the budget, the application) at the default and at a non-default substep count, checking that every FoF group's baryons reach `HaloBaryonFraction * Mvir`, that centrals a negative budget exhausted hold no hot or ejected gas, and that nothing leaks.
- The `CGMrecipeOn` refusals are covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c`.
