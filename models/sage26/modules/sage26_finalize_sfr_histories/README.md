# `sage26_finalize_sfr_histories`

Turns the per-substep star-formation histories of a snapshot into SAGE26's four star-formation outputs, once per FoF group after the last substep: the mean disk and bulge star-formation rates `SfrDisk` and `SfrBulge` (Msun/yr) and the mean metallicities of the gas forming them, `SfrDiskZ` and `SfrBulgeZ` (dimensionless).

## SAGE26 Source

SAGE26 keeps six float arrays of `STEPS = 10` bins per galaxy (`src/core_allvars.h:158-163` in the staged SAGE26 source): `SfrDisk` and `SfrBulge` in 1e10 Msun/h per code time, and the cold gas and its metals sampled with each, `SfrDiskColdGas`, `SfrDiskColdGasMetals`, `SfrBulgeColdGas` and `SfrBulgeColdGasMetals`. Substep `step` of `effective_steps` writes bin `(step * STEPS) / effective_steps`, capped at `STEPS - 1` (`src/core_build_model.c:514-515`): disk star formation adds `stars / dt` to `SfrDisk` and assigns the cold gas and metals on every call (`src/model_starformation_and_feedback.c:1058-1060`), and every burst adds `stars / dt`, the cold gas and the metals of the bursting galaxy (`src/model_mergers.c:755-757`). Before the substeps SAGE26 records `SubstepsUsed = effective_steps` on every galaxy of the halo, merged galaxies included (`src/core_build_model.c:461-468`). Its HDF5 writer then averages (`src/io/save_gals_hdf5.c:934-959`):

```c
float tmp_SfrDisk = 0.0;  /* ... and tmp_SfrBulge, tmp_SfrDiskZ, tmp_SfrBulgeZ */
const int sfr_norm = sfr_rate_divisor(g->SubstepsUsed);         /* > 0 ? n : STEPS */
const int met_norm = sfr_metallicity_divisor(g->SubstepsUsed);  /* min(that, STEPS) */
for(int step = 0; step < STEPS; step++) {
    tmp_SfrDisk += g->SfrDisk[step] * UnitMass_in_g / UnitTime_in_s * SEC_PER_YEAR / SOLAR_MASS / sfr_norm;
    tmp_SfrBulge += /* the same for SfrBulge */;
    if(g->SfrDiskColdGas[step] > 0.0)
        tmp_SfrDiskZ += g->SfrDiskColdGasMetals[step] / g->SfrDiskColdGas[step] / met_norm;
    if(g->SfrBulgeColdGas[step] > 0.0)
        tmp_SfrBulgeZ += /* the same for the bulge */;
}
```

The divisors are `src/model_misc.h:49-58`; `SEC_PER_YEAR` is `3.155e7` and `SOLAR_MASS` is `1.989e33` (`src/macros.h:38`, `:24`).

## What This Module Does

In `post_timestep`, for every galaxy row of the FoF (rows without a galaxy are skipped; rows merged or disrupted this snapshot are included, as SAGE26 includes them):

1. `SubstepsUsed` is set to the package's substep count for the halo (`num_substeps`), the count the substep phases ran and binned with.
2. `mimic_sage_average_sfr_histories` (`shared/sage_sfr_history.h`) reproduces the writer's averaging exactly: float accumulators, the ten bins in increasing order, each rate term left to right in double from the float element with the run's `UnitMass_in_g` and `UnitTime_in_s` (which the core forms as SAGE26 does) and SAGE26's constant values, the integer divisors, and float metallicity terms only for bins whose sampled gas is positive. No term contains a multiply-add, so floating-point contraction cannot change it.
3. The results are stored in the output scalars `SfrDisk`, `SfrBulge`, `SfrDiskZ` and `SfrBulgeZ`, which are written as they are (no `output_convert`).

The histories themselves are package-private (`output: false`): sixty float properties `<history>Bin<k>`, inherited from snapshot to snapshot (no `init_repeat`). `sage26_initialise_merger_clock` zeroes them at joining for a galaxy that stays a Type 0 or 1, SAGE26's reset at the first-occupied update (`src/core_build_model.c:278-282`); a galaxy that becomes or stays an orphan keeps them, as in SAGE26. This module reads them and does not change them.

