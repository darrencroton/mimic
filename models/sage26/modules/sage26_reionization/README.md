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

`init()` enforces ordering: when `sage26_prepare_infall_budget` is also configured in `pre_timestep`, reionization must run before it, because reionization writes `HaloBaryonFraction` that the infall budget reads. Precisely: with `sage26_prepare_infall_budget` in `pre_timestep` as `process_full_halo`, `init()` refuses unless this module precedes it in `pre_timestep` (so a run where this module has no `pre_timestep` entry is refused too), with an error naming both. `sage26_prepare_infall_budget`'s own `init()` refuses the same misorder when both are in `pre_timestep`. Later-phase consumers such as `sage26_satellite_stripping` (a substep phase) run after `pre_timestep`, so a `pre_timestep` placement precedes them without a check.

`init()` does not refuse: any placement of this module (a substep phase, `post_timestep`, a repeated entry) when `sage26_prepare_infall_budget` is not in `pre_timestep`; or a run without this module. Without it `HaloBaryonFraction` keeps its creation value -1 until a consumer replaces it: `sage26_prepare_infall_budget` writes `GlobalBaryonFraction` over a central's -1, and `sage26_satellite_stripping` uses `GlobalBaryonFraction` for a -1. That stands for SAGE26's `ReionizationOn 0` (modifier 1.0, `src/model_infall.c:122-126`, `:230-234`).

## Properties

- Reads: `Type`, `Mvir`
- Writes: `HaloBaryonFraction`, on every row with a galaxy except a Type 3

## Parameters

- `GlobalBaryonFraction`: required, with no default; a missing key fails `init()`. Accepted values (0, 1] (`LOAD_AND_VALIDATE_RANGE_EXCLUSIVE`, which rejects `value <= 0` and `value > 1`; a NaN fails neither comparison and is accepted). It is SAGE26's `BaryonFrac` (0.17, `input/millennium.par:39`); the run files set 0.17.
- `Reionization_z0` and `Reionization_zr` are not parameters: the module fixes them at 8.0 and 7.0, SAGE26's values (`input/millennium.par:85-86`, also its defaults, `src/core_read_parameter_file.c:125-126`), which no configuration R0–R6 changes. SAGE26's `ReionizationOn` (1, `input/millennium.par:51`) has no parameter: configuring the module is the switch.
- The module also reads the core's `Omega`, `OmegaLambda`, `Hubble` and `G`, and the snapshot's redshift.

## Tests

- `_tests/test_unit_sage26_reionization.c`: registration, initialisation and cleanup through the module system; `GlobalBaryonFraction` read (0.20) and refused at 0, -0.1 and 1.5; no memory leak; on hand-built rows, `HaloBaryonFraction` of a Type 0 and a Type 1 row within (0, `GlobalBaryonFraction`], 0 for `Mvir` 0, below half of `GlobalBaryonFraction` for `Mvir` 0.1 (1e9 Msun/h) at z = 2, above 0.16 (of 0.17) for `Mvir` 1e5 (1e15 Msun/h) at z = 0, increasing with mass, varying with redshift, and a Type 3 row left unchanged.
- `_tests/test_integration_sage26_reionization.py` runs the module alone in `pre_timestep` and checks the run: its initialisation and cleanup messages; `HaloBaryonFraction` in the output, non-negative, at most `GlobalBaryonFraction` (0.17, or 0.20 where the run file sets it) and positive where `Mvir` is positive; a lower mean over the lowest third of Type 0 masses than over the highest third, by more than 5%; no memory leak.
- `_tests/test_unit_sage26_reionization_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.
- The misorder refusal is tested at package level by `../_tests/test_unit_sage_dependency_contracts.c` (`test_dep_prepare_infall_reionization_wrong_order_error`): `sage26_prepare_infall_budget` before this module in `pre_timestep` fails initialisation with an error naming this module and `HaloBaryonFraction`.
