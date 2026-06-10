# sage_apply_metal_enrichment

Applies the instantaneous-recycling metal yield for stars formed in the disk this substep, exactly where original SAGE applies it.

## Physics

For the (renormalised) disk star formation `stars = NewStellarMass` committed earlier in the substep by `sage_apply_star_formation_supernova`:

- If `ColdGas > 1e-8`: `MetalsColdGas += Yield * (1 - f_Z) * stars` and `central MetalsHotGas += Yield * f_Z * stars`, where `f_Z = FracZleaveDisk * exp(-Mvir_central / 30)` (Krumholz & Dekel 2011 Eq. 22).
- Otherwise the full yield goes to the central's hot metals.

The module then consumes (zeroes) `NewStellarMass`.

## Ordering (SAGE parity)

In SAGE's `starformation_and_feedback()` the yield is added **after** `check_disk_instability()`, so the disk-instability starburst must see the cold gas metallicity *without* this substep's disk-SF enrichment. This module therefore runs **after** `sage_disk_instability`, `sage_quasar_mode`, and `sage_starburst_feedback` in the `galaxy_physics` phase. Both orderings are enforced at init:

1. `sage_apply_star_formation_supernova` must precede this module (the yield is keyed to the stellar mass it commits).
2. `sage_starburst_feedback` (by-galaxy channel) must precede this module.

The merger/starburst channel applies its own yield inside the burst kernel (`shared/sage_starburst_physics.h`), exactly as SAGE's `collisional_starburst_recipe()` does — this module only handles the quiescent disk-SF yield.

## Parameters

| Parameter | Description |
|-----------|-------------|
| `Yield` | Metal yield per unit stellar mass formed |
| `FracZleaveDisk` | Fraction of newly produced metals ejected directly to the hot halo |

## References

- SAGE: `model_starformation_and_feedback.c` lines 93–102
- Croton et al. (2006, 2016)
