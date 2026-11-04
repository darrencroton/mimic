# `sage26_apply_star_formation_supernova`

Applies star-formation and supernova transport fields to the persistent galaxy reservoirs. This is an infrastructure commit step, not a swappable star-formation or feedback prescription.

## SAGE26 Source

It commits a disk star-formation event as SAGE26's `starformation_and_feedback()` does (`src/model_starformation_and_feedback.c:1063-1085` in the staged SAGE26 source), through the package's one implementation of SAGE26's `update_from_star_formation` and `update_from_feedback` in `shared/sage_sf_feedback_physics.h`, which both starburst channels also use:

1. `update_from_star_formation` with the cold gas's metallicity: `(1 - RecycleFraction) * stars` moves from `ColdGas` to `StellarMass`, with metals (`:1119-1126`).
2. The reheated mass is clamped to the `ColdGas` left after star formation (`:1079-1082`).
3. `update_from_feedback` with the recomputed metallicity (`:1146-1251`, non-CGM branch): the reheated mass, clamped again to `ColdGas` (`:1153-1156`), moves from the galaxy's cold gas to the FoF central's hot gas; the ejected mass, capped to the central's `HotGas` (`:1232-1234`), moves to its ejected reservoir with `metallicityHot * ejected`, capped separately to the central's `MetalsHotGas` (`:1237-1240`); and `SupernovaOutflowRate` (SAGE26's `OutflowRate`) accumulates the undivided reheated mass in float (`:1249`). `sage26_finalize_cooling_heating` divides it by the interval once per halo.

SAGE26 makes both calls on every substep, also when no stars formed, so this step always runs them: a zero event still runs the caps, which can move a negative metal reservoir. The commits round where SAGE26's build rounds, with explicit `fma` for the two metal updates SAGE26's build fuses (`:1168`, `:1229`). The package's `StarFormationRate` (`+= stars / dT`) is a snapshot-averaged diagnostic, not a SAGE26 output.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_calculate_star_formation` and usually after `sage26_calculate_supernova_feedback`
- Receives one galaxy at a time and uses the FoF central for feedback reservoir transfers

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_calculate_star_formation` must precede this module in the same substep phase (when configured) — applying at end-of-substep without an up-to-date `NewStellarMass` commits stale values from the previous substep.
2. `sage26_calculate_supernova_feedback` must precede this module in the same substep phase (when configured) — same stale-value risk for `SupernovaReheatedMass` and `SupernovaEjectedMass`.

**Advisory (emits WARNING if violated):**

- Neither `sage26_calculate_star_formation` nor `sage26_calculate_supernova_feedback` is configured — all SF/SN transport fields will be zero; likely a configuration mistake.
- `sage26_apply_metal_enrichment` is not configured when SF is active — the disk-SF metal yield will not be applied (SAGE parity loss; metals will be under-produced).

## Properties

- Reads: `dT`, `Mvir`, `ColdGas`, `MetalsColdGas`, `HotGas`, `MetalsHotGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass` (`HotGas`/`MetalsHotGas` are read on the FoF central for reheat/eject reservoir transfers)
- Writes: `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `StarFormationRate`, `SupernovaOutflowRate` (the undivided reheated mass)

## Parameters

- `RecycleFraction`

## Notes

This module consumes transport fields and should remain after the modules that calculate them. Disabling it means calculated star formation or feedback budgets are not committed to galaxy reservoirs. `Yield` and `FracZleaveDisk` belong to `sage26_apply_metal_enrichment`, which applies the disk-SF metal yield in a separate step after the disk-instability chain.

## Tests

`_tests/test_unit_sage26_apply_star_formation_supernova_oracle.c` runs every standalone `update_from_star_formation` and `update_from_feedback` fixture under `_tests/oracle/` through the shared commits, called directly (these fixtures have no star-formation chain input), and asserts bit-for-bit equality of every recorded field and of `update_from_feedback`'s validated locals with the SAGE26 function-level oracle (`../_tests/oracle/README.md`); it reports SKIP outside the oracle environment. The disk path through this module's `process()` is asserted by the chained oracle test of `sage26_calculate_star_formation`. `_tests/test_unit_sage26_apply_star_formation_supernova.c` covers the module's transfers, edge cases (including the zero event) and lifecycle.
