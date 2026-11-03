# `sage26_reionization`

Calculates the reionization suppression factor for each galaxy from its own halo mass and the current redshift, and stores the galaxy's local baryon fraction.

## SAGE26 Source

A port of SAGE26's `do_reionization` (`src/model_infall.c:286-337`): the Gnedin (2000) filtering-mass model with the Kravtsov et al. (2004) Appendix B fitting formulas, `modifier = 1 / (1 + 0.26 (M / Mvir))^3` with `M` the larger (`dmax`) of the filtering mass and the characteristic mass of a 10^4 K halo. The reionization scale factors are `a0 = 1 / (1 + 8)` and `ar = 1 / (1 + 7)` (R0's `Reionization_z0` and `Reionization_zr`), derived at `init()` as SAGE26 derives them (`src/core_init.c:70-71`).

Every expression keeps SAGE26's shape: `pow(a/a0, -0.5)` and `/ sqrt(a/a0)` each where SAGE26 writes it, the `CUBE` grouping, `0.26 * (M / Mvir)`, the comparison-based `dmax` rather than `fmax`, and no extra `Mvir` gate (`Mvir = 0` gives a modifier of 0, as in SAGE26). `Mvir` is read as the float SAGE26 holds. The module stores `HaloBaryonFraction = modifier * GlobalBaryonFraction`, the once-rounded product SAGE26 forms as the leading term of the infall and stripping budgets (`src/model_infall.c:128-130`, `:243-244`); `sage26_prepare_infall_budget` and `sage26_satellite_stripping` finish those expressions with the halo mass.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace

## Ordering

`init()` enforces ordering: when `sage26_prepare_infall_budget` is also configured in `pre_timestep`, reionization must run before it, because reionization writes `HaloBaryonFraction` that the infall budget reads. Later-phase consumers such as `sage26_satellite_stripping` (a substep phase) are structurally guaranteed to run after `pre_timestep`, so no explicit check is needed for them.

## Properties

- Reads: `Type`, `Mvir`
- Writes: `HaloBaryonFraction`

## Parameters

- `GlobalBaryonFraction`

## Tests

`_tests/test_unit_sage26_reionization_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.
