# `sage26_satellite_stripping`

Strips excess hot gas from Type 1 satellites and transfers stripped material to the central hot reservoir.

## SAGE26 Source

A port of SAGE26's `strip_from_satellite`, non-CGM routing (`src/model_infall.c:225-273`), called for every live Type 1 satellite whatever its hot gas (`src/core_build_model.c:493-498`). Each substep the satellite gives `-(HaloBaryonFraction * Mvir - baryons) / N` of hot gas to the central, where `HaloBaryonFraction` is the satellite's own once-rounded `reionization_modifier * BaryonFrac` (used even when zero) and `baryons` is SAGE26's float sum of stars, cold, hot, CGM, ejected, black-hole and ICS mass.

Two SAGE26 behaviours are mirrored deliberately and marked `// SAGE parity:`:

- **Asymmetric metal transfer** (`src/model_infall.c:256-270`): the satellite is debited the unclamped-gas metals capped at its own metals, but the central is credited the clamped gas times the original metallicity. When the gas clamp engages with metals above gas (metallicity capped at 1), metals are not conserved.
- **N-dependent stripped fraction** (`src/model_infall.c:238-244`): the recomputed excess is divided by the substep count every substep, so one interval strips `1 - (1 - 1/N)^N` of the initial excess, not a fixed fraction.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, before cooling and star formation in the by-galaxy order
- Receives one galaxy at a time and transfers stripped gas to the FOF central through `ctx->central_galaxy`

## Properties

- Reads: `Type`, `Mvir`, `HaloBaryonFraction`, `HotGas`, `MetalsHotGas`, `StellarMass`, `ColdGas`, `CGMgas`, `EjectedGas`, `BlackHoleMass`, `ICS`
- Writes: `HotGas`, `MetalsHotGas`

## Parameters

- `GlobalBaryonFraction` (used only when `sage26_reionization` is not configured)

## Ordering

**Advisory (no `init()` enforcement):**

1. Run before cooling modules in the galaxy-physics by-galaxy order. Stripped gas then waits in the central's hot reservoir until the next substep, matching SAGE26's interleaved strip-before-cool sequence.

## Tests

`_tests/test_unit_sage26_satellite_stripping_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.

## Notes

This module depends on reionization/infall context through `HaloBaryonFraction`. It runs as `process_by_galaxy` even though it mutates the FOF central, because SAGE26 strips each Type 1 satellite immediately before that satellite cools; the FOF central has usually already cooled in the same galaxy-major pass.
