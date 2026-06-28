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

## Ordering

**Advisory (enforced at the resolver's `init()`, not here):**

1. Must run in `pre_timestep` as `process_full_halo` before `sage_resolve_mergers_and_disruption`. Without this, satellites carry the sentinel `MergTime` value (999.9) from the tree load. The resolver hard-errors at `init()` if this module is absent from `pre_timestep`, and also checks ordering when both modules share the same phase.

## Notes

None.
