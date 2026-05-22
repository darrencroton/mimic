# `sage_disk_instability`

Applies a disk stability criterion, transfers unstable stellar mass to the bulge, and sets a gas-fraction trigger for downstream quasar-mode and starburst modules.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `phase_1`, after disk radii and star-formation reservoir updates are available
- Receives one galaxy at a time

## Properties

- Reads: `Type`, `Vmax`, `ColdGas`, `StellarMass`, `BulgeMass`, `DiskScaleRadius`
- Writes: `StellarMass`, `BulgeMass`, `MetalsStellarMass`, `MetalsBulgeMass`, `UnstableDiskGasFraction`

## Parameters

- `StarFormingDiskFactor`

## Notes

The trigger field is a transport property. Place disk-instability consumers after this module in the same phase and clear the trigger after consumers have run.
