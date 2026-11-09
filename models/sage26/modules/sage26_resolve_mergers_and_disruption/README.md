# `sage26_resolve_mergers_and_disruption`

SAGE26's per-substep merger and disruption pass with its two transactions, `deal_with_galaxy_merger` and `disrupt_satellite_to_ICS`. Once per substep, after every galaxy's physics, the pass walks the FoF's rows in order; each satellite it removes changes its target at once, so a later satellite in the same pass sees the earlier changes. Each merger emits a `merger` event whose consumers (black-hole growth, then the burst) run immediately. The physics pieces live in `shared/sage_merger_physics.h` and `shared/sage_bulge_size_physics.h`.

## Physics

**The pass.** For each Type 1 or 2 not yet removed: its `MergTime` must be below 999 (otherwise the module errors, as SAGE26 aborts) and is decremented by the substep interval `dT / N`. The satellite is eligible when its `StellarMass + ColdGas` is zero, or positive with the interpolated halo mass `Mvir - deltaMvir * (1 - (step + 1) / N)` at most `ThresholdSatDisruption` times it; an orphan's interpolated mass reaches 0 at the last substep. Its target is the FoF central for a Type 1 and its subhalo's central (`CentralHalo`) for a Type 2, redirected once through the target's own `CentralHalo` if the target was removed earlier this snapshot (`shared/central_link.h`). An eligible satellite with a positive clock is disrupted; one with a clock at or below zero merges; one with a non-finite clock stays. A satellite that stays is reconsidered at the next substep. The event time is the lookback time at the middle of the substep.

**Disruption.** The satellite's cold, hot and CGM gas and metals go to the target's hot reservoir, or with `CGMrecipeOn 1` and the target's `Regime` 0 to its CGM (`shared/sage_cgm_routing.h`); its ejected gas, ICS and black hole go to the target's; its stars become intracluster stars, booked into `ICS_disrupt` and `ICS_sum_mt`. The satellite's masses are zeroed and it becomes Type 3.

**Merger.** In order: the baryonic mass ratio (`StellarMass + ColdGas` of the smaller over the larger; 0 when both are zero); the target's morphology before the addition (disk-dominated when its disk exceeds half its stars, carried in `MergerDiskDominated`); `add_galaxies_together`, moving every reservoir (CGM to CGM, hot to hot), the satellite's stars into the bulge, its bulge channels into the target's, and its SFR histories into the target's bulge histories; the `merger` event, for every merger including a ratio of 0; then, with `BulgeSizeOn 3`, the merger remnant radius. A ratio strictly above `ThresholdMajorMerger` is a major merger: the whole stellar mass becomes a merger bulge, the disk histories move into the bulge histories, and `TimeOfLastMajorMerger` is set. Any other merger sets `TimeOfLastMinorMerger`. The satellite becomes Type 3.

**Bulge sizes (`BulgeSizeOn 3`).** The merger and instability bulges follow Tonini et al. (2016). The remnant radius comes from energy conservation over the two progenitors, each with the mass-weighted half-mass radius of its disk (1.68 `DiskScaleRadius`) and bulge, adding their orbital energy and the radiative loss of Covington et al. (2011); it is stored in `MergerBulgeRadius` after a major merger or a minor merger onto a target that was spheroid-dominated before the addition, and discarded for a minor merger onto a disk-dominated one. In the addition, the satellite's disk stars join the target's instability bulge when the target stays disk-dominated (with the incremental instability-bulge radius from the target's `DiskScaleRadius`), and its merger bulge otherwise. With `BulgeSizeOn 0` the three bulge radii are zero.

## Processing Contract

- Supported mode: `process_full_halo`; a `process_by_galaxy` configuration fails at startup
- Receives the full FoF workspace
- Emits the `merger` event: `value0` the mass ratio, `value1` the satellite's substep `dt`; the target carries `MergerDiskDominated`

## Ordering

- `init()` refuses a run without `sage26_initialise_merger_clock` in `pre_timestep` as `process_full_halo` (satellites would keep the unset clock 999.9), and, when this module is itself in `pre_timestep`, a run in which the clock module does not precede it.
- When `sage26_quasar_mode` and `sage26_starburst_feedback` are both `process_per_event` in one substep phase, `init()` refuses the order unless the quasar consumer comes first.
- Not refused: running with no merger-event consumer or only one. Without `sage26_quasar_mode` a merger grows no black hole; without `sage26_starburst_feedback` it triggers no burst.

## Properties

- Reads: `Type`, `Mvir`, `deltaMvir`, `dT`, `SnapNum`, `CentralHalo`, `MergTime`, `DiskScaleRadius`, the disruption target's `Regime` (with `CGMrecipeOn 1`), and every reservoir, bulge channel and history the transactions move
- Writes: `MergTime`, `Type` (3 after a merger or disruption); the target's reservoirs, `BulgeMass`, `MetalsBulgeMass`, `MergerBulgeMass`, `InstabilityBulgeMass`, `BulgeRadius`, `MergerBulgeRadius`, `InstabilityBulgeRadius`, `ICS`, `MetalsICS`, `ICS_disrupt`, `ICS_accrete`, `ICS_sum_mt`, `BlackHoleMass`, the SFR histories, `MergerDiskDominated`, `TimeOfLastMinorMerger`, `TimeOfLastMajorMerger`; and the satellite's zeroed masses

## Parameters

All four are required with no default; a missing key or a value outside the accepted range fails initialisation naming the parameter.

- `ThresholdMajorMerger` (in [0, 1]): a mass ratio strictly above it is a major merger.
- `ThresholdSatDisruption` (in (0, 1000]): the halo-to-baryon mass ratio at or below which a satellite is eligible to merge or disrupt.
- `BulgeSizeOn` (0 or 3): 3 (the shipped value and SAGE26's default) computes the Tonini et al. (2016) bulge sizes, also reading the core's `Hubble_h`; 0 computes none. SAGE26's modes 1 and 2 are not supported; refused at initialisation.
- `CGMrecipeOn` (0 or 1): 1 (the shipped value) deposits a disrupted satellite's gas in the reservoir the target's `Regime` selects; 0 always in its hot gas.

## Notes

`TimeOfLastMinorMerger` and `TimeOfLastMajorMerger` hold the merger's lookback time in code units and are written multiplied by `UnitTime_in_Megayears`, as SAGE26 writes them, so a galaxy that never had such a merger is written as -1 times that factor rather than -1. Mimic writes no merger-target index (SAGE26's `mergeIntoID`).

## Tests

- `_tests/test_unit_sage26_merger_ordering_parity.c`: the pass's immediate ordering and one-hop redirect, the `BulgeSizeOn 3` refusal without core parameters, the disruption's reservoir selection, and the parameter refusals.
- `_tests/test_integration_sage26_merger_event_consumers.py`: the startup refusal of the wrong mode, the merger event chain end to end, the written timestamps and the merger bulge bounds.
