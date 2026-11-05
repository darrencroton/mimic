# `sage26_starburst_feedback`

Ports SAGE26's collisional starburst, `collisional_starburst_recipe` (`src/model_mergers.c:584-831` in the staged SAGE26 source), on the R0 path with `BulgeSizeOn` 0 or 3, for disk-instability triggers and merger events.

## The Burst Transaction

One shared function, `mimic_apply_collisional_starburst` (`shared/sage_starburst_physics.h`), owns SAGE26's whole sequence for both channels:

1. The bursting fraction: the trigger itself for a disk instability (`mode 1`) and `0.56 * ratio^0.7` for a merger (`mode 0`), as SAGE26's code has them (`:600-604`; its comment at `:577` has the modes reversed).
2. The burst gas is the host's total `ColdGas` (`StarburstColdGasOn 1`), floored at zero, times the fraction; the stars are floored at zero (`:680-688`).
3. The reheated mass `FeedbackReheatingEpsilon * stars`, both scaled down together when they exceed the burst gas (`:711-725`).
4. The ejected mass from the burst host's own `Vvir`, squared in float (`:727-749`), not the FoF central's.
5. The bulge history is sampled for every burst, zero stars included: `SfrBulge[bin] += stars / dt`, `SfrBulgeColdGas[bin] += ColdGas`, `SfrBulgeColdGasMetals[bin] += MetalsColdGas` (`:754-757`).
6. The star-formation commit (`update_from_star_formation`), then `recycled_stars = (1 - RecycleFraction) * stars` into `BulgeMass`, `MetalsBulgeMass` (one fused multiply-add, `:773`) and the channel the caller selects, `MergerBulgeMass` or `InstabilityBulgeMass` (`:770-782`). The instability channel is followed at once by the incremental instability-bulge radius from the caller's saved disk radius, the transaction's `old_disk_radius` argument (`:783-784`, `shared/sage_bulge_size_physics.h`; a no-op with `BulgeSizeOn 0`); the merger channel writes no radius.
7. The feedback commit (`update_from_feedback`), reheated gas and ejection going to the FoF central (`:787-791`).
8. For a merger burst whose ratio is strictly below `ThresholdMajorMerger`, the post-minor-merger `check_disk_instability` on the host, whose gas branch grows the black hole (with its quasar wind) and bursts again in `mode 1` into the instability channel (`:806-811`).
9. Only then the burst's metal yield: retained in the host's cold gas (less the fraction leaving the disk) when the host keeps more than `1e-8` of cold gas and the ratio is strictly below the threshold, otherwise all to the FoF central's hot gas (`:813-830`). A ratio equal to the threshold is a minor merger in SAGE26 but gets no recheck and sends all its yield to hot gas.

There is no early return on a non-positive trigger, zero cold gas or zero stars. The star-formation and feedback commits are the package's one implementation, `shared/sage_sf_feedback_physics.h`.

## Channels

- `process_by_galaxy`: the disk instability (`UnstableDiskGasFraction > 0`, `mode 1`, the instability channel), after `sage26_disk_instability` and `sage26_quasar_mode`, as SAGE26's `check_disk_instability` grows the black hole and then bursts (`src/model_disk_instability.c:101-108`). `dt` is the galaxy's own `dT / num_substeps` (`src/core_build_model.c:516`).
- `process_per_event`: the merger event (`mode 0`), after `sage26_quasar_mode`'s merger black-hole growth. `dt` is the merging satellite's substep interval, carried in the event's `value1` (`src/core_build_model.c:579-581`, `src/model_mergers.c:285`, `:809`). The bulge channel is SAGE26's (`src/model_mergers.c:266-282`): `MergerBulgeMass` for a major merger (ratio strictly above `ThresholdMajorMerger`), and for a minor merger `InstabilityBulgeMass` onto a target that was disk-dominated before the satellite was added and `MergerBulgeMass` otherwise, from the flag the resolver carries on the target in `MergerDiskDominated` (`:251-254`). Every merger bursts, a ratio of 0 included.

