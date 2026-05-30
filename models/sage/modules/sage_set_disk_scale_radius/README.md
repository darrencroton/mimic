# `sage_set_disk_scale_radius`

Computes disk scale radii from halo spin and virial properties for the FOF Type 0 central.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace
- Updates only Type 0 galaxies; Type 1 satellites keep the radius inherited from their last central phase

## Properties

- Reads: `Type`, `Spin`, `Vvir`, `Rvir`
- Writes: `DiskScaleRadius`

## Parameters

None.

## Notes

Disk radii are used by star formation and disk instability modules, so this should run before phase-1 baryonic physics. This matches SAGE's central-only radius update: satellites are not recomputed after infall.
