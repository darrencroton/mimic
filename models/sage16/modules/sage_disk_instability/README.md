# `sage_disk_instability`

Applies a disk stability criterion, transfers unstable stellar mass to the bulge, and sets a gas-fraction trigger for downstream quasar-mode and starburst modules.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after disk radii and star-formation reservoir updates are available
- Receives one galaxy at a time

## Properties

- Reads: `Vmax`, `ColdGas`, `StellarMass`, `MetalsStellarMass`, `BulgeMass`, `MetalsBulgeMass`, `DiskScaleRadius`
- Writes: `BulgeMass`, `MetalsBulgeMass`, `UnstableDiskGasFraction`

## Parameters

- `StarFormingDiskFactor`

## Notes

The trigger field is a transport property. Place disk-instability consumers after this module in the same phase and clear the trigger after consumers have run.

Disk-instability stellar transfer is a redistribution from disk stars into the bulge, not a change to total stellar mass. The shared physics helper caps the stellar and metal transfer to the available disk reservoirs before applying the update, then silently snaps tolerance-level component overshoot to the physical boundary. Material overshoots still warn because they indicate a real invariant violation.
