# `sage_supernova_feedback`

Calculates supernova reheating and ejection budgets from newly formed stellar mass.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `phase_1`, after `sage_star_formation` and before `sage_apply_star_formation_supernova`
- Receives one galaxy at a time

## Properties

- Reads: `Vvir`, `ColdGas`, `NewStellarMass`
- Writes: `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`

## Parameters

- `FeedbackReheatingEpsilon`
- `FeedbackEjectionEfficiency`

## Notes

This module only calculates transport budgets. Reservoir changes are committed by `sage_apply_star_formation_supernova`. `NewStellarMass` can be lowered here when the combined SF + reheating budget would exceed the available `ColdGas`; the apply step consumes the capped value.
