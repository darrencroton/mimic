/**
 * @file disk_radius.h
 * @brief Disk scale radius calculation utilities for galaxy physics modules
 *
 * This shared utility provides disk scale radius calculations based on the
 * Mo, Mao & White (1998) model, which relates disk size to halo spin and
 * virial properties. Used by modules dealing with disk physics.
 *
 * Reference:
 * - Mo, H. J., Mao, S., & White, S. D. M. (1998), MNRAS, 295, 319
 * - Bullock et al. (2001) - Spin parameter definition
 *
 * @note This file is part of the shared utilities system in src/modules/shared/.
 *       Modules include it using relative paths: #include "../shared/disk_radius/disk_radius.h"
 */

#ifndef MIMIC_SHARED_DISK_RADIUS_H
#define MIMIC_SHARED_DISK_RADIUS_H

#include <math.h>
#include "constants.h"

/**
 * @brief Calculate spin parameter magnitude from 3D spin vector
 *
 * Computes |J| = sqrt(Jx^2 + Jy^2 + Jz^2) where J is the angular momentum
 * (spin) vector of the dark matter halo.
 *
 * @param spin_x  X-component of halo spin vector
 * @param spin_y  Y-component of halo spin vector
 * @param spin_z  Z-component of halo spin vector
 * @return        Magnitude of spin vector
 *
 * Example usage:
 * @code
 *   float spin_mag = mimic_get_spin_magnitude(halo.Spin[0], halo.Spin[1], halo.Spin[2]);
 * @endcode
 */
static inline float mimic_get_spin_magnitude(float spin_x, float spin_y, float spin_z)
{
    return sqrtf(spin_x * spin_x + spin_y * spin_y + spin_z * spin_z);
}

/**
 * @brief Calculate Bullock-style spin parameter λ
 *
 * Computes the dimensionless spin parameter using the Bullock et al. (2001)
 * definition:
 *   λ = |J| / (√2 * Mvir * Rvir * Vvir)
 *
 * Where:
 *   - |J| is the magnitude of the angular momentum vector
 *   - Mvir is the virial mass (not explicitly used, embedded in Vvir)
 *   - Rvir is the virial radius
 *   - Vvir is the virial velocity
 *
 * Simplified form used here:
 *   λ = |J| / (√2 * Vvir * Rvir)
 *
 * @param spin_magnitude  Magnitude of halo spin vector |J|
 * @param vvir            Virial velocity (km/s)
 * @param rvir            Virial radius (Mpc/h)
 * @return                Dimensionless spin parameter λ (typically 0.01 - 0.1)
 *
 * @note Returns 0.0 if virial velocity or radius are non-positive
 *
 * Example usage:
 * @code
 *   float spin_mag = mimic_get_spin_magnitude(halo.Spin[0], halo.Spin[1], halo.Spin[2]);
 *   float lambda = mimic_get_spin_parameter(spin_mag, vvir, rvir);
 * @endcode
 */
static inline float mimic_get_spin_parameter(float spin_magnitude, float vvir, float rvir)
{
    if (vvir <= EPSILON_SMALL || rvir <= EPSILON_SMALL)
        return 0.0f;

    // Bullock-style lambda: |J| / (sqrt(2) * Vvir * Rvir)
    return spin_magnitude / (1.414213562f * vvir * rvir);
}

/**
 * @brief Calculate galaxy disk scale radius
 *
 * Computes the disk scale radius based on the Mo, Mao & White (1998) model.
 * The disk radius is proportional to the spin parameter and virial radius:
 *
 *   Rd = (λ / √2) * Rvir
 *
 * Where λ is the Bullock-style spin parameter.
 *
 * If virial properties are invalid (≤0), returns a default of 0.1 * Rvir
 * as a fallback estimate.
 *
 * @param spin_x  X-component of halo spin vector
 * @param spin_y  Y-component of halo spin vector
 * @param spin_z  Z-component of halo spin vector
 * @param vvir    Virial velocity (km/s)
 * @param rvir    Virial radius (Mpc/h)
 * @return        Disk scale radius (Mpc/h)
 *
 * @note Uses EPSILON_SMALL for numerical stability in validity checks
 *
 * Reference: Mo, Mao & White (1998) eq. 12
 *
 * Example usage in a module:
 * @code
 *   float disk_radius = mimic_get_disk_radius(
 *       halos[i].Spin[0], halos[i].Spin[1], halos[i].Spin[2],
 *       halos[i].Vvir, halos[i].Rvir
 *   );
 *   galaxy->DiskScaleRadius = disk_radius;
 * @endcode
 */
static inline float mimic_get_disk_radius(float spin_x, float spin_y, float spin_z,
                                          float vvir, float rvir)
{
    // Validate virial properties
    if (vvir > EPSILON_SMALL && rvir > EPSILON_SMALL) {
        // Calculate spin magnitude
        float spin_mag = mimic_get_spin_magnitude(spin_x, spin_y, spin_z);

        // Calculate Bullock-style spin parameter
        float lambda = mimic_get_spin_parameter(spin_mag, vvir, rvir);

        // Mo, Mao & White (1998) eq. 12: Rd = (λ / √2) * Rvir
        return (lambda / 1.414213562f) * rvir;
    } else {
        // Fallback: use 10% of virial radius as rough estimate
        return 0.1f * rvir;
    }
}

#endif /* MIMIC_SHARED_DISK_RADIUS_H */
