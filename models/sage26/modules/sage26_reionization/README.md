# `sage26_reionization`

Calculates the reionization suppression factor for each galaxy from its own halo mass and the current redshift, following SAGE26's `do_reionization`, and stores the galaxy's local baryon fraction `HaloBaryonFraction`.

## Physics

The Gnedin (2000) filtering-mass model with the Kravtsov et al. (2004) Appendix B fitting formulas: `modifier = 1 / (1 + 0.26 * (M / Mvir))^3`, with `M` the larger of the filtering mass and the characteristic mass of a 10^4 K halo at the current redshift. Reionization begins at z = 8 and completes at z = 7 (SAGE26's `Reionization_z0` and `Reionization_zr`). A galaxy with `Mvir = 0` gets a modifier of 0. The module stores `HaloBaryonFraction = modifier * GlobalBaryonFraction`, which `sage26_prepare_infall_budget` and `sage26_satellite_stripping` multiply by the halo mass to form their budgets. The expressions keep SAGE26's forms (`SAGE parity:` in the source).

## Processing Contract

- Supported mode: `process_full_halo`, expected in `pre_timestep`
- Receives the full FoF workspace

## Ordering

- With `sage26_prepare_infall_budget` in `pre_timestep` as `process_full_halo`, `init()` refuses a run unless this module precedes it in `pre_timestep`, with an error naming both; `sage26_prepare_infall_budget`'s `init()` refuses the same misorder. Substep-phase consumers such as `sage26_satellite_stripping` always run after `pre_timestep`.
- Without this module, `HaloBaryonFraction` keeps its creation value -1, and `sage26_prepare_infall_budget` and `sage26_satellite_stripping` use `GlobalBaryonFraction` in its place, which is SAGE26's `ReionizationOn 0`.

## Properties

- Reads: `Type`, `Mvir`
- Writes: `HaloBaryonFraction`, on every row with a galaxy except a Type 3

## Parameters

- `GlobalBaryonFraction` (required, in (0, 1]): the cosmic baryon fraction. The shipped run files use 0.17.

The reionization redshifts are fixed at 8.0 and 7.0 and are not parameters; configuring the module is the switch that SAGE26's `ReionizationOn` provides. The module also reads the core's cosmology (`Omega`, `OmegaLambda`, `Hubble`, `G`) and the snapshot redshift.

## Tests

- `_tests/test_unit_sage26_reionization.c`: lifecycle, the accepted `GlobalBaryonFraction` values reaching `HaloBaryonFraction`, and the suppression's range and its dependence on mass and redshift on hand-built rows.
- `_tests/test_integration_sage26_reionization.py`: the module alone in a full run, with range and mass-trend checks on `HaloBaryonFraction`.
- The `GlobalBaryonFraction` refusals are covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c`, and the misorder refusal by `../_tests/test_unit_sage_dependency_contracts.c`.
