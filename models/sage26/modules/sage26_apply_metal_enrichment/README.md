# `sage26_apply_metal_enrichment`

Applies the instantaneous-recycling metal yield of the stars formed in the disk this substep, then consumes (zeroes) `NewStellarMass`. It runs after the disk-instability chain, as SAGE26 orders it, so an instability burst sees the cold gas's metallicity before enrichment.

## Physics

A fraction `f_Z = FracZleaveDisk * exp(-Mvir_central / 30)` of the new metals escapes the disk (Krumholz & Dekel 2011, Eq. 22), with `Mvir_central` the FoF central's virial mass in 1e10 Msun/h. If `ColdGas > 1e-8`, `MetalsColdGas` gains `Yield * (1 - f_Z) * NewStellarMass` and the escaping `Yield * f_Z * NewStellarMass` goes to the FoF central; otherwise the whole yield goes to the central. With `CGMrecipeOn 1` the central's destination is `MetalsCGMgas` when its `Regime` is 0 and `MetalsHotGas` otherwise; with `CGMrecipeOn 0` it is always `MetalsHotGas` (`shared/sage_cgm_routing.h`). Starbursts apply their own yield inside the burst transaction (`shared/sage_starburst_physics.h`); this module handles only the quiescent disk yield (Croton et al. 2006, 2016).

## Processing Contract

- Supported mode: `process_by_galaxy`
- Expected phase: `galaxy_physics`, after `sage26_apply_star_formation_supernova` and the disk-instability chain (`sage26_disk_instability`, `sage26_quasar_mode`, `sage26_starburst_feedback`)
- Receives one galaxy at a time and uses the FoF central as the destination of the escaping metals
- A feedback-free-burst row (`FFBRegime` 1) is skipped and nothing on it is written, `NewStellarMass` included: `sage26_ffb_star_formation` applies that row's yield
- With no FoF central galaxy in the workspace the module writes nothing

## Ordering

**Enforced at init (fails with ERROR if violated):**

1. When `sage26_apply_star_formation_supernova` is configured, it must precede this module as `process_by_galaxy` in the same substep phase — the yield is keyed to the stars that step commits.
2. When `sage26_starburst_feedback` is configured as `process_by_galaxy` in a substep phase, it must precede this module in that phase — the burst must see the pre-enrichment metallicity.

**Not refused:** `sage26_apply_star_formation_supernova` absent (nothing then writes `NewStellarMass`, so no metals are added); the positions of `sage26_disk_instability` and `sage26_quasar_mode`.

## Properties

- Reads: `FFBRegime`, `Type`, `ColdGas`, `MetalsColdGas`, `NewStellarMass`, and the FoF central's `Mvir` and `Regime`
- Writes: `MetalsColdGas`, the FoF central's `MetalsHotGas` or `MetalsCGMgas`, `NewStellarMass` (zeroed after use)

## Parameters

- `Yield`: metal mass produced per unit stellar mass formed; required, within [0, 1]; the run files hold 0.025.
- `FracZleaveDisk`: the maximum fraction of new metals ejected straight to the central's halo; required, within [0, 1]; the run files hold 0.0, so every new metal stays in the cold gas while it exceeds the threshold.
- `CGMrecipeOn`: required, 0 or 1; 1 (the run files' value) routes the escaping metals by the central's `Regime`, 0 always to `MetalsHotGas`. Any other value, or a missing key, fails initialisation naming the parameter.

## Tests

- `_tests/test_unit_sage26_apply_metal_enrichment.c`: the yield split either side of the cold-gas threshold, the zero-yield event, missing-galaxy and missing-central cases, the `CGMrecipeOn` destinations with `NewStellarMass` consumed, and the skipped FFB row.
- The `CGMrecipeOn` refusals and the required order after `sage26_apply_star_formation_supernova` and `sage26_starburst_feedback` are covered at package level by `../_tests/test_unit_sage26_parameter_refusals.c` and `../_tests/test_unit_sage_dependency_contracts.c`.
