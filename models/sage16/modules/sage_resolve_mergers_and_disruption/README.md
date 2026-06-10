# `sage_resolve_mergers_and_disruption`

Single-pass SAGE merger/disruption handler for Mimic's live SAGE pathway.

## Purpose

This module reproduces SAGE's immediate in-loop merger semantics within the
module layer:
- decrement `MergTime` inside one satellite pass
- evaluate disruption versus merger from live substep state
- resolve the execution target with one-hop consumed-target redirect
- mutate the live target immediately before advancing to the next satellite
- emit merger events immediately for phase-2 `process_per_event` consumers

## Processing contract

- Supported mode: `process_full_halo`
- Expects the full halo workspace for the current FoF system
- Emits `SAGE_EVENT_MERGER` events consumed by modules such as
  `sage_quasar_mode` and `sage_starburst_feedback`

## Notes

This module is intentionally metadata-driven and module-local. Processing-mode
validation is enforced through `module_info.yaml`, and end-to-end event-chain
coverage lives in
`models/sage16/modules/sage_resolve_mergers_and_disruption/_tests/test_integration_sage_merger_event_consumers.py`.
