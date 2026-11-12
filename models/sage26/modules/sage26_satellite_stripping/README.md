# `sage26_satellite_stripping`

Strips excess gas from Type 1 satellites and transfers it to the FoF central, following SAGE26's `strip_from_satellite`: hot gas to hot gas, or with `CGMrecipeOn 1` from and to the reservoir each galaxy's regime selects.

## Physics

Every live Type 1 satellite is called, whatever its gas. Each substep it gives `-(HaloBaryonFraction * Mvir - baryons) / N` of gas to the central when that is positive, where `HaloBaryonFraction` is the satellite's own (from `sage26_reionization`, used even when zero) and `baryons` is the sum of its stars, cold, hot, CGM, ejected, black-hole and ICS mass. The stripped gas is capped at the source reservoir and carries metals at the source's metallicity.

With `CGMrecipeOn 1` the source is the satellite's CGM if the satellite is in `Regime` 0 and its hot gas otherwise, and the destination is the central's CGM if the FoF central is in `Regime` 0 and its hot gas otherwise, chosen independently. Classification moves no gas, so a satellite whose regime changed can be stripped from an empty reservoir while its gas sits in the other. With `CGMrecipeOn 0` both are the hot gas.

Two SAGE26 behaviours are kept deliberately (`SAGE parity:` in the source):

- **Asymmetric metal transfer.** The satellite is debited the metals of the uncapped gas, capped at its own metals, while the central is credited the capped gas times the original metallicity; when the gas cap engages with metals above gas, metals are not conserved.
- **Substep-dependent stripped fraction.** The excess is recomputed and divided by the substep count every substep, so one snapshot interval strips `1 - (1 - 1/N)^N` of the initial excess rather than a fixed fraction.

## Processing Contract

- Supported mode: `process_by_galaxy`, expected in `galaxy_physics` before cooling and star formation
- Receives one galaxy at a time and transfers the stripped gas to the FoF central through `ctx->central_galaxy`. It runs by galaxy, though it changes the central, because SAGE26 strips each satellite immediately before that satellite cools.

## Ordering

`init()` checks only its two parameters and refuses no placement or partner. Advisory:

- Run before the cooling modules in the by-galaxy order; the stripped gas then waits in the central's reservoir until the next substep.
- Without `sage26_reionization`, a satellite whose `HaloBaryonFraction` holds its unset value -1.0 uses `GlobalBaryonFraction`.
- `CGMrecipeOn 1` without `sage26_determine_regime` is not refused: every `Regime` keeps its creation value -1, so the gas moves hot to hot.

## Properties

- Reads: `Type`, `Mvir`, `HaloBaryonFraction`, `HotGas`, `MetalsHotGas`, `StellarMass`, `ColdGas`, `CGMgas`, `MetalsCGMgas`, `EjectedGas`, `BlackHoleMass`, `ICS`, `Regime` (the satellite's and the central's, with `CGMrecipeOn 1`)
- Writes: `HotGas`, `MetalsHotGas`, and with `CGMrecipeOn 1` `CGMgas` and `MetalsCGMgas` (the satellite's source and the central's destination)

## Parameters

- `GlobalBaryonFraction` (required, in (0, 1]): used where the satellite's `HaloBaryonFraction` is unset. The shipped run files use 0.17.
- `CGMrecipeOn` (required, 0 or 1): 1 (the shipped value) selects source and destination by regime; 0 strips hot gas to hot gas.

A missing key or a value outside the accepted range fails initialisation naming the parameter.

## Tests

- `_tests/test_unit_sage26_satellite_stripping.c`: the stripping threshold, the `1 - (1 - 1/N)^N` fraction, mass conservation, the asymmetric metal transfer, and the four regime source/destination combinations.
- `_tests/test_integration_sage26_satellite_stripping.py`: the infall pipeline run without and with the module, checking that satellites and orphans hold less hot gas, centrals more, and the total hot gas is conserved to within the infall budget's rebalancing.
- The `CGMrecipeOn` refusals are covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c`.
