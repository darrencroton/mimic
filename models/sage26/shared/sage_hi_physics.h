/**
 * @file sage_hi_physics.h
 * @brief SAGE26's H I block: the ionisation-corrected atomic hydrogen of the cold disk
 *
 * SAGE26 writes the same H I block twice in R/src/model_starformation_and_feedback.c (R = the
 * staged SAGE26 source, S = that file): in starformation_and_feedback() after the star-formation
 * law (S:1035-1046) and in the feedback-free-burst path starformation_ffb() after it zeroes H2gas
 * (S:1280-1292). Both are
 *
 *   atomicH = ColdGas * HYDROGEN_MASS_FRAC - H2gas, floored at 0
 *   f_ion   = ionized_gas_fraction(ColdGas, atomic_disk_radius(DiskScaleRadius), Hubble_h,
 *                                  SIGMA_HI_CRIT)
 *   H1gas   = (float)(atomicH * (1.0 - f_ion))
 *
 * with atomic_disk_radius (S:130-133, chi * r_s, GAS_DISK_RADIUS_FACTOR 1.0 at S:106) and
 * ionized_gas_fraction (S:135-152: the ionised mass fraction of an exponential gas disk truncated
 * at SIGMA_HI_CRIT, 0.5 Msun/pc^2 at S:97). This header holds the package's one implementation,
 * moved unchanged from sage26_calculate_star_formation so that sage26_ffb_star_formation calls
 * the same code.
 *
 * Rounding points. SAGE26's reference object fuses the ordinary path's atomic hydrogen
 * ColdGas * 0.74 - H2gas into one multiply-subtract (S:1036, model_starformation_and_feedback.o
 * offset 0x728); the package writes it as an explicit fma(). On the FFB path SAGE26 has just stored
 * H2gas = 0.0 (+0.0f, S:1280), and its object computes the atomic hydrogen there as a plain
 * multiply (S:1285, offset 0x19e8): fma(ColdGas, 0.74, -(+0.0)) equals the rounded product
 * ColdGas * 0.74 for every float ColdGas, signed zeros and NaNs included (an exhaustive check over
 * all 2^32 patterns, the oracle README's "The c13 Group, Part 2"), so both paths call this one
 * helper. No other expression here contains a multiply and an add in one expression,
 * so none contracts.
 */

#ifndef MIMIC_SHARED_SAGE_HI_PHYSICS_H
#define MIMIC_SHARED_SAGE_HI_PHYSICS_H

#include <math.h>

#include "types.h"
#include "shared/sage_constants.h"

/** @brief SAGE26's SIGMA_HI_CRIT (S:97): the critical neutral surface density [Msun/pc^2] */
#define MIMIC_SAGE_SIGMA_HI_CRIT 0.5

/** @brief SAGE26's GAS_DISK_RADIUS_FACTOR (S:106): the atomic-to-stellar scale-length ratio */
#define MIMIC_SAGE_GAS_DISK_RADIUS_FACTOR 1.0

/** @brief SAGE26's atomic_disk_radius (S:130-133): chi * r_s */
static inline double mimic_sage_atomic_disk_radius(const double rs_code) {
  return MIMIC_SAGE_GAS_DISK_RADIUS_FACTOR * rs_code;
}

/**
 * @brief SAGE26's ionized_gas_fraction (S:135-152): ionised mass fraction of the cold disk
 *
 * For an exponential gas disk of central surface density Sigma0 = M / (2 pi r_s^2) the gas beyond
 * the radius where Sigma falls to sigma_hi_crit is ionised: f_ion = (1 + ln(Sigma0 /
 * sigma_hi_crit)) * sigma_hi_crit / Sigma0, clamped to [0, 1]. Non-positive inputs give 0; a disk
 * entirely below the cut gives 1. The code-to-physical conversions are SAGE26's CODE_MASS_TO_MSUN
 * and CODE_LENGTH_TO_PC macros (R/src/macros.h:45-46), each left to right.
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
 * @brief SAGE26's H I block (S:1035-1046 and S:1280-1292): writes the float H1gas
 *
 * Reads the galaxy's float ColdGas, H2gas and DiskScaleRadius as they stand at the call: on the
 * ordinary path after the star-formation law has written H2gas (SFprescription 1) or left it
 * (SFprescription 0), on the FFB path after H2gas has been set to 0. Writes only H1gas.
 *
 * @param gal       Galaxy whose H1gas is written
 * @param hubble_h  The run's Hubble_h
 */
static inline void mimic_sage_calculate_h1gas(struct GalaxyData *gal, const double hubble_h) {
  /* SAGE parity: SAGE26's build fuses ColdGas * HYDROGEN_MASS_FRAC - H2gas into one
   * multiply-subtract on the ordinary path (S:1036, model_starformation_and_feedback.o offset
   * 0x728); with H2gas +0 (the FFB path, S:1280) the fused form equals the plain multiply the
   * object computes there (S:1285, offset 0x19e8) */
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
