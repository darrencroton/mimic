# SAGE Add Infall Module

**Module**: `sage_add_infall`
**Version**: 1.0.0
**Phase**: phase_1
**Loop Mode**: LOOP_MODE_ONCE

## Overview

Distributes infalling gas to the hot gas reservoir with metallicity tracking. Works in tandem with `sage_infall` module which calculates the total `InfallingGas` per timestep in the pre_timestep phase.

This module runs during phase_1 and adds `InfallingGas / num_substeps` to the hot gas reservoir each substep, enabling proper time sub-stepping for numerical stability.

## Physics

### Positive Infall (Gas Accretion)

For positive infall (gas accretion onto halo):

```
HotGas += InfallingGas / num_substeps
```

Infalling gas is assumed primordial (zero metallicity) - no metals added.

### Negative Infall (Mass Loss)

For negative infall (mass loss from halo), mass is removed in priority order:

1. **Ejected reservoir first**:
   ```
   EjectedMass += InfallingGas / num_substeps  (negative value)
   MetalsEjectedMass -= removed_mass × Z_ejected
   ```

2. **Hot gas if ejected depleted**:
   ```
   HotGas += remaining_negative_infall
   MetalsHotGas -= removed_mass × Z_hot
   ```

Metallicity is preserved during removal using the `mimic_get_metallicity()` helper.

### Numerical Safety

All mass components are floored at zero to prevent negative masses:
- `EjectedMass >= 0`
- `MetalsEjectedMass >= 0`
- `HotGas >= 0`
- `MetalsHotGas >= 0`

## Key Properties

**Read**:
- `InfallingGas`: Total infall for timestep (set by `sage_infall` module)

**Write**:
- `HotGas`: Hot gas reservoir (receives distributed infall)
- `MetalsHotGas`: Metals in hot gas
- `EjectedMass`: Ejected gas reservoir (for negative infall)
- `MetalsEjectedMass`: Metals in ejected gas

## Parameters

None - this module has no configurable parameters.

## Implementation Notes

### Two-Module Design

Infall is split into two modules for proper time sub-stepping:

1. **sage_infall** (pre_timestep, LOOP_MODE_ONCE):
   - Calculates total `InfallingGas` for the timestep
   - Runs once per timestep before substeps
   - Consolidates satellite reservoirs to central

2. **sage_add_infall** (phase_1, LOOP_MODE_ONCE):
   - Distributes `InfallingGas / num_substeps` to `HotGas` each substep
   - Preserves metallicity during transfer
   - Handles negative infall (mass loss)

This design enables numerical stability through time sub-stepping while maintaining correct physics.

### Central Galaxy Only

The module processes only the central galaxy (Type 0) in each FOF group, since `InfallingGas` is calculated for the central galaxy by `sage_infall`.

### Metallicity Handling

- **Positive infall**: Primordial gas (zero metallicity)
- **Negative infall**: Metallicity preserved using reservoir's current metallicity

## References

- Croton et al. (2006): "The many lives of active galactic nuclei: cooling flows, black holes and the luminosities and colours of galaxies"
- Croton et al. (2016): "Semi-Analytic Galaxy Evolution (SAGE): Model Calibration and Basic Results"
- Based on SAGE `model_infall.c` `add_infall_to_hot()` (https://github.com/darrencroton/sage)

## Dependencies

**Requires**:
- `sage_infall`: Must run in pre_timestep phase to calculate `InfallingGas`

**Works with**:
- All modules that modify baryonic components (affects infall calculation)

## Testing

- Unit tests: `tests/test_unit_sage_add_infall.c` (software quality)
- Integration tests: `tests/test_integration_sage_add_infall.py` (end-to-end)
- Scientific validation: `tests/test_scientific_sage_add_infall.py` (physics accuracy)
