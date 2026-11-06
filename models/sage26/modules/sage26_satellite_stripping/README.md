# `sage26_satellite_stripping`

Strips excess gas from Type 1 satellites and transfers stripped material to the FoF central: hot gas to hot gas, or with `CGMrecipeOn 1` from and to the reservoir each galaxy's regime selects.

## SAGE26 Source

A port of SAGE26's `strip_from_satellite` (`src/model_infall.c:225-273`), called for every live Type 1 satellite whatever its gas (`src/core_build_model.c:493-498`). Each substep the satellite gives `-(HaloBaryonFraction * Mvir - baryons) / N` of gas to the central, where `HaloBaryonFraction` is the satellite's own once-rounded `reionization_modifier * BaryonFrac` (used even when zero) and `baryons` is SAGE26's float sum of stars, cold, hot, CGM, ejected, black-hole and ICS mass.

The required parameter `CGMrecipeOn` (campaign decision D33) selects the reservoirs (`src/model_infall.c:247-254`, `:265-271`). With `CGMrecipeOn 1` the source is the satellite's CGM pair if the **satellite** is in Regime 0 and its hot pair otherwise, and the destination is the central's CGM pair if the **FoF central** is in Regime 0 and its hot pair otherwise, chosen independently. Classification moves no gas, so a satellite whose regime changed can be stripped from an empty reservoir while its gas sits in the other; that gas is not stripped. With `CGMrecipeOn 0` both are the hot pair. The double metallicity is formed from the source pair, and the metals of the uncapped gas before the gas cap; the gas and the source metal debit are capped independently.

Two SAGE26 behaviours are mirrored deliberately and marked `// SAGE parity:`:

- **Asymmetric metal transfer** (`src/model_infall.c:256-270`), on whichever pairs are selected: the satellite is debited the unclamped-gas metals capped at its own metals, but the central is credited the clamped gas times the original metallicity (one expression, which SAGE26's build fuses into a multiply-add, `:267`, `:270`). When the gas clamp engages with metals above gas (metallicity capped at 1), metals are not conserved.
- **N-dependent stripped fraction** (`src/model_infall.c:238-244`): the recomputed excess is divided by the substep count every substep, so one interval strips `1 - (1 - 1/N)^N` of the initial excess, not a fixed fraction.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, before cooling and star formation in the by-galaxy order
- Receives one galaxy at a time and transfers stripped gas to the FOF central through `ctx->central_galaxy`

## Properties

- Reads: `Type`, `Mvir`, `HaloBaryonFraction`, `HotGas`, `MetalsHotGas`, `StellarMass`, `ColdGas`, `CGMgas`, `MetalsCGMgas`, `EjectedGas`, `BlackHoleMass`, `ICS`, `Regime` (the satellite's and the central's, `CGMrecipeOn 1`)
- Writes: `HotGas`, `MetalsHotGas`, and with `CGMrecipeOn 1` `CGMgas`, `MetalsCGMgas` (the satellite's source pair and the central's destination pair)

## Parameters

- `GlobalBaryonFraction` (used only when `sage26_reionization` is not configured)
- `CGMrecipeOn` (required, 0 or 1; any other value, or a missing key, fails initialisation naming it; the run files hold 1, SAGE26's R4, kept at R5)

## Ordering

**Advisory (no `init()` enforcement):**

1. Run before cooling modules in the galaxy-physics by-galaxy order. Stripped gas then waits in the central's selected reservoir until the next substep, matching SAGE26's interleaved strip-before-cool sequence.

## Tests

`_tests/test_unit_sage26_satellite_stripping_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`; the `cgm_*` fixtures are `CGMrecipeOn 1`, all four source/destination combinations with the gas cap binding and not, "The c10 Group"); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. `_tests/test_unit_sage26_satellite_stripping.c` holds platform-independent tests of the four combinations, capped and not, against `CGMrecipeOn 0` on the same exact inputs (`test_cgm_stripping_source_and_destination`), and the parameter's refusal.

## Notes

This module depends on reionization/infall context through `HaloBaryonFraction`. It runs as `process_by_galaxy` even though it mutates the FOF central, because SAGE26 strips each Type 1 satellite immediately before that satellite cools; the FOF central has usually already cooled in the same galaxy-major pass.
