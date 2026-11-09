# `sage26_calculate_star_formation`

Calculates the stellar mass formed in the disk this substep and stores it in the `NewStellarMass` transport field for the feedback and apply steps. `SFprescription` selects the law: Blitz & Rosolowsky (2006) molecular gas from a radial integration of the disk (1, the run files' value) or the Croton et al. (2006) critical-mass law (0). It also writes the H I diagnostic `H1gas` and, with `SFprescription 1`, `H2gas`.

## Physics

Both laws use the star-forming radius `reff = StarFormingDiskFactor * DiskScaleRadius` and the dynamical time `tdyn = reff / Vvir` of the galaxy's own `Vvir`; a galaxy with a non-positive `Vvir` forms no stars.

- `SFprescription 0`: `strdot = SfrEfficiency * (ColdGas - cold_crit) / tdyn` above the critical mass `cold_crit = 0.19 * Vvir * reff`, and zero below it.
- `SFprescription 1`: the molecular gas `H2gas` of an exponential disk integrated over radius (`shared/sage_h2_physics.h`), capped at `0.74 * ColdGas`, the hydrogen mass; `strdot = SfrEfficiency * H2gas / tdyn` with no critical-mass threshold. A non-positive `Vvir` or disk radius writes `H2gas` 0.
- H I: `H1gas = max(0.74 * ColdGas - H2gas, 0) * (1 - f_ion)`, where `f_ion` is the ionised fraction of an exponential gas disk beyond the radius at which its surface density falls to 0.5 Msun/pc^2 (`shared/sage_hi_physics.h`).
- `NewStellarMass = strdot * dt`, floored at zero.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, before `sage26_calculate_supernova_feedback` and `sage26_apply_star_formation_supernova` (and, in the run files, immediately after `sage26_ffb_star_formation`)
- Receives one galaxy at a time
- A feedback-free-burst row (`FFBRegime` 1) is skipped and nothing on it is written: `sage26_ffb_star_formation` runs that row's transaction
- In the core's first-snapshot boundary state (stored `SnapNum` negative, `dT` not positive) the module writes nothing; any other non-positive interval is an error

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_apply_star_formation_supernova` must be configured somewhere in the pipeline — without it `NewStellarMass` is never committed to the reservoirs (silent output loss).

**Not refused:** the apply step's position (its own `init()` requires this module before it in the same substep phase); `sage26_calculate_supernova_feedback` absent or anywhere (its own `init()` checks the order); `sage26_ffb_star_formation` absent (with `EnhancedStarFormationOn 1`, `sage26_determine_ffb_regime` and the FFB module refuse that).

## Properties

- Reads: `FFBRegime`, `Type`, `HaloNr`, `SnapNum`, `dT`, `Vvir`, `ColdGas`, `DiskScaleRadius`, `H2gas`, and with `SFprescription 1` `StellarMass` and `BulgeMass` (the disk stars of the radial integration)
- Writes: `NewStellarMass`, `H1gas`, and with `SFprescription 1` `H2gas`

## Parameters

- `SfrEfficiency`: the star-formation efficiency per dynamical time; required, within [0, 1]; the run files hold 0.05.
- `StarFormingDiskFactor`: the number of disk scale radii over which stars form; required, within [0, 10]; the run files hold 3.0.
- `SFprescription`: required, 0 (Croton et al. 2006) or 1 (Blitz & Rosolowsky 2006, the run files' value).
- `H2RadialIntegrationOn` (1), `H2RadialNBins` (25), `H2RadialRMaxFactor` (5.0): the radial integration, required and accepted only at these values (the single-slab branch is not supported); the integration runs over 25 bins out to 5 disk scale radii.

A missing key, or a value outside the accepted ones (also when `SFprescription` is 0), fails initialisation naming the parameter.

## Notes

`NewStellarMass` is a transport field; `sage26_apply_star_formation_supernova` updates the persistent reservoirs. Under `SFprescription 0` the module never writes `H2gas` and the hydrogen caps of the commits do not run, so `H1gas` can exceed the hydrogen left in `ColdGas` after star formation, as in SAGE26. `H2DepletionTime_Gyr` is not written.

## Tests

- `_tests/test_unit_sage26_calculate_star_formation.c`: the Croton law above, at and below the critical mass, zero disk radius and `Vvir`, the efficiency's effect, the BR06 branch (guards, stale `H2gas` cleared, `H2gas` within the hydrogen budget, no threshold), lifecycle and memory safety, and the skipped FFB row.
- `_tests/test_integration_sage26_calculate_star_formation.py`: runs the module with the apply step at `SFprescription 0` and checks a finite, non-negative and plausible `StarFormationRate`, no star formation without cold gas, the response to `SfrEfficiency`, no memory leak and the run time.
