/**
 * @file metallicity.h
 * @brief Common metallicity calculation utilities for physics modules
 *
 * This shared utility provides robust metallicity calculations used by
 * multiple physics modules across the gas-cycle, feedback, and merger chains.
 *
 * @note This file is part of the model-local shared utilities system.
 *       Modules include it using relative paths: #include "shared/metallicity.h"
 */

#ifndef MIMIC_SHARED_METALLICITY_H
#define MIMIC_SHARED_METALLICITY_H

#include "constants.h"

/**
 * @brief Calculate metallicity (metal mass fraction) with safety checks
 *
 * Computes the metallicity Z = M_metals / M_gas, handling edge cases
 * where gas or metal mass is zero or negative to prevent division by zero
 * and unphysical negative metallicities.
 * Enforces physical limit: metallicity ≤ 1.0 (100% metal mass fraction).
 *
 * SAGE parity (model_misc.c get_metallicity): double-precision arithmetic,
 * returns 0 unless both gas > 0 and metals > 0, caps at 1.0. Negative metal
 * reservoirs (possible from float cancellation) must yield Z = 0, not Z < 0.
 *
 * @param gas     Total gas mass (e.g., HotGas, ColdGas, EjectedGas)
 * @param metals  Metal mass in the gas (e.g., MetalsHotGas)
 * @return        Metallicity (0.0 to 1.0)
 *
 * Example usage:
 * @code
 *   double Z = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);
 *   double stripped_metals = stripped_gas * Z;
 * @endcode
 */
static inline double mimic_get_metallicity(double gas, double metals) {
  double metallicity = 0.0;

  if (gas > 0.0 && metals > 0.0) {
    metallicity = metals / gas;
    if (metallicity >= 1.0)
      metallicity = 1.0;
  }

  return metallicity;
}

#endif /* MIMIC_SHARED_METALLICITY_H */
