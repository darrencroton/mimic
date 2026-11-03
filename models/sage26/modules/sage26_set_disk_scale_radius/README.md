# `sage26_set_disk_scale_radius`

Computes disk scale radii from halo spin and virial properties for the FOF Type 0 central.

## SAGE26 Source

A port of SAGE26's `get_disk_radius` (`src/model_misc.c:203-228`) and its float store into `DiskScaleRadius` (`src/core_build_model.c:297`): the Mo, Mao & White (1998) radius `(lambda / 1.414) * Rvir` with the Bullock spin parameter `lambda = |J| / (1.414 * Vvir * Rvir)`. As in SAGE26, the spin-component products and their sum are float arithmetic (the halo spin is float) widened to double only for `sqrt`; the truncated literal `1.414` stands for `sqrt(2)` twice; the guard is `Vvir > 0 && Rvir > 0`, with the double fallback `0.1 * Rvir`. `Vvir` and `Rvir` are read as the floats SAGE26 holds.

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace
- Updates only Type 0 galaxies; Type 1 satellites keep the radius inherited from their last central phase

## Properties

- Reads: `Type`, `Spin`, `Vvir`, `Rvir`
- Writes: `DiskScaleRadius`

## Parameters

None.

## Ordering

**Advisory (no `init()` enforcement):**

1. Run in `pre_timestep`, before the substep baryonic physics phases — star formation and disk instability both read `DiskScaleRadius` during the substep. Satellite radii are not updated here; they retain the value from their last central phase (SAGE parity: SAGE26 recomputes the radius only for the FoF central, `src/core_build_model.c:284-320`).
2. The shipped run files place it, with `sage26_initialise_merger_clock`, before `sage26_reionization` and `sage26_prepare_infall_budget`, SAGE26's source order (radius and merger clock during progenitor joining, then the infall budget).

## Tests

`_tests/test_unit_sage26_set_disk_scale_radius_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`) through `process()` and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.
