# `sage_satellite_stripping`

Strips excess hot gas from Type 1 satellites and transfers stripped material to the central hot reservoir.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `phase_1`, before cooling and star formation
- Receives the full FoF workspace because gas is moved between satellites and the central

## Properties

- Reads: `Type`, `Mvir`, `HaloBaryonFraction`, `HotGas`, `MetalsHotGas`, `StellarMass`, `ColdGas`, `EjectedGas`, `BlackHoleMass`, `ICS`
- Writes: `HotGas`, `MetalsHotGas`

## Parameters

- `GlobalBaryonFraction`

## Notes

This module depends on reionization/infall context through `HaloBaryonFraction`.
