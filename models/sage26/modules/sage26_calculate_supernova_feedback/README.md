# `sage26_calculate_supernova_feedback`

Calculates supernova reheating and ejection budgets from newly formed stellar mass, as SAGE26's non-FIRE, non-Karpov `compute_sn_feedback()` does on R0 (`src/model_starformation_and_feedback.c:732-889` in the staged SAGE26 source):

- `reheated = FeedbackReheatingEpsilon * stars` (`:767`);
- when `stars + reheated` exceeds `ColdGas`, both are scaled together by `ColdGas / (stars + reheated)` (`:775-779`), against the full sum, before any recycling;
- `ejected = (FeedbackEjectionEfficiency * (EtaSNcode * EnergySNcode) / (Vvir * Vvir) - FeedbackReheatingEpsilon) * stars` with the forming galaxy's **own** `Vvir`, also for satellites and orphans, squared in float as SAGE26's float member is (`:783`, `:795-798`); zero for a non-positive own `Vvir` (`:876-878`), and floored at zero (`:880-882`).

`EtaSNcode` and `EnergySNcode` are SAGE26's code constants (`src/core_init.c:102-103`), derived by the shared helpers in `shared/sage_sf_feedback_physics.h`. `MassLoading` is not written on R0 (SAGE26 writes it only in the FIRE branch). On R0 `FeedbackReheatingEpsilon` is 2.9 (`input/millennium.par:69`).

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_star_formation` and before `sage26_apply_star_formation_supernova`
- Receives one galaxy at a time

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_calculate_star_formation` must precede this module in the same substep phase — SN feedback reads `NewStellarMass` written by SF; wrong order applies stale values from the previous substep.
2. `sage26_apply_star_formation_supernova` must be present somewhere in the pipeline — without it, `SupernovaReheatedMass` and `SupernovaEjectedMass` are computed each substep but never committed to galaxy reservoirs (silent output loss).

## Properties

- Reads: `Vvir` (the forming galaxy's own), `ColdGas`, `NewStellarMass`
- Writes: `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`

## Parameters

- `FeedbackReheatingEpsilon`
- `FeedbackEjectionEfficiency`

## Notes

This module only calculates transport budgets; the chained oracle test of `sage26_calculate_star_formation` asserts them bit for bit against SAGE26. Reservoir changes are committed by `sage26_apply_star_formation_supernova`. `NewStellarMass` can be lowered here when the combined SF + reheating budget would exceed the available `ColdGas`; the apply step consumes the capped value.
