# `sage26_resolve_mergers_and_disruption`

The per-substep merger and disruption pass and SAGE26's merger transaction. The pass decrements `MergTime`, evaluates disruption vs. merger from live substep state, resolves the target, and mutates it immediately, so a later satellite in the same pass sees the earlier changes. A merging satellite runs SAGE26's `deal_with_galaxy_merger`; a disrupting one the package's disruption transfer. The pass itself (`src/core_build_model.c:519-587`) and the disruption (`disrupt_satellite_to_ICS`, `src/model_mergers.c:845-902`) are not yet ported.

## The Merger Transaction

`sage26_resolve_mergers_and_disruption_merge()` reproduces `deal_with_galaxy_merger` on the R0 path (`src/model_mergers.c:228-333`, `M` below), in its order, from the pieces in `shared/sage_merger_physics.h`:

1. The mass ratio from the float `StellarMass + ColdGas` sums of satellite and target, compared in float and widened into doubles; both sums zero gives 0 (`M:235-249`).
2. The target's morphology before the addition: disk-dominated when its float disk mass exceeds half its `StellarMass` (`M:253-254`).
3. `add_galaxies_together` (`M:456-543`): the cold, stellar, hot and ejected reservoirs with their metals; the ICS, a positive satellite ICS booked into `ICS_accrete` with its `ICS_sum_mt` and the satellite's `ICS_disrupt`, `ICS_accrete` and `ICS_sum_mt` cleared; the black hole; the CGM gas; the satellite's stars and metals into the bulge, its `InstabilityBulgeMass` and `MergerBulgeMass` into the target's, and its remaining disk stars into the target's instability channel when the target's disk fraction after the addition exceeds 0.5, else its merger channel; and every bin of the satellite's disk and bulge histories into the target's bulge histories. The satellite's other reservoirs are left as they are.
4. The `merger` event, for every merger, a ratio of 0 included. Its consumers run immediately, in the phase's YAML order: `sage26_quasar_mode` (`grow_black_hole`, `M:262-264`) and then `sage26_starburst_feedback` (the burst, `M:266-287`), which selects its bulge channel from the morphology of step 2, carried on the target in `MergerDiskDominated`.
5. `get_bulge_radius`, which with `BulgeSizeOn 0` zeroes the three bulge radii (`M:293`, `src/model_misc.c:241-246`). SAGE26 then computes the remnant radius (`M:297`), but every branch below zeroes the radii again, so on R0 it never reaches the state; the module does not form it.
6. A ratio strictly above `ThresholdMajorMerger` is a major merger: `make_bulge_from_burst` (`M:553-570`; `BulgeMass` and `MergerBulgeMass` take the final `StellarMass`, `InstabilityBulgeMass` is zeroed, `MetalsBulgeMass` takes `MetalsStellarMass`, the disk histories move into the bulge histories) and `TimeOfLastMajorMerger`. Every other merger, a ratio of 0, at or below 0.1, or equal to the threshold included, sets `TimeOfLastMinorMerger` (`M:299-331`). Both timestamps are the event time, a lookback time in code units, written out times `UnitTime_in_Megayears`, -1 (never) included.
7. The satellite becomes Type 3 (SAGE26's `mergeType` 2 or 1).

## Processing Contract

- Supported mode: `process_full_halo`
- Expects the full halo workspace for the current FoF system
- Emits the `merger` event (declared under `events.emits` in `module_info.yaml`): `value0` the mass ratio, `value1` the satellite's substep `dt`; the target carries `MergerDiskDominated`

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_initialise_merger_clock` must be in `pre_timestep` as `process_full_halo` — without it, satellites carry the sentinel `MergTime` value (999.9) from the tree load and the module hard-errors on the first satellite it encounters at runtime.
2. When `sage26_quasar_mode` and `sage26_starburst_feedback` are both merger-event consumers of one phase, the quasar consumer must come first: SAGE26 grows the black hole before the merger burst (`M:262-287`).

## Properties

- Reads: `Type`, `Mvir`, `deltaMvir`, `dT`, `SnapNum`, `CentralHalo`, `MergTime` and every reservoir, channel and history the transaction moves
- Writes: `MergTime`, `Type` (3 after a merger or disruption), the target's reservoirs, `BulgeMass`, `MetalsBulgeMass`, `MergerBulgeMass`, `InstabilityBulgeMass`, `ICS_accrete`, `ICS_sum_mt`, `CGMgas`, `MetalsCGMgas`, the three bulge radii (zeroed), the six histories, `MergerDiskDominated`, `TimeOfLastMinorMerger` and `TimeOfLastMajorMerger`; the satellite's `ICS_disrupt`, `ICS_accrete` and `ICS_sum_mt`; plus the fields `mimic_sage_disruption_transfer` modifies

## Parameters

- `ThresholdMajorMerger`
- `ThresholdSatDisruption`

## Tests

- `_tests/test_unit_sage26_resolve_mergers_and_disruption_oracle.c` compares the transaction with SAGE26's own routines bit for bit on the SAGE26 oracle fixtures under `_tests/oracle/`: `add_galaxies_together` and `make_bulge_from_burst` through the shared helpers directly, `deal_with_galaxy_merger` through the module's transaction and its real event consumers in order, zero-ratio mergers included, and two mergers into one target in one substep and bin. The disruption, pass and joining fixtures are reported as deferred until those routines are ported. Outside the oracle environment it reports SKIP.
- `_tests/test_unit_sage26_merger_ordering_parity.c` checks the pass's immediate ordering and its one-hop redirect.
- `_tests/test_integration_sage26_merger_event_consumers.py` checks the event chain end to end, the written timestamps against `UnitTime_in_Megayears`, and the merger bulge bounds.
