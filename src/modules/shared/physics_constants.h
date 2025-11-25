/**
 * @file    physics_constants.h
 * @brief   Fundamental physics constants shared across SAGE modules
 *
 * This header provides physics constants used by multiple modules.
 * Following the pattern of metallicity.h, constants are defined as
 * static const for inline use.
 *
 * Vision Principle: Single Source of Truth - eliminate duplication across modules.
 *
 * References:
 *   - Shakura & Sunyaev (1973) - Thin accretion disk efficiency
 *   - Standard kinetic energy formula: E = 1/2 * m * v^2
 *   - Metal ejection scaling calibrated to observations
 */

#ifndef MIMIC_SHARED_PHYSICS_CONSTANTS_H
#define MIMIC_SHARED_PHYSICS_CONSTANTS_H

/**
 * @brief   Radiative efficiency for thin accretion disk
 *
 * Fraction of rest mass energy converted to radiation in a thin
 * accretion disk: η = 0.1 (Shakura & Sunyaev 1973).
 * Used in Eddington rate calculations: L = η * dM/dt * c^2
 *
 * Units: dimensionless
 */
static const double RADIATIVE_EFFICIENCY = 0.1;

/**
 * @brief   Characteristic mass scale for metal ejection
 *
 * Mass scale for metal-dependent ejection fraction: 3×10^10 Msun/h.
 * Used in exponential suppression: exp(-M_vir / METAL_MASS_SCALE).
 * Calibrated to match observed galaxy metallicities.
 *
 * Units: 10^10 Msun/h
 */
static const double METAL_MASS_SCALE = 30.0;

/**
 * @brief   Kinetic/binding energy factor
 *
 * Standard coefficient in kinetic energy formula: E = 1/2 * m * v^2.
 * Also used for gravitational binding energy calculations.
 *
 * Units: dimensionless
 */
static const double KINETIC_ENERGY_FACTOR = 0.5;

#endif /* MIMIC_SHARED_PHYSICS_CONSTANTS_H */
