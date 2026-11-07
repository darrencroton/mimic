# `sage26_apply_metal_enrichment`

Applies the instantaneous-recycling metal yield for stars formed in the disk this substep, then consumes (zeroes) `NewStellarMass`.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_apply_star_formation_supernova` and after the disk-instability chain (`sage26_disk_instability`, `sage26_quasar_mode`, `sage26_starburst_feedback`)
- Receives one galaxy at a time and uses the FoF central for the metal ejection destination
- A row whose `FFBRegime` is 1 (written by `sage26_determine_ffb_regime` only with `EnhancedStarFormationOn` 1) is not processed: SAGE26 hands it to `starformation_ffb` and returns before its ordinary yield (`R/src/model_starformation_and_feedback.c:1004-1008`), so the module returns at once and writes nothing and does not consume `NewStellarMass`, which no module writes on that row; the transaction applies its yield from its renormalised stars. `sage26_ffb_star_formation` runs that row's transaction. The module reads no new parameter: `FFBRegime` is 1 only where the classifier wrote it.
- With no FoF central galaxy (a Mimic-only state: SAGE26 always has one) the module returns without writing anything, `NewStellarMass` included. It has no time guard of its own.

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. Only when `sage26_apply_star_formation_supernova` is configured (in any phase): it must precede this module in the same substep phase, both as `process_by_galaxy` (`module_precedes_in_substep_phase`) — the yield is keyed to the stellar mass committed by that apply step.
2. Only when `sage26_starburst_feedback` is configured as `process_by_galaxy` in a substep phase (`module_in_substep_phase`): that entry must precede this module in the same substep phase — SAGE adds the disk-SF yield after `check_disk_instability()`, so the burst must see pre-enrichment metallicity.

**Not refused by this module's `init()`:** `sage26_apply_star_formation_supernova` absent entirely (check 1 is skipped and nothing writes the `NewStellarMass` this module consumes); `sage26_starburst_feedback` absent, or configured only in another processing mode or outside the substep phases (check 2 is skipped); `sage26_disk_instability` and `sage26_quasar_mode` placed anywhere (only the burst's position is checked); `sage26_ffb_star_formation` absent (not checked here).

## Properties

- Reads: `FFBRegime` (1 returns at once), `Type`, `Mvir` and `Regime` (FoF central), `ColdGas`, `MetalsColdGas`, `NewStellarMass`
- Writes: `MetalsColdGas`, `MetalsHotGas` or `MetalsCGMgas` (the FoF central's selected metals), `NewStellarMass` (zeroed after consuming)

## Parameters

| Parameter | Description |
|-----------|-------------|
| `Yield` | Metal yield per unit stellar mass formed. Required, within [0, 1]; the run files hold 0.025 (SAGE26's `input/millennium.par:81`) |
| `FracZleaveDisk` | Fraction of newly produced metals ejected directly to the hot halo. Required, within [0, 1]; the run files hold 0.0 (SAGE26's `input/millennium.par:83`) |
| `CGMrecipeOn` | Required, 0 or 1, no default: 1 sends the escaping metals to the FoF central's `MetalsCGMgas` when its `Regime` is 0 and to its `MetalsHotGas` otherwise (the run files' value, SAGE26's R4, kept through R6); 0 (R0-R3) always to `MetalsHotGas`. Any other value, or a missing key, fails initialisation naming the parameter |

## Notes

The metal split ports SAGE26's disk yield (`src/model_starformation_and_feedback.c:1092-1109` in the staged SAGE26 source), after the disk-instability chain as SAGE26 orders it (`:1088-1090`). It uses `f_Z = FracZleaveDisk * exp(-1.0 * Mvir_central / 30.0)` (Krumholz & Dekel 2011 Eq. 22) with the FoF central's float `Mvir`: if `ColdGas > 1.0e-8`, `MetalsColdGas += Yield * (1 - f_Z) * stars` (`Yield * (1 - f_Z)` first, then one fused multiply-add with `stars` in SAGE26's build, an explicit `fma` here) and the escaping `Yield * f_Z * stars` goes to the central's selected metals; otherwise the full yield does. The escaping metals are formed in their own statement, as SAGE26 forms `metals_leaving_disk`, and added as that completed double through SAGE26's `add_metals_to_hot_reservoir` (`src/model_misc.h:67-74`, `shared/sage_cgm_routing.h`): to the FoF CENTRAL's `MetalsCGMgas` with `CGMrecipeOn 1` and the central's `Regime` 0, to its `MetalsHotGas` otherwise (`:1102`, `:1108`). The yield is added on every call, also with no new stars, as SAGE26 does (a zero yield turns a `-0.0` metal reservoir into `+0.0`). `FracZleaveDisk` is 0 in every configuration R0–R6 (SAGE26's `input/millennium.par:83`) and in the run files. The starburst channel applies its own yield inside the burst kernel (`shared/sage_starburst_physics.h`); this module only handles the quiescent disk-SF yield.

## Tests

- `_tests/test_unit_sage26_apply_metal_enrichment.c`: registration and lifecycle; the refusal of the SF/SN apply step or `sage26_starburst_feedback` ordered after this module; the yield split above the 1e-8 cold-gas threshold and the full yield to the central below it; `NewStellarMass` consumed; no metals with no new stars, and a zero yield still added (turning `-0.0` metals into `+0.0`); a missing galaxy, a missing FoF central and `ngal` other than 1; `test_cgm_yield_destination` runs `CGMrecipeOn` 1 against 0 on the same input for both branches (the FoF central in each `Regime`, the forming satellite in the other, exact literal expectations), and `test_cgm_recipe_on_refused` the parameter's refusals; `test_ffb_row_bypassed` shows that an `FFBRegime` 1 row leaves itself and its FoF central unchanged, with no yield and `NewStellarMass` not consumed.
- `_tests/test_integration_sage26_apply_metal_enrichment.py` runs Mimic with this module alone in `galaxy_physics` (so `NewStellarMass` stays zero and no metals are applied, and neither ordering check applies) and checks that the module initialises and cleans up, logs the `Yield` and `FracZleaveDisk` it read, leaks no memory, and that the output holds `MetalsColdGas` (as float32), `MetalsHotGas`, `ColdGas` and `Type`.
- The chained oracle test of `sage26_calculate_star_formation` (`../sage26_calculate_star_formation/_tests/test_unit_sage26_calculate_star_formation_oracle.c`) runs this module's `process()` last in the disk chain and asserts its yield bit for bit against SAGE26, with each `CGMrecipeOn 1` fixture's yield destination checked against the FoF central's `Regime`.

## References

- SAGE26: `src/model_starformation_and_feedback.c:1092-1109`
- Croton et al. (2006, 2016)
