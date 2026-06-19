# `sage_calculate_supernova_feedback`

Calculates supernova reheating and ejection budgets from newly formed stellar mass.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage_calculate_star_formation` and before `sage_apply_star_formation_supernova`
- Receives one galaxy at a time

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage_calculate_star_formation` must precede this module in the same substep phase — SN feedback reads `NewStellarMass` written by SF; wrong order applies stale values from the previous substep.
2. `sage_apply_star_formation_supernova` must be present somewhere in the pipeline — without it, `SupernovaReheatedMass` and `SupernovaEjectedMass` are computed each substep but never committed to galaxy reservoirs (silent output loss).

## Properties

- Reads: `Vvir`, `ColdGas`, `NewStellarMass`
- Writes: `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`

## Parameters

- `FeedbackReheatingEpsilon`
- `FeedbackEjectionEfficiency`

## Notes

This module only calculates transport budgets. Reservoir changes are committed by `sage_apply_star_formation_supernova`. `NewStellarMass` can be lowered here when the combined SF + reheating budget would exceed the available `ColdGas`; the apply step consumes the capped value.
