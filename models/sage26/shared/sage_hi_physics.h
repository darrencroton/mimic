/**
 * @file sage_hi_physics.h
 * @brief SAGE26's H I block: the ionisation-corrected atomic hydrogen of the cold disk
 *
 * Implements the H I block SAGE26 runs in both starformation_and_feedback (after the
 * star-formation law) and starformation_ffb (after it zeroes H2gas), together with its helpers
 * atomic_disk_radius and ionized_gas_fraction:
 *
 *   atomicH = ColdGas * HYDROGEN_MASS_FRAC - H2gas, floored at 0
 *   f_ion   = ionized_gas_fraction(ColdGas, atomic_disk_radius(DiskScaleRadius), Hubble_h,
 *                                  SIGMA_HI_CRIT)
 *   H1gas   = (float)(atomicH * (1.0 - f_ion))
 *
 * Used by sage26_calculate_star_formation and sage26_ffb_star_formation.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_HI_PHYSICS_H
#define MIMIC_SHARED_SAGE_HI_PHYSICS_H

#include <math.h>

#include "types.h"
#include "shared/sage_constants.h"

/** @brief SAGE26's SIGMA_HI_CRIT: the critical neutral surface density [Msun/pc^2] */
#define MIMIC_SAGE_SIGMA_HI_CRIT 0.5

/** @brief SAGE26's GAS_DISK_RADIUS_FACTOR: the atomic-to-stellar scale-length ratio */
#define MIMIC_SAGE_GAS_DISK_RADIUS_FACTOR 1.0

/** @brief SAGE26's atomic_disk_radius: chi * r_s */
static inline double mimic_sage_atomic_disk_radius(const double rs_code) {
  return MIMIC_SAGE_GAS_DISK_RADIUS_FACTOR * rs_code;
}

/**
 * @brief SAGE26's ionized_gas_fraction: ionised mass fraction of the cold disk
 *
 * For an exponential gas disk of central surface density Sigma0 = M / (2 pi r_s^2) the gas beyond
 * the radius where Sigma falls to sigma_hi_crit is ionised: f_ion = (1 + ln(Sigma0 /
 * sigma_hi_crit)) * sigma_hi_crit / Sigma0, clamped to [0, 1]. Non-positive inputs give 0; a disk
 * entirely below the cut gives 1. The code-to-physical conversions are SAGE26's CODE_MASS_TO_MSUN
 * and CODE_LENGTH_TO_PC macros, each evaluated left to right.
 */
static inline double mimic_sage_ionized_gas_fraction(const double coldgas_code,
                                                     const double rs_code, const double h,
                                                     const double sigma_hi_crit) {
  if (coldgas_code <= 0.0 || rs_code <= 0.0 || sigma_hi_crit <= 0.0 || h <= 0.0) {
    return 0.0;
  }
  const double mgas = coldgas_code * 1.0e10 / h;             /* Msun */
  const double rs_pc = rs_code * 1.0e6 / h;                  /* pc */
  const double sigma0 = mgas / (2.0 * M_PI * rs_pc * rs_pc); /* Msun/pc^2 */
  if (sigma0 <= sigma_hi_crit) {
    return 1.0;
  }
  const double x = log(sigma0 / sigma_hi_crit);
  double f_ion = (1.0 + x) * sigma_hi_crit / sigma0;
  if (f_ion < 0.0) {
    f_ion = 0.0;
  }
  if (f_ion > 1.0) {
    f_ion = 1.0;
  }
  return f_ion;
}

/**
 * @brief SAGE26's H I block (starformation_and_feedback, starformation_ffb): writes H1gas
 *
 * Reads the galaxy's float ColdGas, H2gas and DiskScaleRadius as they stand at the call: on the
 * ordinary path after the star-formation law has written H2gas (SFprescription 1) or left it
 * (SFprescription 0), on the FFB path after H2gas has been set to 0. Writes only H1gas.
 *
 * @param gal       Galaxy whose H1gas is written
 * @param hubble_h  The run's Hubble_h
 */
static inline void mimic_sage_calculate_h1gas(struct GalaxyData *gal, const double hubble_h) {
  /* SAGE parity: ColdGas * HYDROGEN_MASS_FRAC - H2gas is evaluated as one fma, as SAGE26's
   * build computes it */
  double atomicH = fma((double)gal->ColdGas, SAGE_HYDROGEN_MASS_FRAC, -(double)gal->H2gas);
  if (atomicH < 0.0) {
    atomicH = 0.0;
  }
  const double f_ion = mimic_sage_ionized_gas_fraction(
      gal->ColdGas, mimic_sage_atomic_disk_radius(gal->DiskScaleRadius), hubble_h,
      MIMIC_SAGE_SIGMA_HI_CRIT);
  atomicH *= (1.0 - f_ion);
  gal->H1gas = atomicH;
}

#endif /* MIMIC_SHARED_SAGE_HI_PHYSICS_H */
