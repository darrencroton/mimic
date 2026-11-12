# `sage26_starburst_feedback`

Implements SAGE26's collisional starburst, `collisional_starburst_recipe`, with its supernova feedback and metal yield, for two triggers: disk instabilities and galaxy mergers. One shared function, `mimic_apply_collisional_starburst` (`shared/sage_starburst_physics.h`), runs the whole sequence for both channels; the star-formation and feedback commits are the package's one implementation in `shared/sage_sf_feedback_physics.h`.

## Physics

1. **Bursting fraction.** The trigger itself (`UnstableDiskGasFraction`) for a disk instability; `0.56 * ratio^0.7` for a merger (Somerville et al. 2001). The burst gas is the host's total `ColdGas` times this fraction; the new stars are that gas, floored at zero.
2. **Reheated mass.** With `FIREmodeOn 1`, `FeedbackReheatingEpsilon` times the Muratov et al. (2015) mass-loading scaling, evaluated from the burst host's own `Vvir` and the redshift of its stored snapshot, capped by the supernova energy available; unlike the disk path, the burst writes no `MassLoading`. With `FIREmodeOn 0`, `FeedbackReheatingEpsilon * stars`. Stars and reheated mass are scaled down together when they exceed the burst gas.
3. **Ejected mass.** From the host's `Vvir`: with `FIREmodeOn 1` the Hirschmann et al. (2016) energy-based ejection (the supernova energy left after lifting the reheated gas, over `0.5 * Vvir^2`); with `FIREmodeOn 0` the fixed-efficiency formula with `FeedbackEjectionEfficiency`. Zero for a non-positive `Vvir`, floored at zero.
4. **Commits.** The bulge SFR history of the substep's bin is sampled (every burst, zero stars included); star formation is committed, with `(1 - RecycleFraction) * stars` added to `BulgeMass` and the selected bulge channel; then the feedback moves reheated and ejected gas to the FoF central, into its CGM when `CGMrecipeOn 1` and the central's `Regime` is 0, otherwise its hot gas. The instability channel updates the instability-bulge radius (Tonini et al. 2016) from the saved disk radius.
5. **Hydrogen refresh** (`SFprescription 1`). The commits cap the hydrogen; then `H2gas` is capped at `0.74 * ColdGas` and `H1gas` set to the atomic remainder `0.74 * ColdGas - H2gas`, floored at zero, with no ionisation correction.
6. **Post-minor-merger recheck.** For a merger with ratio strictly below `ThresholdMajorMerger`, the host's disk instability is rechecked: its gas branch grows the black hole (with its quasar wind, as in `sage26_quasar_mode`) and bursts again into the instability channel.
7. **Metal yield.** Retained in the host's cold gas (less `FracZleaveDisk`) when the host keeps more than 1e-8 of cold gas and the ratio is strictly below the threshold; otherwise all of it goes to the FoF central's hot or (as for the feedback) CGM metals. A ratio equal to the threshold gets no recheck and loses its whole yield.

There is no early return on a zero trigger, cold gas or stellar mass. The package's `StarFormationRate` diagnostic adds `stars / dT`; `sage26_finalize_sfr_histories` averages the bulge history after the last substep. A stored `SnapNum` outside the run's redshift table is refused with `FIREmodeOn 1`: the burst writes nothing and `process()` fails.

## Processing Contract

- Supported modes: `process_by_galaxy`, `process_per_event`
- `process_by_galaxy`: the disk instability in `galaxy_physics` (`UnstableDiskGasFraction > 0`, the instability channel), after `sage26_disk_instability` and `sage26_quasar_mode`, with `dt` the galaxy's own substep interval. It never acts on a feedback-free-burst galaxy (`FFBRegime` 1), whose `UnstableDiskGasFraction` stays 0.
- `process_per_event`: `merger` events from `sage26_resolve_mergers_and_disruption` in `satellite_mergers`, after `sage26_quasar_mode`'s black-hole growth, with `dt` the satellite's substep interval (`value1`). Every merger bursts, a ratio of 0 and an FFB target included. The channel is `MergerBulgeMass` for a major merger, and for a minor one `InstabilityBulgeMass` onto a target that was disk-dominated before the addition (`MergerDiskDominated`), `MergerBulgeMass` otherwise.

## Ordering

