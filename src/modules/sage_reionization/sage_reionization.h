/**
 * @file    sage_reionization.h
 * @brief   SAGE reionization suppression module interface
 *
 * Calculates halo-specific baryon fractions modified by reionization suppression
 * following the Gnedin (2000) model. After cosmic reionization, gas accretion onto
 * low-mass halos is suppressed due to increased gas temperature and Jeans mass.
 *
 * Physics: HaloBaryonFraction = GlobalBaryonFraction × f_reion(Mvir, z)
 *
 * This module MUST run before modules that use HaloBaryonFraction (sage_infall,
 * sage_satellite_stripping).
 *
 * Reference: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2016)
 */

#ifndef SAGE_REIONIZATION_H
#define SAGE_REIONIZATION_H

/**
 * @brief   Register the sage_reionization module with the module registry
 */
void sage_reionization_register(void);

#endif /* SAGE_REIONIZATION_H */
