# `sage_clear_disk_instability_triggers`

Clears the `UnstableDiskGasFraction` transport trigger after downstream disk-instability consumers have used it.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: after `sage_quasar_mode` and `sage_starburst_feedback` have consumed disk-instability triggers
- Receives one galaxy at a time

## Properties

- Writes: `UnstableDiskGasFraction`

## Parameters

None.

## Notes

This module is intentionally small. If future logic is added, add module-local tests before changing the contract.
