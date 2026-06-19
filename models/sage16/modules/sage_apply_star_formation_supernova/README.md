# `sage_apply_star_formation_supernova`

Applies star-formation and supernova transport fields to the persistent galaxy reservoirs. This is an infrastructure commit step, not a swappable star-formation or feedback prescription.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage_calculate_star_formation` and usually after `sage_calculate_supernova_feedback`
- Receives one galaxy at a time and uses the FoF central for feedback reservoir transfers

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage_calculate_star_formation` must precede this module in the same substep phase (when configured) — applying at end-of-substep without an up-to-date `NewStellarMass` commits stale values from the previous substep.
2. `sage_calculate_supernova_feedback` must precede this module in the same substep phase (when configured) — same stale-value risk for `SupernovaReheatedMass` and `SupernovaEjectedMass`.

**Advisory (emits WARNING if violated):**

- Neither `sage_calculate_star_formation` nor `sage_calculate_supernova_feedback` is configured — all SF/SN transport fields will be zero; likely a configuration mistake.
- `sage_apply_metal_enrichment` is not configured when SF is active — the disk-SF metal yield will not be applied (SAGE parity loss; metals will be under-produced).

## Properties

- Reads: `dT`, `Mvir`, `ColdGas`, `MetalsColdGas`, `HotGas`, `MetalsHotGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass` (`HotGas`/`MetalsHotGas` are read on the FoF central for reheat/eject reservoir transfers)
- Writes: `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `StarFormationRate`, `SupernovaOutflowRate`

## Parameters

- `RecycleFraction`
- `Yield`
- `FracZleaveDisk`

## Notes

This module consumes transport fields and should remain after the modules that calculate them. Disabling it means calculated star formation or feedback budgets are not committed to galaxy reservoirs.
