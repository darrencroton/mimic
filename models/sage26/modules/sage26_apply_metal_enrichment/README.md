# `sage26_apply_metal_enrichment`

Applies the instantaneous-recycling metal yield for stars formed in the disk this substep, then consumes (zeroes) `NewStellarMass`.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_apply_star_formation_supernova` and after the disk-instability chain (`sage26_disk_instability`, `sage26_quasar_mode`, `sage26_starburst_feedback`)
- Receives one galaxy at a time and uses the FoF central for the metal ejection destination
- A row whose `FFBRegime` is 1 (written by `sage26_determine_ffb_regime` only with `EnhancedStarFormationOn` 1) is not processed: SAGE26 hands it to `starformation_ffb` and returns before its ordinary yield (`R/src/model_starformation_and_feedback.c:1004-1008`), so the module returns at once and writes nothing and does not consume `NewStellarMass`, which no module writes on that row; the transaction applies its yield from its renormalised stars. `sage26_ffb_star_formation` runs that row's transaction. The module reads no new parameter: `FFBRegime` is 1 only where the classifier wrote it.

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_apply_star_formation_supernova` must precede this module in the same substep phase — the yield is keyed to the stellar mass committed by that apply step.
2. `sage26_starburst_feedback` (by-galaxy channel) must precede this module — SAGE adds the disk-SF yield after `check_disk_instability()`, so the burst must see pre-enrichment metallicity.

## Properties

- Reads: `FFBRegime` (1 returns at once), `Type`, `Mvir` and `Regime` (FoF central), `ColdGas`, `MetalsColdGas`, `NewStellarMass`
- Writes: `MetalsColdGas`, `MetalsHotGas` or `MetalsCGMgas` (the FoF central's selected metals), `NewStellarMass` (zeroed after consuming)

## Parameters

| Parameter | Description |
|-----------|-------------|
| `Yield` | Metal yield per unit stellar mass formed |
| `FracZleaveDisk` | Fraction of newly produced metals ejected directly to the hot halo |
| `CGMrecipeOn` | Required, 0 or 1 (campaign decision D33): 1 sends the escaping metals to the FoF central's `MetalsCGMgas` when its `Regime` is 0 and to its `MetalsHotGas` otherwise (the run files' value, SAGE26's R4, kept at R5); 0 (R0-R3) always to `MetalsHotGas`. Any other value, or a missing key, fails initialisation naming the parameter |

## Notes

The metal split ports SAGE26's disk yield (`src/model_starformation_and_feedback.c:1092-1109` in the staged SAGE26 source), after the disk-instability chain as SAGE26 orders it (`:1088-1090`). It uses `f_Z = FracZleaveDisk * exp(-1.0 * Mvir_central / 30.0)` (Krumholz & Dekel 2011 Eq. 22) with the FoF central's float `Mvir`: if `ColdGas > 1.0e-8`, `MetalsColdGas += Yield * (1 - f_Z) * stars` (`Yield * (1 - f_Z)` first, then one fused multiply-add with `stars` in SAGE26's build, an explicit `fma` here) and the escaping `Yield * f_Z * stars` goes to the central's selected metals; otherwise the full yield does. The escaping metals are formed in their own statement, as SAGE26 forms `metals_leaving_disk`, and added as that completed double through SAGE26's `add_metals_to_hot_reservoir` (`src/model_misc.h:67-74`, `shared/sage_cgm_routing.h`): to the FoF CENTRAL's `MetalsCGMgas` with `CGMrecipeOn 1` and the central's `Regime` 0, to its `MetalsHotGas` otherwise (`:1102`, `:1108`). The yield is added on every call, also with no new stars, as SAGE26 does (a zero yield turns a `-0.0` metal reservoir into `+0.0`). On R0 `FracZleaveDisk` is 0. The starburst channel applies its own yield inside the burst kernel (`shared/sage_starburst_physics.h`); this module only handles the quiescent disk-SF yield. The chained oracle test of `sage26_calculate_star_formation` asserts it bit for bit against SAGE26. `_tests/test_unit_sage26_apply_metal_enrichment.c`'s `test_cgm_yield_destination` runs `CGMrecipeOn` 1 against 0 on the same input for both branches (the FoF central in each `Regime`, the forming satellite in the other, exact literal expectations), and `test_cgm_recipe_on_refused` the parameter's refusals.

## References

- SAGE26: `src/model_starformation_and_feedback.c:1092-1109`
- Croton et al. (2006, 2016)
