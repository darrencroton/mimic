# `sage26_starburst_feedback`

Applies collisional starburst physics from disk-instability triggers and merger events, including associated feedback and reservoir updates.

## Bulge Star-Formation History

Every burst samples the bursting galaxy's bulge history in the shared kernel (`shared/sage_starburst_physics.h`) once its mass is final and before it commits the star formation, as SAGE26's `collisional_starburst_recipe` does (`src/model_mergers.c:755-757` in the staged SAGE26 source): `SfrBulge[bin] += stars / dt`, `SfrBulgeColdGas[bin] += ColdGas` and `SfrBulgeColdGasMetals[bin] += MetalsColdGas`, through `shared/sage_sfr_history.h`. The bin is the substep's, `(substep * 10) / num_substeps` capped at 9. `dt` is SAGE26's substep interval for the burst:

- the disk-instability burst (`process_by_galaxy`): the galaxy's own `dT / num_substeps`, the `dt` SAGE26's `starformation_and_feedback` hands to `check_disk_instability` and on to the burst (`src/core_build_model.c:516`, `src/model_disk_instability.c:106`);
- the merger burst and its post-merger instability recheck (`process_per_event`): the merging satellite's substep interval, which `sage26_resolve_mergers_and_disruption` carries in the merger event's `value1`, as SAGE26 passes `deltaT / effective_steps` of the satellite to `deal_with_galaxy_merger` (`src/core_build_model.c:579-581`), which hands it to the burst and to `check_disk_instability` (`src/model_mergers.c:285`, `:809`).

The burst arithmetic itself is unchanged; the package's `StarFormationRate` diagnostic keeps its snapshot interval `dT`. `sage26_finalize_sfr_histories` averages the bulge history into `SfrBulge` and `SfrBulgeZ` after the last substep. A merger with a zero mass ratio forms no burst here (the event is skipped), where SAGE26 still calls the burst and samples the cold gas: the burst orchestration is the merger node's.

## Processing Contract

- Supported modes: `process_by_galaxy`, `process_per_event`
- `process_by_galaxy` path consumes disk-instability triggers in `galaxy_physics`
- `process_per_event` path consumes merger events in `satellite_mergers`

## Properties

- Reads: `Type`, `HaloNr`, `SnapNum`, `dT`, `Mvir`, `Vvir`, `ColdGas`, `MetalsColdGas`, `StellarMass`, `BulgeMass`, `HotGas`, `EjectedGas`, `BlackHoleMass`, `DiskScaleRadius`, `UnstableDiskGasFraction`
- Writes: `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, `BulgeMass`, `MetalsBulgeMass`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `StarFormationRate`, `SupernovaOutflowRate`, and the bulge history elements `SfrBulgeBin<k>`, `SfrBulgeColdGasBin<k>`, `SfrBulgeColdGasMetalsBin<k>` of the substep's bin

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

## Ordering

**Enforced at init by event subscription validation (fails with ERROR if violated):**

1. When used as `process_per_event`, `sage26_resolve_mergers_and_disruption` must be configured as `process_full_halo` in the same substep phase — it is the only producer of `merger` events that this module consumes.

**Advisory (emits WARNING if violated):**

- `process_by_galaxy` mode: `sage26_disk_instability` should precede this module in the same substep phase — without it, `UnstableDiskGasFraction` is zero and the disk-instability starburst channel fires on no galaxies.
- `process_per_event` mode (when the post-merger disk-instability recheck is active): `sage26_quasar_mode` as `process_per_event` is recommended in the same substep phase to reproduce SAGE's ordering of BH growth and quasar winds before the burst.

## Events

Consumes `merger` events from `sage26_resolve_mergers_and_disruption`.
