# `sage_satellite_stripping`

Strips excess hot gas from Type 1 satellites and transfers stripped material to the central hot reservoir.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, before cooling and star formation in the by-galaxy order
- Receives one galaxy at a time and transfers stripped gas to the FOF central through `ctx->central_galaxy`

## Properties

- Reads: `Type`, `Mvir`, `HaloBaryonFraction`, `HotGas`, `MetalsHotGas`, `StellarMass`, `ColdGas`, `EjectedGas`, `BlackHoleMass`, `ICS`
- Writes: `HotGas`, `MetalsHotGas`

## Parameters

- `GlobalBaryonFraction`

## Ordering

**Advisory (no `init()` enforcement):**

1. Run before cooling modules in the galaxy-physics by-galaxy order. Stripped gas then waits in the central's hot reservoir until the next substep, matching SAGE's interleaved strip-before-cool sequence.

## Notes

This module depends on reionization/infall context through `HaloBaryonFraction`. It runs as `process_by_galaxy` even though it mutates the FOF central, because SAGE strips each Type 1 satellite immediately before that satellite cools; the FOF central has usually already cooled in the same galaxy-major pass.
