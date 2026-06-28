# `sage_apply_metal_enrichment`

Applies the instantaneous-recycling metal yield for stars formed in the disk this substep, then consumes (zeroes) `NewStellarMass`.

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage_apply_star_formation_supernova` and after the disk-instability chain (`sage_disk_instability`, `sage_quasar_mode`, `sage_starburst_feedback`)
- Receives one galaxy at a time and uses the FoF central for the metal ejection destination

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. `sage_apply_star_formation_supernova` must precede this module in the same substep phase — the yield is keyed to the stellar mass committed by that apply step.
2. `sage_starburst_feedback` (by-galaxy channel) must precede this module — SAGE adds the disk-SF yield after `check_disk_instability()`, so the burst must see pre-enrichment metallicity.

## Properties

- Reads: `Type`, `Mvir` (FoF central), `ColdGas`, `MetalsColdGas`, `NewStellarMass`
- Writes: `MetalsColdGas`, `MetalsHotGas` (FoF central), `NewStellarMass` (zeroed after consuming)

## Parameters

| Parameter | Description |
|-----------|-------------|
| `Yield` | Metal yield per unit stellar mass formed |
| `FracZleaveDisk` | Fraction of newly produced metals ejected directly to the hot halo |

## Notes

The metal split uses `f_Z = FracZleaveDisk * exp(-Mvir_central / 30)` (Krumholz & Dekel 2011 Eq. 22): if `ColdGas > 1e-8`, `MetalsColdGas += Yield * (1 - f_Z) * stars` and the central's `MetalsHotGas += Yield * f_Z * stars`; otherwise the full yield goes to the central's hot halo. The starburst channel applies its own yield inside the burst kernel (`shared/sage_starburst_physics.h`); this module only handles the quiescent disk-SF yield.

## References

- SAGE: `model_starformation_and_feedback.c` lines 93–102
- Croton et al. (2006, 2016)
