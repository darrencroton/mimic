# `sage_supernova_feedback`

Calculates supernova reheating and ejection budgets from newly formed stellar mass.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `phase_1`, after `sage_star_formation` and before `sage_apply_star_formation_supernova`
- Receives one galaxy at a time

## Properties

- Reads: `Vvir`, `ColdGas`, `NewStellarMass`
- Writes: `SupernovaReheatedMass`, `SupernovaEjectedMass`

## Parameters

- `FeedbackReheatingEpsilon`
- `FeedbackEjectionEfficiency`

## Notes

This module only calculates transport budgets. Reservoir changes are committed by `sage_apply_star_formation_supernova`.
