# `sage26_resolve_mergers_and_disruption`

SAGE26's per-substep merger and disruption pass and its two transactions. Once per substep, after every galaxy's physics, the pass walks the FoF's rows in order; each satellite it removes mutates its target at once, so a later satellite in the same pass sees the earlier changes. A merging satellite runs SAGE26's `deal_with_galaxy_merger`, a disrupted one its `disrupt_satellite_to_ICS`.

## The Pass

`sage26_resolve_mergers_and_disruption_pass()` reproduces SAGE26's pass (`src/core_build_model.c:519-587`, `B` below). For each row, in order, a Type 1 or 2 not yet removed (`B:523`; a removed row is the package's Type 3):

1. Its clock must be below 999 (`B:524-527`, where SAGE26 aborts; a NaN clock aborts too). The module errors there.
2. Its clock is decremented by its substep interval `deltaT / N`, the float clock less the double interval (`B:529-530`). `deltaT` is the row's `dT`, which the core forms as SAGE26 does, `Age[SnapNum] - Age[current]`.
3. It is eligible when its float `StellarMass + ColdGas` is exactly zero, or positive with the interpolated virial mass `Mvir - deltaMvir * (1 - (step + 1) / N)` at most `ThresholdSatDisruption` times it (`B:534-537`). The interpolation is not floored at zero (the package's earlier floor is gone): an orphan's `Mvir` is 0 and its `deltaMvir` minus its previous mass, so it reaches 0 at the last substep. SAGE26's objects fuse it into one multiply-subtract, written here as `fma()`.
4. Its target is the FoF central for a Type 1 and its subhalo's central (`CentralHalo`) for a Type 2; a target already removed this snapshot is redirected once through its own `CentralHalo` (`B:539-543`, `shared/central_link.h`). A Type 1's `CentralHalo` is itself, so an orphan whose Type 1 has just merged lands on that merged galaxy, as in SAGE26. There is no fallback to the FoF central for an invalid link (the package's earlier one is gone); an index outside the workspace, a target equal to the satellite or a target without a galaxy is reported as an error rather than read, defences no valid run reaches (the core sets every `CentralHalo` inside its subhalo's slice).
5. A satellite with a non-finite clock stays as it is (`B:547`). Otherwise, at the event time `Age[SnapNum] - (step + 0.5) * (deltaT / N)` (`B:549`, fused in SAGE26's objects, written as `fma()`; `Age[SnapNum]` is the core's `Age[current]` (`ctx->time`) plus the row's `dT`, which the core forms as `Age[SnapNum] - Age[current]`), a positive clock disrupts it to the target's ICS (`B:574-575`) and any other clock merges it into the target, with the substep's history bin and `deltaT / N` (`B:576-581`).

A satellite that is not eligible, or whose clock is not finite, stays and is reconsidered at the next substep. There is no early orphan purge and no forced clock: an orphan's removal is an outcome of the pass. SAGE26's `mergeIntoID` (`B:545`), the output index of the target, has no package member (Mimic's output writes no merger target).

## The Disruption

`sage26_resolve_mergers_and_disruption_disrupt()` runs `mimic_sage_disrupt_satellite_to_ics()` (`shared/sage_merger_physics.h`), SAGE26's `disrupt_satellite_to_ICS` on the R0 path (`src/model_mergers.c:845-902`): the satellite's cold, hot and CGM gas and their metals, each summed in float and only then widened, to the target's hot gas (`CGMrecipeOn 0`); its ejected gas and existing ICS transferred; its stars and stellar metals into the target's ICS; positive stars booked into `ICS_disrupt`, with `StellarMass * time` into `ICS_sum_mt` (one fused multiply-add in SAGE26's objects, written as `fma()`); its black hole into the target's; a positive existing ICS booked into `ICS_accrete` with its `ICS_sum_mt`. Then exactly SAGE26's satellite fields are zeroed (`M:891-900`: the gas, ejected, ICS, ICS bookkeeping, stellar and bulge masses with their metals, and the black hole), and not the two bulge channels, the radii, `H1gas`, `H2gas`, `QuasarModeBHaccretionMass`, the outflow rate, the timestamps or the histories. The satellite becomes Type 3 (SAGE26's `mergeType` 4), reported to the action hook as `"disrupt"`.

## The Merger Transaction

`sage26_resolve_mergers_and_disruption_merge()` reproduces `deal_with_galaxy_merger` on the R0 path (`src/model_mergers.c:228-333`, `M` below), in its order, from the pieces in `shared/sage_merger_physics.h`:

1. The mass ratio from the float `StellarMass + ColdGas` sums of satellite and target, compared in float and widened into doubles; both sums zero gives 0 (`M:235-249`).
2. The target's morphology before the addition: disk-dominated when its float disk mass exceeds half its `StellarMass` (`M:253-254`).
3. `add_galaxies_together` (`M:456-543`): the cold, stellar, hot and ejected reservoirs with their metals; the ICS, a positive satellite ICS booked into `ICS_accrete` with its `ICS_sum_mt` and the satellite's `ICS_disrupt`, `ICS_accrete` and `ICS_sum_mt` cleared; the black hole; the CGM gas; the satellite's stars and metals into the bulge, its `InstabilityBulgeMass` and `MergerBulgeMass` into the target's, and its remaining disk stars into the target's instability channel when the target's disk fraction after the addition exceeds 0.5 (followed, with `BulgeSizeOn 3`, by the target's incremental instability-bulge radius from its own `DiskScaleRadius`, `M:516-522`), else its merger channel; and every bin of the satellite's disk and bulge histories into the target's bulge histories. The satellite's other reservoirs are left as they are.
4. The `merger` event, for every merger, a ratio of 0 included. Its consumers run immediately, in the phase's YAML order: `sage26_quasar_mode` (`grow_black_hole`, `M:262-264`) and then `sage26_starburst_feedback` (the burst, `M:266-287`), which selects its bulge channel from the morphology of step 2, carried on the target in `MergerDiskDominated`.
5. `get_bulge_radius`, which with `BulgeSizeOn 0` zeroes the three bulge radii (`M:293`, `src/model_misc.c:241-246`). SAGE26 then computes the remnant radius (`M:297`), but every branch below zeroes the radii again, so on R0 it never reaches the state; the module does not form it. `BulgeSizeOn 3`'s synchronisation, remnant radius and major/minor assignment (`M:293-330`) are not yet ported: the module zeroes the radii here in both modes.
6. A ratio strictly above `ThresholdMajorMerger` is a major merger: `make_bulge_from_burst` (`M:553-570`; `BulgeMass` and `MergerBulgeMass` take the final `StellarMass`, `InstabilityBulgeMass` is zeroed, `MetalsBulgeMass` takes `MetalsStellarMass`, the disk histories move into the bulge histories) and `TimeOfLastMajorMerger`. Every other merger, a ratio of 0, at or below 0.1, or equal to the threshold included, sets `TimeOfLastMinorMerger` (`M:299-331`). Both timestamps are the event time, a lookback time in code units, written out times `UnitTime_in_Megayears`, -1 (never) included.
7. The satellite becomes Type 3 (SAGE26's `mergeType` 2 or 1), reported to the action hook as `"merge"`.

## Processing Contract

- Supported mode: `process_full_halo`
- Expects the full halo workspace for the current FoF system
- Emits the `merger` event (declared under `events.emits` in `module_info.yaml`): `value0` the mass ratio, `value1` the satellite's substep `dt`; the target carries `MergerDiskDominated`

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_initialise_merger_clock` must be in `pre_timestep` as `process_full_halo` — without it, satellites carry the sentinel `MergTime` value (999.9) from the tree load and the module hard-errors on the first satellite it encounters at runtime.
2. When `sage26_quasar_mode` and `sage26_starburst_feedback` are both merger-event consumers of one phase, the quasar consumer must come first: SAGE26 grows the black hole before the merger burst (`M:262-287`).

## Properties

- Reads: `Type`, `Mvir`, `deltaMvir`, `dT`, `SnapNum`, `CentralHalo`, `MergTime` and every reservoir, channel and history the two transactions move
- Writes: `MergTime`, `Type` (3 after a merger or disruption); in a merger, the target's reservoirs, `BulgeMass`, `MetalsBulgeMass`, `MergerBulgeMass`, `InstabilityBulgeMass`, `ICS_accrete`, `ICS_sum_mt`, `CGMgas`, `MetalsCGMgas`, the three bulge radii (zeroed), the six histories, `MergerDiskDominated`, `TimeOfLastMinorMerger` and `TimeOfLastMajorMerger`, and the satellite's `ICS_disrupt`, `ICS_accrete` and `ICS_sum_mt`; in a disruption, the target's `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `ICS`, `MetalsICS`, `ICS_disrupt`, `ICS_accrete`, `ICS_sum_mt` and `BlackHoleMass`, and the satellite's fields SAGE26 zeroes

## Parameters

- `ThresholdMajorMerger`
- `ThresholdSatDisruption`
- `BulgeSizeOn` (required, 0 or 3): SAGE26's bulge-size mode. 0, the run files' value and SAGE26's R0, computes no sizes (`R/src/model_misc.c:242-247`, `:395`); 3 is SAGE26's Tonini et al. (2016) merger and instability bulges (`:317-424`, `R1.par:109`). Modes 1 and 2 are not ported: any value other than 0 or 3, or a missing key, fails initialisation with an error naming the parameter. Mode 3 also reads the core's `Hubble_h`.

## Tests

- `_tests/test_unit_sage26_resolve_mergers_and_disruption_oracle.c` compares the module with SAGE26's own routines and the oracle's source-reviewed transcriptions bit for bit on every fixture under `_tests/oracle/`: `add_galaxies_together` (with `BulgeSizeOn 3` for the c07 `add_s3_*` fixtures) and `make_bulge_from_burst` through the shared helpers directly; `deal_with_galaxy_merger` through the module's transaction and its real event consumers in order, zero-ratio mergers included; `disrupt_satellite_to_ICS` through the disruption entry; the pass on each pass fixture, with every row's pass record against SAGE26's driver locals and the merger events in the pass's order (several mergers in one bin included); and joining (the core's inheritance, `sage26_initialise_merger_clock` and `sage26_set_disk_scale_radius`) followed by the pass, which shows a new orphan keeping its histories and moving them into its target's bulge histories in a merger. Outside the oracle environment it reports SKIP.
- `_tests/test_unit_sage26_merger_ordering_parity.c` checks the pass's immediate ordering and its one-hop redirect.
- `_tests/test_integration_sage26_merger_event_consumers.py` checks the event chain end to end, the written timestamps against `UnitTime_in_Megayears`, and the merger bulge bounds.
