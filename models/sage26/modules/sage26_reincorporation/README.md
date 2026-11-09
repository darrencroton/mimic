# `sage26_reincorporation`

Returns gas from the ejected reservoir of a FoF central whose virial velocity exceeds a critical velocity, following SAGE26's `reincorporate_gas`. The gas goes to the central's hot reservoir or, with `CGMrecipeOn 1` and a CGM-regime central, to its CGM.

## Physics

The critical velocity is `Vcrit = 354.53 * ReIncorporationFactor` km/s, from the escape-velocity argument of Croton et al. (2016), with SAGE26's ejecta velocity of about 501 km/s. A central with `Vcrit > 0`, `Vvir > Vcrit`, `Rvir > 0` and `EjectedGas > 0` reincorporates `(Vvir / Vcrit - 1) * EjectedGas / (Rvir / Vvir) * dt`, capped at its ejected gas, moving metals at the ejected-gas metallicity. The destination is chosen by `mimic_sage_add_gas_to_hot_reservoir` (`shared/sage_cgm_routing.h`): the CGM pair with `CGMrecipeOn 1` and `Regime` 0, the hot pair otherwise. Only the destination depends on `CGMrecipeOn`; the amount, guards, cap and metallicity do not.

The substep `dt` is the central's `dT` divided by the substep count (`shared/time_parity.h`). At the core's first-snapshot boundary state (stored `SnapNum` below 0 with `dT` not positive) the module writes nothing; any other non-positive `dT` makes `process()` fail with an error naming the halo.

## Processing Contract

- Supported mode: `process_full_halo`, expected in `galaxy_physics` before cooling and star formation
- Receives the full FoF workspace and acts on the FoF central only

## Ordering

`init()` checks only its two parameters and refuses no placement or partner. Advisory:

- Run after `sage26_apply_infall` (as SAGE26 does and the shipped run files order them) and before the cooling modules, so the reincorporated gas cools in the same substep.
- `CGMrecipeOn 1` without `sage26_determine_regime` is not refused: `Regime` keeps its creation value -1 and the gas goes to the hot reservoir.

## Properties

- Reads: `HaloNr`, `SnapNum`, `Type`, `Vvir`, `Rvir`, `dT`, `EjectedGas`, `MetalsEjectedGas`, `Regime` (with `CGMrecipeOn 1`)
- Writes: `EjectedGas`, `MetalsEjectedGas`, and `HotGas` and `MetalsHotGas` or (with `CGMrecipeOn 1` and `Regime` 0) `CGMgas` and `MetalsCGMgas`

## Parameters

- `ReIncorporationFactor` (required, in [0, 10]): scales the critical velocity. The shipped run files use 0.19. At 0 the module writes nothing, as SAGE26 skips reincorporation then.
- `CGMrecipeOn` (required, 0 or 1): 1 (the shipped value) routes a CGM-regime central's gas to its CGM; 0 always to its hot gas.

A missing key or a value outside the accepted range fails initialisation naming the parameter.

## Notes

The module returns early when `EjectedGas <= 0`, where SAGE26 performs a zero-mass transfer; the only possible difference is the sign bit of an exact-zero destination field.

## Tests

- `_tests/test_unit_sage26_reincorporation.c`: the reincorporated amount, the `Vcrit` threshold, the cap, the metallicity, the central-only restriction, both destinations, substeps and the parameter refusals.
- `_tests/test_integration_sage26_reincorporation.py`: the module in full runs, alone and with the infall modules, with non-negativity and metallicity-range checks on the output.
