# SAGE Calculate Infall Module

**Module**: `sage_calculate_infall`
**Version**: 1.0.0
**Phase**: pre_timestep
**Loop Mode**: PROCESSING_MODE_FULL_HALO

## Overview

Implements cosmological gas infall onto central galaxies from the SAGE (Semi-Analytic Galaxy Evolution) model. Calculates the net infalling gas mass based on halo baryon content and consolidates satellite ejected gas and intracluster stars to the central galaxy.

## Physics

### Infall Calculation

The infalling gas mass is calculated as the difference between expected baryon content and current baryonic mass:

```
InfallingGas = HaloBaryonFraction × Mvir - (M_stellar + M_cold + M_hot + M_ejected + M_BH + M_ICS)
```

Where:
- `HaloBaryonFraction`: Local baryon fraction (cosmic value modified by reionization suppression, set by `sage_reionization` module)
- `Mvir`: Virial mass of the halo
- All mass components summed across entire FOF group

### Satellite Consolidation

The module consolidates satellite reservoir components to the central galaxy:
- Ejected gas (`EjectedMass`, `MetalsEjectedMass`) → Central
- Intracluster stars (`ICS`, `MetalsICS`) → Central

After consolidation, satellites have these components zeroed out, preventing double-counting in the infall calculation.

### Mass Conservation

**Note**: Full FOF baryon fraction conservation only achieved when combined with:
- `sage_satellite_stripping`: Removes hot gas from satellites
- Merger modules: Handle Type 2 (orphan) galaxy evolution

## Key Properties

**Read**:
- `HaloBaryonFraction`: Modified by reionization (initialized to `GlobalBaryonFraction` if -1.0)
- `StellarMass`, `BlackHoleMass`, `ColdGas`, `HotGas`, `EjectedMass`, `ICS`
- `MetalsEjectedMass`, `MetalsICS`

**Write**:
- `InfallingGas`: Stored for distribution over substeps by `sage_add_infall` module
- `EjectedMass`, `MetalsEjectedMass`: Consolidated to central
- `ICS`, `MetalsICS`: Consolidated to central
- `HaloBaryonFraction`: Initialized if needed

## Parameters

- `GlobalBaryonFraction`: Cosmic baryon fraction (0.0-1.0, exclusive)

## Implementation Notes

### Two-Module Design

Infall is split into two modules:
1. **sage_calculate_infall** (pre_timestep, PROCESSING_MODE_FULL_HALO):
   - Calculates total `InfallingGas` for the timestep
   - Runs once per timestep before substeps
   - Consolidates satellite reservoirs to central

2. **sage_add_infall** (phase_1, PROCESSING_MODE_FULL_HALO):
   - Distributes `InfallingGas / num_substeps` to `HotGas` each substep
   - Preserves metallicity during transfer
   - Handles negative infall (mass loss) by removing from ejected reservoir first

This design enables proper time sub-stepping for numerical stability.

### Validation

The module includes a `validate_mass_metals()` helper function that ensures physical constraints:
- `mass >= 0`, `metals >= 0`, `metals <= mass`
- Applied after consolidation to central galaxy

### Metallicity

Infalling gas is assumed primordial (zero metallicity) - metals are inherited from existing reservoir components through consolidation.

## References

- Croton et al. (2006): "The many lives of active galactic nuclei: cooling flows, black holes and the luminosities and colours of galaxies"
- Croton et al. (2016): "Semi-Analytic Galaxy Evolution (SAGE): Model Calibration and Basic Results"
- Based on SAGE `model_infall.c` (https://github.com/darrencroton/sage)

## Dependencies

**Requires**:
- `sage_reionization`: Sets `HaloBaryonFraction` based on reionization suppression
- `sage_add_infall`: Distributes infalling gas over substeps

**Works with**:
- `sage_satellite_stripping`: Proper satellite treatment for mass conservation
- Merger modules: Handle orphan galaxy reservoirs

## Testing

- Unit tests: `tests/test_unit_sage_calculate_infall.c` (software quality)
- Integration tests: `tests/test_integration_sage_calculate_infall.py` (end-to-end)
- Scientific validation: `tests/test_scientific_sage_calculate_infall.py` (physics accuracy)
