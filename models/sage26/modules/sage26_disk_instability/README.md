# `sage26_disk_instability`

Tests the disk for gravitational instability with the Mo, Mao & White (1998) criterion, moves the unstable stars and their metals into the bulge (its instability channel), updates the instability-bulge radius, and hands the unstable cold-gas fraction to `sage26_quasar_mode` and `sage26_starburst_feedback`. It implements SAGE26's `check_disk_instability` up to its black-hole and burst calls.

## Physics

The arithmetic lives in `shared/sage_disk_instability_physics.h`, which the burst transaction (`shared/sage_starburst_physics.h`) also calls for its post-merger instability check.

- The disk mass is `diskmass = ColdGas + (StellarMass - BulgeMass)`; nothing happens unless it is positive.
- The critical mass is `Mcrit = Vmax^2 * (StarFormingDiskFactor * DiskScaleRadius) / G`, from the galaxy's own `Vmax`, capped at `diskmass`.
- The excess `diskmass - Mcrit` is split in proportion to the disk's content: `unstable_gas = (ColdGas / diskmass) * excess` and `unstable_stars` the rest.
- Unstable stars move to `BulgeMass` and `InstabilityBulgeMass`, with metals at the disk's stellar metallicity added to `MetalsBulgeMass`. The transfer is not capped at the disk's stars and the bulge is not clamped to the stellar totals, so `BulgeMass` or `MetalsBulgeMass` can end one float rounding above `StellarMass` or `MetalsStellarMass`, as in SAGE26.
- With `BulgeSizeOn 3`, the instability-bulge radius follows Tonini et al. (2016) Eq. 15 (or `0.2 * DiskScaleRadius` for a first transfer) from the disk radius before the transfer, and the bulge radius is recombined from the merger and instability components (`shared/sage_bulge_size_physics.h`); with `BulgeSizeOn 0` every bulge radius keeps its value.
- `UnstableDiskGasFraction = unstable_gas / ColdGas` where both are positive, otherwise 0.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after the disk radii and `sage26_apply_star_formation_supernova`, and before `sage26_quasar_mode`, `sage26_starburst_feedback` and `sage26_apply_metal_enrichment`
- Receives one galaxy at a time
- A feedback-free-burst row (`FFBRegime` 1) is skipped and nothing on it is written; its `UnstableDiskGasFraction` keeps the per-snapshot reset value 0, so the quasar and burst entries do not act on it. The burst transaction's post-merger check has no FFB test and runs on any target

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. When `sage26_quasar_mode` is configured as `process_by_galaxy` in a substep phase, this module must precede it in that phase — otherwise the quasar mode reads the fraction from the previous substep, or 0 at the first.

**Advisory:** `sage26_starburst_feedback`'s own `init()` logs a WARNING, and the run continues, when this module does not precede it in the same substep phase.

**Not refused:** running with neither consumer; the transfer and radius update still run and the fraction is written but unread. The module's presence as `process_by_galaxy` in a substep phase also switches on the post-minor-merger instability check in `sage26_starburst_feedback`.

## Properties

- Reads: `FFBRegime`, `Vmax`, `ColdGas`, `StellarMass`, `MetalsStellarMass`, `BulgeMass`, `MetalsBulgeMass`, `InstabilityBulgeMass`, `DiskScaleRadius`; with `BulgeSizeOn 3` also `MergerBulgeMass`, `MergerBulgeRadius`, `InstabilityBulgeRadius`
- Writes: `BulgeMass`, `MetalsBulgeMass`, `InstabilityBulgeMass`, `UnstableDiskGasFraction`; with `BulgeSizeOn 3` also `BulgeRadius`, `MergerBulgeRadius`, `InstabilityBulgeRadius`

## Parameters

- `StarFormingDiskFactor`: the number of disk scale radii in the critical mass; required, within [0, 10]; the run files hold 3.0.
- `BulgeSizeOn`: required, 0 (no bulge sizes) or 3 (Tonini et al. 2016 merger and instability bulges, the run files' value); modes 1 and 2 are not supported and, like any other value or a missing key, are refused at initialisation naming the parameter. Mode 3 also reads the core's `Hubble_h`.

## Notes

`UnstableDiskGasFraction` is a transport property that hands the unstable gas to SAGE26's continuation: place `sage26_quasar_mode` (black-hole growth and quasar wind) and then `sage26_starburst_feedback` (the collisional starburst) after this module in the same phase, both `process_by_galaxy`.

## Tests

- `_tests/test_unit_sage26_disk_instability.c`: registration and lifecycle, stable disks (below and at the threshold, zero disk mass) transferring nothing, unstable disks transferring stars and metals, zero `Vmax` and disk radius, the absence of a transfer cap and bulge clamp, the factor's effect on stability, and the skipped FFB row.
- `_tests/test_integration_sage26_disk_instability.py`: runs the module behind a pipeline that supplies disks, checking that unstable disks occur, that the bulge outputs are non-negative and at most one float above the stellar totals, and that nothing leaks.
- The `StarFormingDiskFactor` refusals are covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c`.
