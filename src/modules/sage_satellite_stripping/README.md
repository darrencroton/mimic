# SAGE Satellite Stripping Module

The `sage_satellite_stripping` module implements environmental gas removal from satellites via ram pressure and tidal stripping. Satellites lose hot gas when their baryon content exceeds cosmological expectations (adjusted for reionization suppression using Gnedin 2000 model). Stripped gas transfers to the central galaxy's hot reservoir with metallicity preserved. Uses shared `reionization.h` utility for suppression factors, enabling easy model swapping. Only processes Type 1 satellites; Type 2 satellites and centrals unaffected.

**Configuration Example**:
```
EnabledModules  sage_infall,sage_satellite_stripping
SageSatelliteStripping_BaryonFrac  0.17
```

**Additional Comments**:

**References**: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2006, 2016)
