# SAGE Satellite Stripping Module

**Module**: `sage_satellite_stripping`
**Version**: 1.0.0
**Phase**: phase_1
**Loop Mode**: PROCESSING_MODE_FULL_HALO

## Overview

Implements environmental gas stripping from satellite galaxies via ram pressure and tidal effects. Satellites lose hot gas when their baryon content exceeds the expected amount based on `HaloBaryonFraction`. Stripped gas transfers to the central galaxy's hot reservoir with metallicity preserved.

## Physics

### Stripping Calculation

Hot gas is stripped from satellites when baryon content exceeds expectations:

```
strippedGas = -(HaloBaryonFraction × Mvir - total_baryons) / SubSteps
```

Where:
- `HaloBaryonFraction`: Local baryon fraction (set by `sage_reionization` module)
- `total_baryons = M_stellar + M_cold + M_hot + M_ejected + M_BH + M_ICS`
- Stripping distributed over `SubSteps` for numerical stability

If `strippedGas > 0`, gas and metals transfer from satellite hot reservoir to central hot reservoir.

### Mass Flow

```
Satellite HotGas → Central HotGas (with metals)
```

Metallicity is preserved during the transfer using the satellite's hot gas metallicity.

## Key Properties

**Read**:
- `HaloBaryonFraction`: Baryon fraction (fallback to `GlobalBaryonFraction` if unset)
- `Mvir`, `StellarMass`, `ColdGas`, `HotGas`, `EjectedMass`, `BlackHoleMass`, `ICS`
- `MetalsHotGas`

**Write**:
- `HotGas`: Reduced for satellites, increased for central
- `MetalsHotGas`: Transferred proportionally with gas

## Parameters

- `GlobalBaryonFraction`: Cosmic baryon fraction (0.0-1.0, exclusive), used as fallback

## Implementation Notes

### Loop Mode: ONCE

Module must process entire FOF group together:
1. Find central galaxy (Type 0)
2. Loop through all satellites
3. Transfer stripped gas to central

Cannot work in galaxy-major mode where each galaxy is processed individually.

### Fallback Logic

If `HaloBaryonFraction <= 0` (unset), uses `GlobalBaryonFraction` as fallback. This handles edge cases where `sage_reionization` hasn't set the property.

### Limiting Transfers

Stripped amounts are limited to available hot gas and metals to prevent negative values.

## References

- Gnedin (2000): "Effect of Reionization on Structure Formation in the Universe"
- Kravtsov et al. (2004): "The Dark Side of the Halo Occupation Distribution"
- Croton et al. (2006, 2016): SAGE model papers

## Dependencies

**Requires**:
- `sage_reionization`: Sets `HaloBaryonFraction` based on reionization suppression

**Works with**:
- `sage_calculate_infall`: Provides hot gas reservoir that gets stripped
- Other phase_1 modules: Cooling, star formation, etc.

## Testing

- Unit tests: `tests/test_unit_sage_satellite_stripping.c` (5 tests)
- Integration tests: `tests/test_integration_sage_satellite_stripping.py` (5 tests)
- Scientific validation: Deferred to Phase 4.3+ (requires downstream modules)
