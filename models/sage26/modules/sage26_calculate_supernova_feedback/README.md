# `sage26_calculate_supernova_feedback`

Calculates the gas reheated and ejected by supernovae from the stars formed this substep, and stores them in `SupernovaReheatedMass` and `SupernovaEjectedMass` for `sage26_apply_star_formation_supernova`. `FIREmodeOn` selects fixed-efficiency feedback (Croton et al. 2016) or feedback calibrated on the FIRE simulations, with the Muratov et al. (2015) mass loading and the Hirschmann et al. (2016) energy-driven ejection.

## Physics

Both modes use the forming galaxy's own `Vvir`, also for satellites and orphans, and the supernova energy per unit stellar mass `EtaSNcode * EnergySNcode` (`shared/sage_sf_feedback_physics.h`). In both, when `stars + reheated` exceeds `ColdGas` the two are scaled down together by `ColdGas / (stars + reheated)` before the ejection is formed, and `NewStellarMass` is overwritten with the reduced stars. A non-positive `Vvir` gives no ejection, and the ejection is floored at zero.

- `FIREmodeOn 0`: `reheated = FeedbackReheatingEpsilon * stars` and `ejected = (FeedbackEjectionEfficiency * EtaSNcode * EnergySNcode / Vvir^2 - FeedbackReheatingEpsilon) * stars`.
- `FIREmodeOn 1` (`shared/sage_fire_feedback_physics.h`): the scaling `(1 + z)^1.25 * (max(Vvir, 1) / 60)^beta`, with `beta` -3.2 below 60 km/s and -1.0 at and above, and `z` the redshift of the galaxy's stored snapshot (zero scaling for a non-positive `Vvir` or a negative redshift). The reheating loading `FeedbackReheatingEpsilon * scaling` is capped at `2 * EtaSNcode * EnergySNcode / Vvir^2`, the whole supernova budget, stored in `MassLoading` on every call (also with no stars), and `reheated = loading * stars`. The ejection is energy-driven: with the coupling `min(FeedbackEjectionEfficiency * scaling, 2)`, `E_FB = coupling * 0.5 * stars * EtaSNcode * EnergySNcode` and `E_lift = 0.5 * reheated * Vvir^2`, `ejected = (E_FB - E_lift) / (0.5 * Vvir^2)` when `E_FB > E_lift`, else 0. The reheating can therefore consume all the coupled energy and leave nothing to eject.

The 1 km/s floor applies to the scaling only. The reheated gas goes to the FoF central's reservoir, and the ejection is capped against it, in `sage26_apply_star_formation_supernova`.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_star_formation` and before `sage26_apply_star_formation_supernova`
- Receives one galaxy at a time
- A feedback-free-burst row (`FFBRegime` 1) is skipped and nothing on it is written: `sage26_ffb_star_formation` runs that row's transaction
- With `FIREmodeOn 1`: in the core's first-snapshot boundary state (stored `SnapNum` negative, `dT` not positive) the module writes nothing, `MassLoading` included; a stored `SnapNum` outside the run's redshift table is an error naming it

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_apply_star_formation_supernova` must be configured somewhere in the pipeline — without it the budgets are never committed (silent output loss).
2. When `sage26_calculate_star_formation` is configured, it must precede this module as `process_by_galaxy` in the same substep phase — otherwise the feedback reads a stale `NewStellarMass` from the previous substep.

**Not refused:** `sage26_calculate_star_formation` absent (the module uses whatever `NewStellarMass` holds); the apply step's position (its own `init()` requires this module before it).

## Properties

- Reads: `FFBRegime`, `Vvir`, `ColdGas`, `NewStellarMass`, and with `FIREmodeOn 1` `SnapNum` and `dT`; `HaloNr` and `Type` in log messages only
- Writes: `NewStellarMass` (when renormalised), `SupernovaReheatedMass`, `SupernovaEjectedMass`, and with `FIREmodeOn 1` `MassLoading`

## Parameters

- `FeedbackReheatingEpsilon`: the reheating efficiency (mass reheated per unit stellar mass, before the FIRE scaling); required, finite and >= 0; the run files hold 2.9.
- `FeedbackEjectionEfficiency`: the fraction of supernova energy available for ejection (before the FIRE scaling); required, within [0, 100]; the run files hold 0.3.
- `FIREmodeOn`: required, 0 (fixed efficiency) or 1 (FIRE scaling, the run files' value). Any other value, or a missing key, fails initialisation naming the parameter. `sage26_starburst_feedback` reads the same parameter for every burst.

## Tests

- `_tests/test_unit_sage26_calculate_supernova_feedback.c`: registration and parameter reading, the fixed-efficiency reheating, renormalisation and ejection with its floor, edge cases (no stars, zero `Vvir`, little cold gas), and for `FIREmodeOn 1` the scaling's guards, floor and slopes, the loading cap and `MassLoading`, the energy-driven ejection, the boundary skip and the redshift-table refusal, plus the parameter's refusals and the skipped FFB row.
- `_tests/test_integration_sage26_calculate_supernova_feedback.py`: runs the star-formation chain at `FIREmodeOn 0` with default, zero and very high efficiencies and checks that `SupernovaOutflowRate` is finite, non-negative, tracks `StarFormationRate` and rises with the efficiencies, with no memory leak.
