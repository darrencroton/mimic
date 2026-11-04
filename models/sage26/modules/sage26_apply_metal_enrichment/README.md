# `sage26_apply_metal_enrichment`

Applies the instantaneous-recycling metal yield for stars formed in the disk this substep, then consumes (zeroes) `NewStellarMass`.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_apply_star_formation_supernova` and after the disk-instability chain (`sage26_disk_instability`, `sage26_quasar_mode`, `sage26_starburst_feedback`)
- Receives one galaxy at a time and uses the FoF central for the metal ejection destination

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage26_apply_star_formation_supernova` must precede this module in the same substep phase — the yield is keyed to the stellar mass committed by that apply step.
2. `sage26_starburst_feedback` (by-galaxy channel) must precede this module — SAGE adds the disk-SF yield after `check_disk_instability()`, so the burst must see pre-enrichment metallicity.

## Properties

- Reads: `Type`, `Mvir` (FoF central), `ColdGas`, `MetalsColdGas`, `NewStellarMass`
- Writes: `MetalsColdGas`, `MetalsHotGas` (FoF central), `NewStellarMass` (zeroed after consuming)

## Parameters

| Parameter | Description |
|-----------|-------------|
| `Yield` | Metal yield per unit stellar mass formed |
| `FracZleaveDisk` | Fraction of newly produced metals ejected directly to the hot halo |

## Notes

The metal split ports SAGE26's disk yield (`src/model_starformation_and_feedback.c:1092-1109` in the staged SAGE26 source), after the disk-instability chain as SAGE26 orders it (`:1088-1090`). It uses `f_Z = FracZleaveDisk * exp(-1.0 * Mvir_central / 30.0)` (Krumholz & Dekel 2011 Eq. 22) with the FoF central's float `Mvir`: if `ColdGas > 1.0e-8`, `MetalsColdGas += Yield * (1 - f_Z) * stars` (`Yield * (1 - f_Z)` first, then one fused multiply-add with `stars` in SAGE26's build, an explicit `fma` here) and the central's `MetalsHotGas += Yield * f_Z * stars`; otherwise the full yield goes to the central's hot halo. The escaping metals are formed in their own statement, as SAGE26 forms `metals_leaving_disk`. The yield is added on every call, also with no new stars, as SAGE26 does (a zero yield turns a `-0.0` metal reservoir into `+0.0`). On R0 `FracZleaveDisk` is 0. The starburst channel applies its own yield inside the burst kernel (`shared/sage_starburst_physics.h`); this module only handles the quiescent disk-SF yield. The chained oracle test of `sage26_calculate_star_formation` asserts it bit for bit against SAGE26.

## References

- SAGE26: `src/model_starformation_and_feedback.c:1092-1109`
- Croton et al. (2006, 2016)
