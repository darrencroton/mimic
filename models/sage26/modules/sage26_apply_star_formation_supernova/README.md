# `sage26_apply_star_formation_supernova`

Applies star-formation and supernova transport fields to the persistent galaxy reservoirs. This is an infrastructure commit step, not a swappable star-formation or feedback prescription.

## SAGE26 Source

It samples the disk star-formation history and commits a disk star-formation event as SAGE26's `starformation_and_feedback()` does (`src/model_starformation_and_feedback.c:1058-1085` in the staged SAGE26 source), through the package's history helper `shared/sage_sfr_history.h` and its one implementation of SAGE26's `update_from_star_formation` and `update_from_feedback` in `shared/sage_sf_feedback_physics.h`, which both starburst channels also use:

0. The disk history sample at the substep's bin, before any reservoir changes (`:1058-1060`): `SfrDisk[bin] += stars / dt` (the float element plus the double quotient, rounded once), `SfrDiskColdGas[bin] = ColdGas` and `SfrDiskColdGasMetals[bin] = MetalsColdGas`. `stars` is the renormalised mass `sage26_calculate_supernova_feedback` leaves in `NewStellarMass`, `dt` the substep interval `dT / num_substeps` and `bin` is `(substep * 10) / num_substeps`, capped at 9 (`src/core_build_model.c:514-515`). The elements are the package-private `SfrDiskBin<k>`, `SfrDiskColdGasBin<k>` and `SfrDiskColdGasMetalsBin<k>` properties; `sage26_finalize_sfr_histories` averages them into `SfrDisk` and `SfrDiskZ` after the last substep.
1. `update_from_star_formation` with the cold gas's metallicity: `(1 - RecycleFraction) * stars` moves from `ColdGas` to `StellarMass`, with metals (`:1119-1126`). With `SFprescription 1` it then caps `H2gas` and `H1gas`, each on its own and strictly, at the float `max_h = 0.74` of the remaining `ColdGas` (0 when none is left, `:1128-1135`); the caps never act on their sum.
2. The reheated mass is clamped to the `ColdGas` left after star formation (`:1079-1082`).
3. `update_from_feedback` with the recomputed metallicity (`:1146-1251`, non-CGM branch): the reheated mass, clamped again to `ColdGas` (`:1153-1156`), moves from the galaxy's cold gas to the FoF central's hot gas; the ejected mass, capped to the central's `HotGas` (`:1232-1234`), moves to its ejected reservoir with `metallicityHot * ejected`, capped separately to the central's `MetalsHotGas` (`:1237-1240`); with `SFprescription 1`, after the cold-gas and metal debits and before the hot-gas credit, `H2gas` and `H1gas` are capped at 0.74 of the cold gas left (`:1169-1173`); and `SupernovaOutflowRate` (SAGE26's `OutflowRate`) accumulates the undivided reheated mass in float (`:1249`). `sage26_finalize_cooling_heating` divides it by the interval once per halo.

SAGE26 samples and makes both calls on every substep, also when no stars formed, so this step always does: a zero event still assigns the gas samples and runs the caps, which can move a negative metal reservoir. (Two Mimic-only degenerate paths: with no FoF central galaxy the step samples the history but returns before committing anything, as SAGE26 always has a central; on the initial-snapshot boundary, where the core gives no positive interval and `sage26_calculate_star_formation` forms nothing, there is no `dt` and nothing is sampled, as SAGE26 always has a positive interval. A non-positive interval off that boundary is an error.) The commits round where SAGE26's build rounds, with explicit `fma` for every multiply-add SAGE26's build fuses: the four updates of `update_from_star_formation` (`:1123-1126`) and the two metal updates of `update_from_feedback` (`:1168`, `:1229`). The package's `StarFormationRate` (`+= stars / dT`) is a snapshot-averaged diagnostic, not a SAGE26 output.

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

- Reads: `dT`, `Mvir`, `ColdGas`, `MetalsColdGas`, `HotGas`, `MetalsHotGas`, `NewStellarMass`, `SupernovaReheatedMass`, `SupernovaEjectedMass` (`HotGas`/`MetalsHotGas` are read on the FoF central for reheat/eject reservoir transfers), and with `SFprescription 1` `H2gas` and `H1gas`
- Writes: `ColdGas`, `MetalsColdGas`, `StellarMass`, `MetalsStellarMass`, `HotGas`, `MetalsHotGas`, `EjectedGas`, `MetalsEjectedGas`, `StarFormationRate`, `SupernovaOutflowRate` (the undivided reheated mass), `H2gas` and `H1gas` (the caps, `SFprescription 1` only), and the disk history elements `SfrDiskBin<k>`, `SfrDiskColdGasBin<k>`, `SfrDiskColdGasMetalsBin<k>` of the substep's bin

## Parameters

- `RecycleFraction`
- `SFprescription` (required, 0 or 1): 0 (the run files' R1 value) commits without the hydrogen caps, 1 (BR06, R2) with them (`:1131`, `:1169`); any other value, or a missing key, fails initialisation with an error naming the parameter.

## Notes

This module consumes transport fields and should remain after the modules that calculate them. Disabling it means calculated star formation or feedback budgets are not committed to galaxy reservoirs. `Yield` and `FracZleaveDisk` belong to `sage26_apply_metal_enrichment`, which applies the disk-SF metal yield in a separate step after the disk-instability chain.

## Tests

`_tests/test_unit_sage26_apply_star_formation_supernova_oracle.c` runs every standalone `update_from_star_formation` and `update_from_feedback` fixture under `_tests/oracle/` through the shared commits, called directly (these fixtures have no star-formation chain input), and asserts bit-for-bit equality of every recorded field and of `update_from_feedback`'s validated locals with the SAGE26 function-level oracle (`../_tests/oracle/README.md`), and on the `SFprescription 1` fixtures (`ufsf_r2_*`, `uff_r2_*`) each cap's guard, binding flags and bound `max_h`; it reports SKIP outside the oracle environment. The disk path through this module's `process()` is asserted by the chained oracle test of `sage26_calculate_star_formation`. `_tests/test_unit_sage26_apply_star_formation_supernova.c` covers the module's transfers, edge cases (including the zero event), the disk history sampling (on every call over a 13-substep snapshot, zero-star calls included, and the two degenerate paths) and lifecycle.
