# `sage26_determine_regime`

Classifies every live galaxy of the FoF group, once per snapshot before the infall budget, as CGM regime (`Regime` 0, gas held in the `CGMgas` reservoir) or hot-halo regime (`Regime` 1, gas held in `HotGas`) from its virial mass against the Dekel & Birnboim (2006) shock mass. The module classifies only with `CGMrecipeOn 1`; the regime then selects the reservoir that infall, cooling, feedback and metal deposits use.

## Physics

The shock mass is `Mshock = MShockMsun * Hubble_h / 1e10` in 1e10 Msun/h. A galaxy with `Mvir <= 0` (an orphan) is CGM regime with no draw. Otherwise the hot-halo probability is the sigmoid `hot_fraction = 1 / (1 + exp(-x))` with `x = log10(Mvir / Mshock) / 0.1`, 0.1 dex wide about the shock mass, and the galaxy is hot-halo regime when a uniform deviate `u < hot_fraction`. There is no hysteresis, no redshift term and no `Type` restriction (centrals, satellites and orphans are all classified), and classification moves no gas. A NaN mass ratio is classified 0. References: Dekel & Birnboim (2006), MNRAS, 368, 2; Voit (2015), ApJL, 808, L30, cited with it for the two-regime CGM model.

The deviate is the package's keyed draw (`shared/sage_keyed_draw.h`), a pure function of the galaxy's `UniqueGalaxyID`, the current snapshot and a purpose salt, strictly inside (0, 1), with no seed and no global state: the vertical and horizontal drivers classify a galaxy alike, and a run is reproducible. It stands in for SAGE26's libc `rand()`, so individual classifications do not follow SAGE26's random sequence.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`, once; the run files place it after `sage26_reionization` and immediately before `sage26_prepare_infall_budget`
- Receives the full FoF workspace; merged rows (`Type` 3) and rows without a galaxy are skipped

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. Exactly one entry, in `pre_timestep`, and none in `post_timestep` or a substep phase.
2. When `sage26_prepare_infall_budget` is configured in `pre_timestep`, this module must precede it — the infall routing reads the central's regime.

**Not refused here:** the module's absence. `sage26_calculate_cooling_budget` refuses `CGMrecipeOn 1` without this module in `pre_timestep`. With `CGMrecipeOn 0` the module writes nothing and every reader of `Regime` sees its creation value -1, which takes the hot path.

## Properties

- Reads: `Type`, `Mvir`, `UniqueGalaxyID`
- Writes: `Regime` (only with `CGMrecipeOn 1`; with 0 it keeps its creation value -1)

## Parameters

- `CGMrecipeOn`: required, 0 (no classification) or 1 (the classifier, the run files' value). With 1 this module, the regime-aware routing of deposits (`shared/sage_cgm_routing.h`) and `sage26_regime_aware_cooling` together run SAGE26's CGM recipe. Any other value, or a missing key, is refused naming it.
- `MShockMsun`: the shock mass in physical Msun (no `h`); required and validated whatever `CGMrecipeOn` holds, positive and finite; the run files hold 6.0e11.

## Notes

`sage26_determine_regime.h` declares `sage26_determine_regime_set_test_seam()`, through which a test can supply each deviate and read back each classified row's values; a run never installs one.

## Tests

- `_tests/test_unit_sage26_determine_regime.c`: no writes at `CGMrecipeOn 0`, the no-draw branch for zero and negative masses and the draw for a NaN one, deviates either side of and equal to the sigmoid value, every `Type`, and the production keyed draw against reference deviates.
- `_tests/test_integration_sage26_determine_regime.py`: runs the module in Mimic and checks that it writes only `Regime` at `CGMrecipeOn 1` and that both regimes occur.
- The keyed draw is tested by `shared/_tests/test_unit_sage_keyed_draw.c`.
- The placement checks are covered at package level by `../_tests/test_unit_sage_dependency_contracts.c`, and the parameter refusals also by `../_tests/test_unit_sage26_parameter_refusals.c`.
