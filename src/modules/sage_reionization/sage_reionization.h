/**
 * @file    sage_reionization.h
 * @brief   SAGE reionization suppression module
 *
 * This module calculates halo-specific baryon fractions modified by
 * reionization suppression following Gnedin (2000).
 *
 * Physics:
 *   HaloBaryonFraction = GlobalBaryonFraction * f_reion(Mvir, z)
 *
 * The reionization suppression factor f_reion depends on the ratio between
 * halo mass and a characteristic mass (maximum of filtering mass and mass
 * corresponding to virial temperature of 10^4 K).
 *
 * This module MUST run before any module that uses HaloBaryonFraction
 * (sage_infall, sage_satellite_stripping).
 *
 * Reference:
 *   - Gnedin (2000) - Reionization model
 *   - Kravtsov et al. (2004) - Filtering mass formulas
 *   - Bryan & Norman (1998) - Critical overdensity
 *   - Croton et al. (2016) - SAGE model description
 *
 * Vision Principles:
 *   - Single Source of Truth: HaloBaryonFraction is the authoritative local baryon fraction
 *   - Runtime Modularity: Can disable reionization by removing this module
 *   - Physics-Agnostic Core: Interacts only through module interface
 */

#ifndef SAGE_REIONIZATION_H
#define SAGE_REIONIZATION_H

/**
 * @brief Register the sage_reionization module
 */
void sage_reionization_register(void);

#endif /* SAGE_REIONIZATION_H */
