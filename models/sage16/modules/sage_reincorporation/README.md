# `sage_reincorporation`

Returns gas from the ejected reservoir to the hot reservoir in systems where reincorporation is allowed by the module prescription.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `galaxy_physics`, before cooling and star formation
- Receives the full FoF workspace and acts on the FoF central

## Properties

- Reads: `HaloNr`, `SnapNum`, `Type`, `Vvir`, `Rvir`, `dT`, `EjectedGas`, `MetalsEjectedGas`
- Writes: `EjectedGas`, `MetalsEjectedGas`, `HotGas`, `MetalsHotGas`

## Parameters

- `ReIncorporationFactor`

## Ordering

**Advisory (no `init()` enforcement):**

1. Run before cooling modules in the galaxy-physics phase so reincorporated gas participates in the same substep's hot-halo cooling calculation.

## Notes

None.