- `process_per_event` requires `sage26_resolve_mergers_and_disruption` as `process_full_halo` in the same substep phase; `init()` refuses a run without it. When this module and `sage26_quasar_mode` are both `process_per_event` in one phase, the resolver's `init()` refuses any order but the quasar consumer first.
- Warned, not refused: `process_by_galaxy` with no preceding `sage26_disk_instability` (the instability channel then fires on no galaxy); and `sage26_disk_instability` configured without `sage26_quasar_mode` as `process_per_event` (the recheck then skips its black-hole growth).
- Not checked: the `process_by_galaxy` entry's position against `sage26_quasar_mode`'s, and either mode alone. Without `sage26_disk_instability` as `process_by_galaxy`, no merger burst runs the recheck.

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Mvir`, `Vvir`, `Vmax`, `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, `BulgeMass`, `MetalsBulgeMass`, `MergerBulgeMass`, `InstabilityBulgeMass`, the three bulge radii, `HotGas`, `MetalsHotGas`, `CGMgas`, `MetalsCGMgas`, `Regime` (the FoF central's for feedback and yield, the host's for the recheck's wind), `EjectedGas`, `MetalsEjectedGas`, `BlackHoleMass`, `QuasarModeBHaccretionMass`, `DiskScaleRadius`, `UnstableDiskGasFraction`, `MergerDiskDominated`, `H2gas`, `H1gas`
- Writes: `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, `BulgeMass`, `MetalsBulgeMass`, `MergerBulgeMass`, `InstabilityBulgeMass`, `HotGas`, `MetalsHotGas`, `CGMgas`, `MetalsCGMgas`, `EjectedGas`, `MetalsEjectedGas`, `StarFormationRate`, `SupernovaOutflowRate`, the bulge history elements of the substep's bin, `BlackHoleMass` and `QuasarModeBHaccretionMass` (the recheck), the bulge radii (`BulgeSizeOn 3`), and `H2gas` and `H1gas` (`SFprescription 1`)

## Parameters

All are required with no default; a missing key or a value outside the accepted range fails initialisation naming the parameter. The four marked conditional are read only when the post-minor-merger recheck needs them.

- `FeedbackReheatingEpsilon` (finite, >= 0): the reheating efficiency.
- `FeedbackEjectionEfficiency` (in [0, 10]), `RecycleFraction` (in [0, 1]), `Yield` (in [0, 1]), `FracZleaveDisk` (in [0, 1]), `ThresholdMajorMerger` (in [0, 1]): the ejection efficiency, recycled fraction, metal yield, fraction of the yield leaving the disk, and major-merger threshold.
- `StarFormingDiskFactor` (in [0, 10]; conditional on `sage26_disk_instability` as `process_by_galaxy`): the recheck's critical-mass factor.
- `BlackHoleGrowthRate` (in (0, 1]), `QuasarModeEfficiency` (in [0, 1]), `AGNrecipe` (0 or 2; conditional on `sage26_disk_instability` as `process_by_galaxy` and `sage26_quasar_mode` as `process_per_event`): the recheck's black-hole growth and wind. With `AGNrecipe 0` the recheck grows no black hole; SAGE26's 1 and 3 are not supported, refused at initialisation (`shared/sage_agn_recipe.h`).
- `BulgeSizeOn` (0 or 3): 3 (the shipped value) updates the Tonini et al. (2016) bulge sizes; 0 computes none. Modes 1 and 2 are not supported.
- `SFprescription` (0 or 1): 1 (the shipped value) applies the hydrogen caps and refresh; 0 does not.
- `StarburstColdGasOn` (1 only): bursts consume cold gas; SAGE26's H2-fuel option 0 is not supported.
- `FIREmodeOn` (0 or 1): 1 (the shipped value) selects the FIRE feedback budget for every burst, including the recheck's; 0 the fixed-efficiency budget.
- `CGMrecipeOn` (0 or 1): 1 (the shipped value) routes feedback and escaping yield by the FoF central's `Regime`; 0 uses the hot gas throughout.

## Tests

- `_tests/test_unit_sage26_starburst_feedback.c`: the hydrogen refresh, the FIRE budget and its wiring on every burst path, the CGM routing of feedback, yield and recheck wind, and `AGNrecipe 0` in the recheck.
- `_tests/test_integration_sage26_starburst_feedback.py`: the module as `process_by_galaxy` in a pipeline that supplies cold gas, checking that galaxies burst, that bursting rows carry stars, bulge, instability-channel bulge and outflow, that the bulge stays within rounding of the stellar mass, and that every reservoir is finite and non-negative.
- The `AGNrecipe`, `FIREmodeOn` and `CGMrecipeOn` refusals are covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c`.