## Processing Contract

- Supported mode: `process_full_halo`
- Required phase: `post_timestep` only, after every substep and its merger phase. In a substep phase it would average histories still being sampled, so `init()` enforces the placement: exactly one `post_timestep` entry as `process_full_halo`, and none in `pre_timestep` or any substep phase
- Requires `sage26_initialise_merger_clock` in `pre_timestep` as `process_full_halo`: the histories are inherited (no `init_repeat`), and that module's reset at joining (`src/core_build_model.c:278-282`) is their only reset. Without it they would accumulate across snapshots and every average written here would be wrong, so `init()` fails with an error naming the history reset
- Order beside `sage26_finalize_cooling_heating`: independent (neither reads what the other writes); the run files place this module after it
- Receives the full FoF workspace
- Not refused by `init()`: its order among the other `post_timestep` modules (`sage26_record_transition_state`'s own `init()` requires that module to be the last entry); the position of `sage26_initialise_merger_clock` within `pre_timestep`; and a run in which no module samples the histories (none of `sage26_apply_star_formation_supernova`, `sage26_ffb_star_formation` and `sage26_starburst_feedback`), where the four averages are formed from whatever the histories hold

## Properties

- Reads: `SfrDiskBin<k>`, `SfrBulgeBin<k>`, `SfrDiskColdGasBin<k>`, `SfrDiskColdGasMetalsBin<k>`, `SfrBulgeColdGasBin<k>`, `SfrBulgeColdGasMetalsBin<k>` (`k = 0..9`)
- Writes: `SubstepsUsed`, `SfrDisk`, `SfrBulge`, `SfrDiskZ`, `SfrBulgeZ`

## Parameters

None. The unit factors are the run's (`UnitMass_in_g`, `UnitTime_in_s`).

## Differences From SAGE26

- `num_substeps` stands for SAGE26's `effective_steps`. SAGE26's R0 chooses `effective_steps` per halo between a floor of `STEPS` and a cap (`src/core_build_model.c:420-459`), while the run files use the core's fixed 10 substeps, so a halo SAGE26 integrates in more substeps is binned and divided differently here. The core's substep count decides the bins and the divisors, as it decides the substep `dt`; matching it to SAGE26's is a core-alignment item, not package work.
- Not a difference: SAGE26 keeps the histories of an orphan (Type 2) galaxy across snapshots and moves them in mergers (`src/model_mergers.c:529-532`, `:562-568`), and so does the package (the reset at joining in `sage26_initialise_merger_clock`, the transfers in `sage26_resolve_mergers_and_disruption`).
- SAGE26 writes a row per galaxy in its output list; which rows Mimic writes (orphans included) is the core's. This module averages every galaxy row of the FoF, so each written row carries its averages.

## Tests

`_tests/test_unit_sage26_finalize_sfr_histories_oracle.c` runs every fixture under `_tests/oracle/` (listed in `../_tests/oracle/fixtures.manifest`): on the `step_bin_map` fixtures it asserts the substep interval and every substep's bin, and on the `sfr_history_average` fixtures the writer's two divisors, its skipped-bin counts and the four averages, bit for bit, through the shared helper and, where `SubstepsUsed` is positive, through `process()`. Outside the oracle environment (macOS arm64, Apple clang 21) it reports SKIP. The chained oracle test of `sage26_calculate_star_formation` runs this module after its multi-substep fixtures. `_tests/test_unit_sage26_finalize_sfr_histories.c` covers the placement check, the refusal without `sage26_initialise_merger_clock`, `SubstepsUsed` on every row, the averaging on hand-checkable values and the divisors, and `_tests/test_integration_sage26_finalize_sfr_histories.py` checks on a real run that only the four averages change and that `SfrDisk` equals the package's `StarFormationRate` diagnostic to float rounding at 10 and 13 substeps.
