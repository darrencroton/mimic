# `sage26_determine_ffb_regime`

Classifies every unmerged galaxy of the FoF group, once per snapshot before the substeps, as a feedback-free-burst (FFB) galaxy (`FFBRegime` 1) or a normal one (`FFBRegime` 0), from the Li et al. (2024) FFB fraction and a deterministic hash draw. An FFB galaxy then forms its stars through `sage26_ffb_star_formation` instead of the ordinary disk chain. The module classifies only with `EnhancedStarFormationOn 1`.

## Physics

The FFB threshold mass at the current redshift `z` is `Mvir_ffb = 10^(0.8 + log10(h) - 6.2 log10((1 + z) / 10))` in 1e10 Msun/h (Li et al. 2024, Eq. 2), and the FFB fraction is the sigmoid `f_ffb = 1 / (1 + exp(-x))` with `x = log10(Mvir / Mvir_ffb) / 0.15` (Eq. 3), zero for a non-positive `Mvir`. A galaxy is FFB when `draw < f_ffb`, a strict comparison. The draw is SAGE26's SplitMix64 hash (Steele, Lea & Flood 2014) of the galaxy's `MostBoundID` and the current snapshot, mapped to a uniform deviate in (0, 1], so the same galaxy at the same snapshot always draws the same value. The arithmetic lives in `shared/sage_ffb_regime.h`.

Satellites and orphans are classified like centrals, and the CGM regime does not enter; an orphan's `Mvir` is 0, so it is never FFB. A NaN `Mvir` gives FFB 0 and an infinite one FFB 1. The current redshift and snapshot come from the module context, never from a galaxy's stored snapshot.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`, once; the run files place it last, after `sage26_prepare_infall_budget`
- Receives the full FoF workspace; merged rows (`Type` 3) and rows without a galaxy are skipped

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. Exactly one entry, in `pre_timestep`, as `process_full_halo`: an entry in a substep phase or `post_timestep`, a second entry or another mode is refused.
2. Where `sage26_determine_regime` and `sage26_prepare_infall_budget` are configured in `pre_timestep`, this module must follow them.
3. Only with `EnhancedStarFormationOn 1`: `sage26_ffb_star_formation` must be configured as `process_by_galaxy` in a substep phase — an FFB galaxy skips the ordinary star-formation chain, so without the FFB transaction it would form no stars.

## Properties

- Reads: `Type`, `Mvir`, `MostBoundID`
- Writes: `FFBRegime` (only with `EnhancedStarFormationOn 1`; with 0 it keeps its creation value 0). It carries over between snapshots.

`Hubble_h` is read from the run configuration.

## Parameters

- `EnhancedStarFormationOn`: required, 0 (no classification) or 1 (the Li et al. 2024 classifier and FFB star formation, the run files' value). Value 2, the Boylan-Kolchin (2025) acceleration criterion, is not supported; it and any other value are refused at initialisation, naming the parameter.

## Notes

`sage26_determine_ffb_regime.h` declares `sage26_determine_ffb_regime_set_test_seam()`, through which a test can substitute the compared deviate (to reach the tie `draw == f_ffb`) and read back each classified row's values; a run never installs one.

## Tests

- `_tests/test_unit_sage26_determine_ffb_regime.c`: no writes at `EnhancedStarFormationOn 0`, the classification of each `Type`, independence of the CGM regime, the inputs the result depends on (narrowed `Mvir`, current redshift, draw keyed on `MostBoundID` and the current snapshot), the tie through the seam, and the misplacements.
- `_tests/test_integration_sage26_determine_ffb_regime.py`: runs the module in Mimic and checks the no-op at `EnhancedStarFormationOn 0` and the written `FFBRegime` at 1.
- The shared arithmetic is tested by `shared/_tests/test_unit_sage_ffb_regime.c`.
- The parameter refusals and the placement checks are also covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c` and `../_tests/test_unit_sage_dependency_contracts.c`.
