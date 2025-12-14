# SAGE Reionization Module

**Module**: `sage_reionization`
**Version**: 1.0.0
**Phase**: pre_timestep
**Loop Mode**: PROCESSING_MODE_FULL_HALO

## Overview

Implements reionization suppression of gas accretion onto low-mass halos from the SAGE (Semi-Analytic Galaxy Evolution) model. After cosmic reionization, the increased temperature and Jeans mass of ionized gas suppresses accretion onto halos below a characteristic mass.

## Physics

### Gnedin (2000) Model

The module calculates halo-specific baryon fractions modified by reionization suppression:

```
HaloBaryonFraction = GlobalBaryonFraction × f_reion(Mvir, z)
```

Where:
- `GlobalBaryonFraction`: Cosmic baryon fraction (Ωb/Ωm ≈ 0.17)
- `f_reion(Mvir, z)`: Suppression factor (0 = complete suppression, 1 = no suppression)
- `Mvir`: Virial mass of halo (in 10^10 Msun/h)
- `z`: Redshift

### Suppression Factor

The suppression factor depends on the ratio of halo mass to characteristic mass:

```
f_reion = [1 + 0.26 × (Mchar/Mvir)]^-3
```

Where `Mchar` is the maximum of:
1. **Filtering mass**: Mass scale below which baryons are prevented from collapsing
2. **Characteristic mass**: Mass corresponding to virial temperature Tvir = 10^4 K

### Three Reionization Regimes

Based on scale factor (a = 1/(1+z)):

1. **Before UV background** (a ≤ a0, z ≥ z0 = 8):
   - Partial suppression begins
   - UV photons start ionizing intergalactic medium

2. **During reionization** (a0 < a < ar, 8 > z > 7):
   - Increasing suppression as ionized fraction grows
   - Complex evolution of filtering mass

3. **After reionization** (a ≥ ar, z ≤ zr = 7):
   - Full suppression effect established
   - Filtering mass evolves with expansion

### Physical Interpretation

- **Low-mass halos** (Mvir << Mchar): Strong suppression, f_reion → 0
- **High-mass halos** (Mvir >> Mchar): Minimal suppression, f_reion → 1
- **Transition scale** (Mvir ~ Mchar): Gradual transition in suppression

The characteristic mass Mchar increases with redshift, meaning progressively more massive halos can efficiently accrete gas at earlier times.

## Key Properties

**Write**:
- `HaloBaryonFraction`: Modified cosmic baryon fraction for each halo (0.0 to GlobalBaryonFraction)

**Read**:
- `Mvir`: Virial mass from halo structure (not GalaxyData)
- `Type`: Halo type for diagnostic logging

## Parameters

- `GlobalBaryonFraction`: Cosmic baryon fraction (0.0-1.0, exclusive)

## Implementation Notes

### Model Parameters

The Gnedin (2000) model uses hardcoded parameters calibrated to match numerical simulations:

- `z0 = 8.0`: Redshift when UV background turns on
- `zr = 7.0`: Redshift of full reionization
- `alpha = 6.0`: Suppression strength exponent
- `Tvir = 10^4 K`: Virial temperature threshold

These values are specific to the Gnedin model and should not be modified without careful justification.

### Execution Order

This module **must** run in `pre_timestep` phase **before** modules that read `HaloBaryonFraction`:
- `sage_calculate_infall`: Uses HaloBaryonFraction to calculate infalling gas
- `sage_satellite_stripping`: Uses HaloBaryonFraction for stripping calculation

If `sage_reionization` is not enabled, downstream modules fall back to `HaloBaryonFraction = GlobalBaryonFraction` (no suppression).

### Zero Mass Handling

Halos with `Mvir <= 0` (orphans) are assigned `HaloBaryonFraction = 0.0` since they have no virial mass to support baryon accretion.

### Cosmology Dependence

The suppression calculation requires cosmological parameters:
- `Omega`: Matter density parameter
- `OmegaLambda`: Dark energy density parameter
- `Hubble_h`: Reduced Hubble constant (H0 / 100 km/s/Mpc)

These are automatically extracted from the module context.

## References

- **Gnedin (2000)**: "Effect of Reionization on Structure Formation in the Universe"
  - Original reionization suppression model
  - Numerical simulations showing filtering mass evolution

- **Kravtsov et al. (2004)**: "The Dark Side of the Halo Occupation Distribution"
  - Appendix B: Analytical fitting formulas for f(a) evolution
  - Calibration of suppression parameters

- **Croton et al. (2016)**: "Semi-Analytic Galaxy Evolution (SAGE): Model Calibration and Basic Results"
  - SAGE implementation of reionization suppression
  - Integration with full galaxy formation model

- Based on SAGE `model_reionization.c` (https://github.com/darrencroton/sage)

## Dependencies

**Requires**:
- No other modules (sets baseline property)

**Used by**:
- `sage_calculate_infall`: Reads HaloBaryonFraction for cosmological infall calculation
- `sage_satellite_stripping`: Reads HaloBaryonFraction for gas stripping calculation

## Testing

- Unit tests: `tests/test_unit_sage_reionization.c` (software quality)
- Integration tests: `tests/test_integration_sage_reionization.py` (end-to-end)
- Scientific validation: `tests/test_scientific_sage_reionization.py` (deferred until full pipeline validated)
