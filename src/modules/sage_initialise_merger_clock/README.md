# `sage_initialise_merger_clock`

Initialises or updates merger clocks for satellites and handles reset behavior for central promotions and orphan force-merge cases.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace because merger-clock state depends on central/satellite relationships

## Properties

- Reads: `Type`, `CentralHalo`, `Len`, `Mvir`, `Rvir`, `Vvir`, `StellarMass`, `ColdGas`
- Writes: `MergTime`

## Parameters

None.

## Notes

Keep this module before merger resolution so `sage_resolve_mergers_and_disruption` sees current merger-clock values.
