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

## Ordering

**Advisory (no `init()` enforcement):**

1. Run in `pre_timestep`, before the substep baryonic physics phases — star formation and disk instability both read `DiskScaleRadius` during the substep. Satellite radii are not updated here; they retain the value from their last central phase (SAGE parity).

## Notes

None.
