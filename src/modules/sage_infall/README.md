# SAGE Infall Module

The `sage_infall` module implements cosmological gas infall from the SAGE model. Central galaxies accrete baryonic gas proportional to their dark matter halo growth, with the local baryon fraction modified by reionization suppression (set by `sage_reionization` module). The module also consolidates satellite ejected gas and intracluster stars to centrals, preserving metallicity throughout all transfers.

**Physics**: `InfallingGas = HaloBaryonFraction × Mvir - total_baryons`

**Execution Order**: This module requires `sage_reionization` to run first to set the `HaloBaryonFraction` property for each halo.

## Parameters

This module has no parameters. It uses the `HaloBaryonFraction` property set by `sage_reionization`.

## Configuration Example

```yaml
modules:
  enabled:
    - sage_reionization  # MUST run first - sets HaloBaryonFraction
    - sage_infall        # Uses HaloBaryonFraction
  parameters:
    GlobalBaryonFraction: 0.17  # Used by sage_reionization
```

**References**: Croton et al. (2006, 2016)
