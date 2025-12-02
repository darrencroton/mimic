# SAGE Satellite Stripping Module

The `sage_satellite_stripping` module implements environmental gas removal from satellites via ram pressure and tidal stripping. Satellites lose hot gas when their baryon content exceeds expectations based on their local baryon fraction (set by `sage_reionization` with reionization suppression). Stripped gas transfers to the central galaxy's hot reservoir with metallicity preserved. Only processes Type 1 satellites; Type 2 satellites and centrals unaffected.

**Physics**: `strippedGas = -(HaloBaryonFraction × Mvir - total_baryons) / STEPS`

**Execution Order**: This module requires `sage_reionization` to run first to set the `HaloBaryonFraction` property for each halo.

## Parameters

This module has no parameters. It uses the `HaloBaryonFraction` property set by `sage_reionization`.

**Configuration Example**:
```yaml
modules:
  enabled:
    - sage_reionization        # MUST run first - sets HaloBaryonFraction
    - sage_infall
    - sage_satellite_stripping # Uses HaloBaryonFraction
  parameters:
    GlobalBaryonFraction: 0.17  # Used by sage_reionization
```

**References**: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2006, 2016)
