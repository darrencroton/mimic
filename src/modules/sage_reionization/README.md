# SAGE Reionization Module

Calculates halo-specific baryon fractions modified by reionization suppression following the Gnedin (2000) model. After cosmic reionization, gas accretion onto low-mass halos is suppressed due to increased gas temperature and Jeans mass.

**Physics**: `HaloBaryonFraction = GlobalBaryonFraction × f_reion(Mvir, z)`

The suppression factor f_reion depends on the ratio between halo mass and a characteristic mass (maximum of filtering mass and mass corresponding to virial temperature of 10^4 K). Three regimes based on scale factor:
1. Before UV background turns on (a ≤ a0): Partial suppression
2. During partial reionization (a0 < a < ar): Increasing suppression
3. After full reionization (a ≥ ar): Full suppression effect

**Execution Order**: This module **MUST** run before `sage_infall` and `sage_satellite_stripping` as they depend on the `HaloBaryonFraction` property.

**Configuration Example**:
```yaml
modules:
  enabled:
    - sage_reionization          # FIRST - sets HaloBaryonFraction
    - sage_infall                # Uses HaloBaryonFraction
    - sage_satellite_stripping   # Uses HaloBaryonFraction
  parameters:
    GlobalBaryonFraction: 0.17   # Planck 2018 cosmic baryon fraction
```

**Model Parameters**:
- **z0 = 8.0**: Redshift when UV background turns on
- **zr = 7.0**: Redshift of full reionization
- **alpha = 6.0**: Suppression strength (Gnedin 2000)
- **Tvir = 10^4 K**: Virial temperature threshold

These parameters are **hardcoded** in the module. To use a different reionization model, create a new module with different parameters.

**References**:
- Gnedin (2000) - Reionization model
- Kravtsov et al. (2004) - Filtering mass formulas
- Bryan & Norman (1998) - Critical overdensity
- Croton et al. (2016) - SAGE model description
