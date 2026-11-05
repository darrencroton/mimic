# `sage26_calculate_supernova_feedback`

Calculates supernova reheating and ejection budgets from newly formed stellar mass, as SAGE26's non-Karpov `compute_sn_feedback()` does (`src/model_starformation_and_feedback.c:732-889` in the staged SAGE26 source, `SupernovaRecipeOn 1`), for either value of the required parameter `FIREmodeOn`.

With `FIREmodeOn 0` (SAGE26's R0-R2, the fixed-epsilon Croton et al. 2016 feedback):

- `reheated = FeedbackReheatingEpsilon * stars` (`:767`);
- when `stars + reheated` exceeds `ColdGas`, both are scaled together by `ColdGas / (stars + reheated)` (`:775-779`), against the full sum, before any recycling;
- `ejected = (FeedbackEjectionEfficiency * (EtaSNcode * EnergySNcode) / (Vvir * Vvir) - FeedbackReheatingEpsilon) * stars` with the forming galaxy's **own** `Vvir`, also for satellites and orphans, squared in float as SAGE26's float member is (`:783`, `:795-798`); zero for a non-positive own `Vvir` (`:876-878`), and floored at zero (`:880-882`). `MassLoading` is not written.

With `FIREmodeOn 1` (SAGE26's R3), through the helpers of `shared/sage_fire_feedback_physics.h`:

- the Muratov et al. (2015) scaling `(1 + z)^1.25 * (max(Vvir, 1) / 60)^beta`, `beta` -3.2 below 60 km/s and -1.0 at and above, from the forming galaxy's **own** float `Vvir` widened to double and the redshift of its **stored** snapshot, `ZZ[SnapNum]` (`:743-753`), never the current halo snapshot's or a substep's; zero for a non-positive `Vvir` or a negative redshift. A stored `SnapNum` outside the redshift table is refused with an error naming it and the table size (SAGE26 reads `ZZ` without a bounds check; campaign decision D35);
- the reheating loading `FeedbackReheatingEpsilon * scaling`, capped at `2 * EtaSNcode * EnergySNcode / Vvir^2` in double (`capped_eta_reheat`, `src/model_misc.h:141-149`; unchanged for a non-positive `Vvir`), stored as the float `MassLoading` **before** the renormalisation, also when no stars form (`:762-764`), and `reheated = eta * stars` (`:765`);
- the same renormalisation (`:775-779`);
- the Hirschmann et al. (2016) energy-based ejection from the renormalised stars and reheated mass: the coupling `FeedbackEjectionEfficiency * scaling` capped at 2 (`sn_energy_coupling`, `src/model_misc.h:118-124`), `E_FB = coupling * 0.5 * stars * (EtaSNcode * EnergySNcode)`, `E_lift = 0.5 * reheated * Vvir * Vvir` and `ejected = (E_FB - E_lift) / (0.5 * Vvir * Vvir)` when `E_FB > E_lift`, else 0 (`:786-792`), in double; zero for a non-positive own `Vvir` (`:876-878`), then the same floor at zero (`:880-882`), which the FIRE ejection never reaches below zero.

The 1 km/s floor applies to the scaling only, never to the loading cap or the energies. The two caps are distinct: the loading is capped by the whole supernova budget, the ejection's coupling by the scaled efficiency, so the reheating can consume all the coupled energy and leave nothing to eject. SAGE26's object computes the high-velocity term `pow(x, -1.0)` as the division `1.0 / x`, which the package writes as such (the shared header and the oracle README's c09 table give the evidence).

The scaling and the ejection use the forming galaxy's own `Vvir` and stored `SnapNum`; the reheated gas still goes to its FoF central's hot gas and the ejection is capped against that central's hot gas, in `sage26_apply_star_formation_supernova`. `EtaSNcode` and `EnergySNcode` are SAGE26's code constants (`src/core_init.c:102-103`), derived by the shared helpers in `shared/sage_sf_feedback_physics.h`. `FeedbackReheatingEpsilon` is 2.9 and `FeedbackEjectionEfficiency` 0.3 on every rung (`par/R3.par:69-70`).

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_star_formation` and before `sage26_apply_star_formation_supernova`
- Receives one galaxy at a time

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_calculate_star_formation` must precede this module in the same substep phase — SN feedback reads `NewStellarMass` written by SF; wrong order applies stale values from the previous substep.
2. `sage26_apply_star_formation_supernova` must be present somewhere in the pipeline — without it, `SupernovaReheatedMass` and `SupernovaEjectedMass` are computed each substep but never committed to galaxy reservoirs (silent output loss).

## Properties

- Reads: `Vvir` (the forming galaxy's own), `ColdGas`, `NewStellarMass`, and with `FIREmodeOn 1` `SnapNum` (the stored snapshot)
- Writes: `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass`, and with `FIREmodeOn 1` `MassLoading`

## Parameters

- `FeedbackReheatingEpsilon`
- `FeedbackEjectionEfficiency`
- `FIREmodeOn` (required, 0 or 1, no default): a missing key fails initialisation and any other value is refused, naming the parameter

## Notes

This module only calculates transport budgets. Its oracle test (`_tests/test_unit_sage26_calculate_supernova_feedback_oracle.c`) runs it on the state SAGE26's `compute_sn_feedback` receives in each of its fixtures and asserts the budgets, `MassLoading` and the FIRE locals bit for bit; the chained oracle test of `sage26_calculate_star_formation` asserts the whole chain. Reservoir changes are committed by `sage26_apply_star_formation_supernova`. `NewStellarMass` can be lowered here when the combined SF + reheating budget would exceed the available `ColdGas`; the apply step consumes the capped value.
