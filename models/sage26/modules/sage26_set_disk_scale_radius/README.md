# `sage26_set_disk_scale_radius`

Computes disk scale radii from halo spin and virial properties for the FOF Type 0 central, then synchronises its bulge radii.

## SAGE26 Source

A port of SAGE26's `get_disk_radius` (`src/model_misc.c:203-228`) and its float store into `DiskScaleRadius` (`src/core_build_model.c:297`): the Mo, Mao & White (1998) radius `(lambda / 1.414) * Rvir` with the Bullock spin parameter `lambda = |J| / (1.414 * Vvir * Rvir)`. As in SAGE26, the spin-component products and their sum are float arithmetic (the halo spin is float) widened to double only for `sqrt`; the truncated literal `1.414` stands for `sqrt(2)` twice; the guard is `Vvir > 0 && Rvir > 0`, with the double fallback `0.1 * Rvir`. `Vvir` and `Rvir` are read as the floats SAGE26 holds.

Immediately after the store, SAGE26's `get_bulge_radius` on the same central (`src/core_build_model.c:298`, `src/model_misc.c:239-377`, `shared/sage_bulge_size_physics.h`): with `BulgeSizeOn 0` it zeroes the three bulge radii; with `BulgeSizeOn 3` it recombines them from the two bulge channels, its instability fallback reading the new disk radius. Satellites are not resynchronised here, and there is no end-of-snapshot radius pass (SAGE26 has none).

## Processing Contract

- Supported mode: `process_full_halo`
- Expected phase: `pre_timestep`
- Receives the full FoF workspace
- Updates only Type 0 galaxies; Type 1 satellites keep the radius inherited from their last central phase

## Properties

- Reads: `Type`, `Spin`, `Vvir`, `Rvir`; with `BulgeSizeOn 3` also `MergerBulgeMass`, `InstabilityBulgeMass`, `MergerBulgeRadius`, `InstabilityBulgeRadius` and, for the instability fallback, the `DiskScaleRadius` it has just written
- Writes: `DiskScaleRadius`, `BulgeRadius`, `MergerBulgeRadius`, `InstabilityBulgeRadius`

## Parameters

- `BulgeSizeOn` (required, with no default, 0 or 3): SAGE26's bulge-size mode. 0, SAGE26's R0, computes no sizes (`R/src/model_misc.c:242-247`, `:395`); 3, the run files' value, is SAGE26's Tonini et al. (2016) merger and instability bulges (`:317-424`), SAGE26's default (`src/core_read_parameter_file.c:107`; `input/millennium.par` does not set it), so configurations R1–R6 run with it. Modes 1 and 2 are not ported: any value other than 0 or 3, or a missing key, fails initialisation with an error naming the parameter (`LOAD_PARAM_INT`, then `mimic_sage_validate_bulge_size_on` in `shared/sage_bulge_size_physics.h`). Mode 3 also reads the core's `Hubble_h`.

## Ordering

**Advisory (no `init()` enforcement):**

1. Run in `pre_timestep`, before the substep baryonic physics phases — star formation and disk instability both read `DiskScaleRadius` during the substep. Satellite radii are not updated here; they retain the value from their last central phase (SAGE parity: SAGE26 recomputes the radius only for the FoF central, `src/core_build_model.c:284-320`).
2. The shipped run files place it, with `sage26_initialise_merger_clock`, before `sage26_reionization` and `sage26_prepare_infall_budget`, SAGE26's source order (radius and merger clock during progenitor joining, then the infall budget).

`init()` refuses only a missing or unsupported `BulgeSizeOn` (Parameters); it makes no placement or partner check. It does not refuse an entry in a substep phase or `post_timestep`, or a repeated entry, and no other module's `init()` requires it. `BulgeSizeOn` 0 is not a no-op: each Type 0 row still gets its disk radius and its three bulge radii are zeroed.

## Tests

- `_tests/test_unit_sage26_set_disk_scale_radius.c`: behavioural tests at `BulgeSizeOn` 0 on hand-built rows: the Type 0 disk radius for normal, high-, low- and zero-spin halos and the `0.1 * Rvir` fallback for a zero `Vvir` or `Rvir`, each against SAGE26's expression written in the test (zero spin: exactly 0); tiny positive `Vvir` and `Rvir` taking the formula, not the fallback; Type 1 and Type 2 rows, and a row without a galaxy, left unchanged; a mixed FoF; initialisation through the module system; no memory leak.
- `_tests/test_integration_sage26_set_disk_scale_radius.py` runs the module alone in `pre_timestep` with `BulgeSizeOn` 0 and checks the output: `DiskScaleRadius` present, finite and non-negative; on rows where it and `Rvir` are both positive, at most `Rvir`, correlated with it (coefficient above 0.2, when there are more than 10 such rows) and with a median between 1e-4 and 10; more than half the Type 0 rows non-zero; satellite and orphan rows finite; no memory leak and a run under 30 s.
- `_tests/test_unit_sage26_set_disk_scale_radius_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`): `get_disk_radius` and the central's synchronisation (`tr_sync_*`, the oracle's transcription of `src/core_build_model.c:297-298`) through `process()`, and `get_bulge_radius` (`gbr_*`) through the shared helper directly, and asserts bit-for-bit equality with the SAGE26 function-level oracle (`../_tests/oracle/README.md`); outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP.
- The `BulgeSizeOn` refusals are tested at package level by `../_tests/test_unit_sage26_bulge_size_parameter.c`: a missing key and the values 1, 2, 4 and -1 fail this module's `init()`, 0 and 3 are accepted, and `BulgeSizeOn` 3 without the run configuration (`Hubble_h`) fails `process()`.
