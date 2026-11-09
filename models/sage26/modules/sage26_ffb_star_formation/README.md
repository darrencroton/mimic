# `sage26_ffb_star_formation`

Forms the stars of a feedback-free-burst (FFB) galaxy, `FFBRegime` 1 as set by `sage26_determine_ffb_regime`, in one by-galaxy transaction per substep that replaces the ordinary disk chain: star formation from the whole cold gas at `EnhancedSFEfficiency` per disk dynamical time, the history sample and star-formation commit, FIRE supernova feedback against the gas left, and the metal yield, with no disk instability. It implements SAGE26's `starformation_ffb` (feedback-free starbursts, Li et al. 2024) and writes nothing with `EnhancedStarFormationOn 0` or on any other galaxy.

## Physics

1. `reff = StarFormingDiskFactor * DiskScaleRadius` and `tdyn = reff / Vvir` from the galaxy's own `Vvir`; `tdyn` is 0 unless `reff` and `Vvir` are both positive.
2. `H2gas` is set to 0 and `H1gas` is the H I block of `shared/sage_hi_physics.h`.
3. `stars = EnhancedSFEfficiency * ColdGas / tdyn * dt`, capped at `ColdGas`; a NaN or infinite input, a zero `tdyn`, or a NaN, infinite or negative result gives 0.
4. The disk history is sampled in the substep's bin (`shared/sage_sfr_history.h`), then the stars are committed (`update_from_star_formation`, `shared/sage_sf_feedback_physics.h`, with the `SFprescription 1` hydrogen caps) and added to the `StarFormationRate` diagnostic.
5. The FFB supernova budget (`shared/sage_ffb_feedback_physics.h`) applies the FIRE scaling from the galaxy's own `Vvir` and stored snapshot's redshift, the capped loading (stored in `MassLoading`), the renormalisation against the cold gas left after the commit, and the energy-driven ejection (Muratov et al. 2015; Hirschmann et al. 2016).
6. The reheated mass, clamped to the remaining cold gas, and the ejection are committed (`update_from_feedback`) to the FoF central's reservoir selected by the central's `Regime`, at the cold gas's metallicity from before the star-formation commit.
7. The yield is applied from the renormalised stars (`shared/sage_metal_yield_physics.h`).

As in SAGE26, the committed stars and history use the stars formed while the feedback and yield may use fewer; an invalid FIRE scaling (non-positive `Vvir` or negative redshift) leaves `MassLoading` as it was with no reheating or ejection; and only this disk call skips the instability, so merger events are unchanged.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: a substep phase, once, immediately before `sage26_calculate_star_formation`
- Receives one galaxy at a time and uses the FoF central for the feedback and yield
- On an FFB galaxy `sage26_calculate_star_formation`, `sage26_calculate_supernova_feedback`, `sage26_apply_star_formation_supernova`, `sage26_disk_instability` and `sage26_apply_metal_enrichment` each write nothing, so its `UnstableDiskGasFraction` stays 0 and the by-galaxy quasar and burst entries do not act on it; merger consumers are not bypassed
- In the core's first-snapshot boundary state (stored `SnapNum` -1, `dT` 0) the module writes nothing; any other invalid interval, or a stored `SnapNum` outside the run's redshift table, is an error. With no FoF central galaxy it writes `H2gas`, `H1gas` and the history sample and commits nothing

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. Exactly one entry, in a substep phase, as `process_by_galaxy`: an entry in `pre_timestep` or `post_timestep`, a second entry or another mode is refused.
2. Only with `EnhancedStarFormationOn 1`: the entry must sit immediately before `sage26_calculate_star_formation` (`process_by_galaxy`, same substep phase), and `sage26_determine_ffb_regime` must be configured in `pre_timestep` as `process_full_halo`, without which no galaxy is ever FFB.

In turn, with `EnhancedStarFormationOn 1`, `sage26_determine_ffb_regime`'s `init()` refuses a run without this module.

## Properties

- Reads: `dT`, `SnapNum`, `FFBRegime`, the galaxy's own `Vvir`, `DiskScaleRadius`, `ColdGas`, `MetalsColdGas`, the FoF central's `Mvir` (for the yield) and `Regime`; `HaloNr` and `Type` in log messages only
- Writes (FFB galaxies only): `H2gas`, `H1gas`, `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, the disk history bins (`SfrDiskBin<k>`, `SfrDiskColdGasBin<k>`, `SfrDiskColdGasMetalsBin<k>`), `StarFormationRate`, `SupernovaOutflowRate`, `MassLoading` (valid FIRE scaling only), and the FoF central's selected `HotGas`/`MetalsHotGas` or `CGMgas`/`MetalsCGMgas`, `EjectedGas` and `MetalsEjectedGas`

## Parameters

- `EnhancedStarFormationOn`: required, 0 (nothing written) or 1 (FFB star formation, the run files' value); any other value is refused, naming it.
- `EnhancedSFEfficiency`: the FFB star-formation efficiency per dynamical time; required, finite and within [0, 1]; the run files hold 0.2.
- `FIREmodeOn`: required; at `EnhancedStarFormationOn 1` only 1 is accepted, since the fixed-efficiency FFB feedback is not supported; 0 is refused at initialisation.
- Shared keys, each with its usual validation: `StarFormingDiskFactor`, `SFprescription`, `RecycleFraction`, `CGMrecipeOn`, `FeedbackReheatingEpsilon`, `FeedbackEjectionEfficiency`, `Yield`, `FracZleaveDisk`.

## Notes

`sage26_ffb_star_formation.h` declares `sage26_ffb_star_formation_set_test_observer()`, through which a test can read each transaction's intermediate values; it never changes the result and a run never installs one.

## Tests

- `_tests/test_unit_sage26_ffb_star_formation.c`: each case compares the galaxy and its central with SAGE26's transaction written out in the test, covering the stars and yield used, the pre-commit metallicity, the invalid FIRE scaling, the H I block, the guards, satellites routed by the central's regime, no instability, the no-op states, the redshift-table refusal, every parameter refusal and misplacement, and the rest of the chain writing nothing on an FFB galaxy.
- `_tests/test_integration_sage26_ffb_star_formation.py`: runs the module in Mimic and checks the no-op at `EnhancedStarFormationOn 0`, the written domain at 1, and that `init()` refusals and the cross-module guards reach the run's exit status.
- The shared helpers are tested by `shared/_tests/test_unit_sage_ffb_shared_helpers.c`.
