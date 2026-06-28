# `sage_resolve_mergers_and_disruption`

Single-pass SAGE merger/disruption handler: decrements `MergTime`, evaluates disruption vs. merger from live substep state, mutates the target immediately, and emits per-merger events for `process_per_event` consumers in the same substep phase.

## Processing Contract

- Supported mode: `process_full_halo`
- Expects the full halo workspace for the current FoF system
- Emits the `merger` event (declared under `events.emits` in `module_info.yaml`), consumed by modules such as `sage_quasar_mode` and `sage_starburst_feedback`

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage_initialise_merger_clock` must be in `pre_timestep` as `process_full_halo` — without it, satellites carry the sentinel `MergTime` value (999.9) from the tree load and the module hard-errors on the first satellite it encounters at runtime.

## Properties

- Reads: `Type`, `Mvir`, `deltaMvir`, `dT`, `SnapNum`, `Len`, `CentralHalo`, `StellarMass`, `ColdGas`, `MergTime`, `BulgeMass`, `MetalsBulgeMass`, `MetalsStellarMass`, `TimeOfLastMinorMerger`, `TimeOfLastMajorMerger`, `ICS`, `MetalsICS`
- Writes: `MergTime`, `Type` (sets to 3 after merge/disruption), `BulgeMass`, `MetalsBulgeMass`, `TimeOfLastMinorMerger`, `TimeOfLastMajorMerger`, plus all fields modified by `mimic_sage_merge_transfer` and `mimic_sage_disruption_transfer`

## Parameters

- `ThresholdMajorMerger`
- `ThresholdSatDisruption`

## Notes

Processing-mode validation is enforced through `module_info.yaml`. End-to-end event-chain coverage lives in `_tests/test_integration_sage_merger_event_consumers.py`.
