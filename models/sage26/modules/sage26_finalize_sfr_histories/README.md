# `sage26_finalize_sfr_histories`

Averages a snapshot's per-substep star-formation histories into four outputs, once per FoF group after the last substep: the mean disk and bulge star-formation rates `SfrDisk` and `SfrBulge` (Msun/yr) and the mean metallicities of the gas forming them, `SfrDiskZ` and `SfrBulgeZ` (dimensionless). It also records the substep count in `SubstepsUsed`.

## Processing Contract

- Supported mode: `process_full_halo`
- Required phase: `post_timestep` only, after every substep and merger phase; in a substep phase it would average histories still being sampled
- Receives the full FoF workspace; every galaxy row is averaged, including galaxies merged or disrupted this snapshot, and rows without a galaxy are skipped
- Independent of `sage26_finalize_cooling_heating` (neither reads what the other writes); the run files place this module after it

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. No entry of this module in `pre_timestep` or a substep phase, and exactly one entry in `post_timestep`, as `process_full_halo`.
2. `sage26_initialise_merger_clock` must be configured in `pre_timestep` as `process_full_halo` — its reset at joining is the histories' only reset, and without it they would accumulate across snapshots and every average would be wrong.

**Not refused:** its order among the other `post_timestep` modules (`sage26_record_transition_state` requires itself to be last); a run in which no module samples the histories, when the averages are formed from whatever the histories hold.

## Properties

- Reads: `SfrDiskBin<k>`, `SfrBulgeBin<k>`, `SfrDiskColdGasBin<k>`, `SfrDiskColdGasMetalsBin<k>`, `SfrBulgeColdGasBin<k>`, `SfrBulgeColdGasMetalsBin<k>` (`k = 0..9`)
- Writes: `SubstepsUsed`, `SfrDisk`, `SfrBulge`, `SfrDiskZ`, `SfrBulgeZ`

## Parameters

None. The unit factors are the run's (`UnitMass_in_g`, `UnitTime_in_s`).

## Notes

The histories are six package-private float arrays of ten bins: the disk and bulge star-formation rates (1e10 Msun/h per code time) and the cold gas and metals sampled with each. Substep `s` of `N` writes bin `(s * 10) / N`, capped at 9; the disk is sampled by `sage26_apply_star_formation_supernova` and `sage26_ffb_star_formation`, the bulge by every burst in `sage26_starburst_feedback`. The bins and divisors follow the run's substep count, as the substep `dt` does.

The averaging is SAGE26's HDF5-writer arithmetic, `mimic_sage_average_sfr_histories` (`shared/sage_sfr_history.h`): each rate is the sum over the bins of the element converted to Msun/yr, divided by the substep count (10 if it is zero); each metallicity is the sum, over bins whose sampled gas is positive, of metals over gas, divided by the smaller of the substep count and 10. The results are written as they are, with no `output_convert`.

The histories carry over between snapshots. `sage26_initialise_merger_clock` zeroes them at joining for a galaxy that stays a Type 0 or 1; an orphan keeps its histories, and `sage26_resolve_mergers_and_disruption` moves them in mergers. This module reads them and does not change them.

## Tests

- `_tests/test_unit_sage26_finalize_sfr_histories.c`: the placement check, the refusal without `sage26_initialise_merger_clock`, `SubstepsUsed` on every row, and the averaging and its divisors on hand-checkable values.
- `_tests/test_integration_sage26_finalize_sfr_histories.py`: checks on a real run that only the four averages change, and that `SfrDisk` equals the `StarFormationRate` diagnostic to float rounding at 10 and 13 substeps.
