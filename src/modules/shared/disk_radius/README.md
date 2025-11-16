# Disk Radius Calculation Utility

**Location**: `src/modules/shared/disk_radius/`

## Overview

This shared utility provides galaxy disk scale radius calculations based on the Mo, Mao & White (1998) model. The disk radius is computed from dark matter halo properties using the relationship between disk size, halo spin parameter, and virial radius.

## Physics Background

The Mo, Mao & White (1998) model relates disk size to the angular momentum (spin) of the host dark matter halo:

```
Rd = (λ / √2) * Rvir
```

Where:
- `Rd` is the disk scale radius
- `λ` is the dimensionless spin parameter (Bullock definition)
- `Rvir` is the virial radius of the halo

The spin parameter is defined as:
```
λ = |J| / (√2 * Mvir * Rvir * Vvir)
```

Simplified to:
```
λ = |J| / (√2 * Vvir * Rvir)
```

Where `|J|` is the magnitude of the angular momentum vector.

## Functions Provided

### `mimic_get_spin_magnitude()`
Calculates the magnitude of the 3D spin vector.

**Parameters**:
- `spin_x`, `spin_y`, `spin_z` - Components of halo spin vector

**Returns**: Magnitude |J| = √(Jx² + Jy² + Jz²)

### `mimic_get_spin_parameter()`
Calculates the Bullock-style dimensionless spin parameter λ.

**Parameters**:
- `spin_magnitude` - Magnitude of spin vector |J|
- `vvir` - Virial velocity (km/s)
- `rvir` - Virial radius (Mpc/h)

**Returns**: Dimensionless spin parameter λ (typically 0.01 - 0.1)

### `mimic_get_disk_radius()`
Calculates the galaxy disk scale radius using the Mo, Mao & White (1998) model.

**Parameters**:
- `spin_x`, `spin_y`, `spin_z` - Components of halo spin vector
- `vvir` - Virial velocity (km/s)
- `rvir` - Virial radius (Mpc/h)

**Returns**: Disk scale radius (Mpc/h)

## Usage Example

```c
// In your module's .c file
#include "../shared/disk_radius/disk_radius.h"

static int my_module_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
    for (int i = 0; i < ngal; i++) {
        if (halos[i].Type != 0) continue;  // Only centrals

        // Calculate disk radius from halo properties
        float disk_radius = mimic_get_disk_radius(
            halos[i].Spin[0], halos[i].Spin[1], halos[i].Spin[2],
            halos[i].Vvir,
            halos[i].Rvir
        );

        // Use in galaxy property
        halos[i].galaxy->DiskScaleRadius = disk_radius;
    }
    return 0;
}
```

## Edge Cases Handled

1. **Zero or negative virial properties**: Returns fallback value of 0.1 * Rvir
2. **Zero spin**: Calculates properly (results in small lambda and disk radius)
3. **Numerical stability**: Uses `EPSILON_SMALL` for validity checks

## Physical Interpretation

- **Typical spin parameters**: λ ~ 0.03 - 0.05 for ΛCDM halos
- **Typical disk radius**: Rd ~ 0.02 - 0.05 * Rvir for MW-like galaxies
- **Scaling**: Disk radius ∝ spin parameter ∝ virial radius

## References

- **Mo, H. J., Mao, S., & White, S. D. M. (1998)**, "Galaxy formation in a hierarchical universe - I. Why discs survive mergers", MNRAS, 295, 319-336
- **Bullock, J. S., et al. (2001)**, "A Universal Angular Momentum Profile for Galactic Halos", ApJ, 555, 240

## Testing

Unit tests are provided in `test_unit_disk_radius.c` covering:
- Spin magnitude calculation
- Spin parameter calculation
- Disk radius calculation with valid inputs
- Edge cases (zero/negative values)
- Physical plausibility checks
- Consistency with Mo98 formulation

Run tests:
```bash
make test-unit
```

## Implementation Notes

- **Header-only**: All functions are `static inline` for simplicity and performance
- **Namespace**: Functions prefixed with `mimic_` to avoid collisions
- **Dependencies**: Only requires `constants.h` for `EPSILON_SMALL`
- **Float precision**: Uses `float` to match galaxy property types