Each channel passes SAGE26's `old_disk_radius`: the galaxy's current `DiskScaleRadius` for the disk instability, as `check_disk_instability` passes it (`src/model_disk_instability.c:106-107`), and the target's `DiskScaleRadius` for a merger, which SAGE26 saves before `add_galaxies_together` (`src/model_mergers.c:257`); no package module writes `DiskScaleRadius` between that point of the resolver's transaction and this consumer (only `sage26_set_disk_scale_radius` writes it, at pre-timestep), so the target's field is the saved value. The recheck saves and passes its own. The bin is the substep's, `(substep * 10) / num_substeps` capped at 9. The post-minor-merger recheck runs when `sage26_disk_instability` is configured as `process_by_galaxy` (SAGE26's `DiskInstabilityOn`); its black-hole growth runs when `sage26_quasar_mode` is configured as `process_per_event` (SAGE26's `AGNrecipeOn`), with `BlackHoleGrowthRate` and `QuasarModeEfficiency`. `sage26_finalize_sfr_histories` averages the bulge history into `SfrBulge` and `SfrBulgeZ` after the last substep. The package's `StarFormationRate` diagnostic, which SAGE26 does not hold, adds `stars / dT` with the snapshot interval.

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Mvir`, `Vvir`, `Vmax`, `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, `BulgeMass`, `MetalsBulgeMass`, `MergerBulgeMass`, `InstabilityBulgeMass`, the three bulge radii (`BulgeSizeOn` 3), `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `BlackHoleMass`, `QuasarModeBHaccretionMass`, `DiskScaleRadius`, `UnstableDiskGasFraction`
- Writes: `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, `BulgeMass`, `MetalsBulgeMass`, `MergerBulgeMass`, `InstabilityBulgeMass`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `StarFormationRate`, `SupernovaOutflowRate`, the bulge history elements `SfrBulgeBin<k>`, `SfrBulgeColdGasBin<k>`, `SfrBulgeColdGasMetalsBin<k>` of the substep's bin, and, through the post-minor-merger recheck's black-hole growth, `BlackHoleMass` and `QuasarModeBHaccretionMass`; with `BulgeSizeOn 3`, the three bulge radii (the instability channel's updates)

## Parameters

- `FeedbackReheatingEpsilon`
- `FeedbackEjectionEfficiency`
- `RecycleFraction`
- `Yield`
- `FracZleaveDisk`
- `StarFormingDiskFactor`
- `BlackHoleGrowthRate`
- `QuasarModeEfficiency`
- `ThresholdMajorMerger`
- `BulgeSizeOn` (required, 0 or 3): SAGE26's bulge-size mode. 0, SAGE26's R0, computes no sizes (`R/src/model_misc.c:242-247`, `:395`); 3, the run files' value, is SAGE26's Tonini et al. (2016) merger and instability bulges (`:317-424`, `R1.par:109`). Modes 1 and 2 are not ported: any value other than 0 or 3, or a missing key, fails initialisation with an error naming the parameter. Mode 3 also reads the core's `Hubble_h`.

## Ordering

**Enforced at init by event subscription validation (fails with ERROR if violated):**

1. When used as `process_per_event`, `sage26_resolve_mergers_and_disruption` must be configured as `process_full_halo` in the same substep phase — it is the only producer of `merger` events that this module consumes.

**Advisory (emits WARNING if violated):**

- `process_by_galaxy` mode: `sage26_disk_instability` should precede this module in the same substep phase — without it, `UnstableDiskGasFraction` is zero and the disk-instability starburst channel fires on no galaxies.
- `process_per_event` mode (when the post-merger disk-instability recheck is active): `sage26_quasar_mode` as `process_per_event` is needed for the recheck's black-hole growth, which SAGE26 runs before the recheck's burst.

## Events

Consumes `merger` events from `sage26_resolve_mergers_and_disruption`.

## Tests

`_tests/test_unit_sage26_starburst_feedback_oracle.c` runs every SAGE26 oracle fixture under `_tests/oracle/` through `mimic_apply_collisional_starburst` directly, with the fixture's ratio, mode, channel, saved disk radius, burst host, FoF central, bin and substep `dt` standing in for the trigger and `BulgeSizeOn` 0 or 3 (the c07 `burst_s3_*` fixtures), and compares the result bit for bit with SAGE26's own routine; outside the oracle environment it reports SKIP.
