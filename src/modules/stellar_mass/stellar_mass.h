/**
 * @file stellar_mass.h
 * @brief Stellar mass module interface
 *
 * This module implements a simple stellar mass calculation for galaxies.
 * This is a minimal proof-of-concept demonstrating the module system.
 *
 * Physics: StellarMass = 0.1 * Mvir
 *
 * This simplified prescription assumes:
 * - 10% of the halo virial mass converts to stars
 * - No dependence on halo properties, redshift, or other factors
 * - Serves as a placeholder for more sophisticated star formation models
 */

#ifndef STELLAR_MASS_H
#define STELLAR_MASS_H

/**
 * @brief Register the stellar mass module
 *
 * Registers this module with the module system. Should be called once
 * during program initialization before module_system_init().
 */
void stellar_mass_register(void);

#endif // STELLAR_MASS_H
