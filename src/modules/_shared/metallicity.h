/**
 * @file metallicity.h
 * @brief Common metallicity calculation utilities for physics modules
 *
 * This shared utility provides robust metallicity calculations used by
 * multiple physics modules (sage_calculate_infall, sage_cooling, etc.).
 *
 * @note This file is part of the shared utilities system in src/modules/shared/.
 *       Modules include it using relative paths: #include "../shared/metallicity.h"
 */

#ifndef MIMIC_SHARED_METALLICITY_H
#define MIMIC_SHARED_METALLICITY_H

#include "constants.h"

/**
 * @brief Calculate metallicity (metal mass fraction) with safety checks
 *
 * Computes the metallicity Z = M_metals / M_gas, handling edge cases
 * where gas mass is zero or negligible to prevent division by zero.
 * Enforces physical limit: metallicity ≤ 1.0 (100% metal mass fraction).
 *
 * @param gas     Total gas mass (e.g., HotGas, ColdGas, EjectedMass)
 * @param metals  Metal mass in the gas (e.g., MetalsHotGas)
 * @return        Metallicity (0.0 to 1.0), or 0.0 if gas <= EPSILON_SMALL
 *
 * @note Uses EPSILON_SMALL for numerical stability rather than exact zero check
 * @note Caps metallicity at 1.0 to prevent unphysical values (Bug Fix #3)
 *
 * Example usage:
 * @code
 *   float Z = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);
 *   float stripped_metals = stripped_gas * Z;
 * @endcode
 */
static inline float mimic_get_metallicity(float gas, float metals)
{
    if (gas <= EPSILON_SMALL)
        return 0.0f;

    float Z = metals / gas;

    /* Cap at 1.0 to enforce physical limit (100% metal mass fraction) */
    return (Z < 1.0f) ? Z : 1.0f;
}

#endif /* MIMIC_SHARED_METALLICITY_H */
