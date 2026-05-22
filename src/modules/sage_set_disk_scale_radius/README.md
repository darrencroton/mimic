# `sage_set_disk_scale_radius`

Computes disk scale radii from halo spin and virial properties for resolved galaxies.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace

## Properties

- Reads: `Type`, `Spin`, `Vvir`, `Rvir`
- Writes: `DiskScaleRadius`

## Parameters

None.

## Notes

Disk radii are used by star formation and disk instability modules, so this should run before phase-1 baryonic physics.
