/**
 * @file    sage_starformation_feedback_constants.h
 * @brief   Physics constants for SAGE star formation and feedback module
 *
 * This header defines all physics constants used in the SAGE star formation
 * and feedback module. These constants were previously hardcoded and have
 * been extracted for clarity and maintainability.
 *
 * Constants are organized by physics process:
 * - Star Formation (Kennicutt-Schmidt law parameters)
 * - Metal Distribution (Krumholz & Dekel 2011 model)
 * - Numerical Thresholds
 *
 * References:
 *   - Kauffmann (1996) - Critical gas density threshold
 *   - Kennicutt (1998) - Star formation law
 *   - Krumholz & Dekel (2011) - Metal distribution model
 *   - SAGE: sage-code/model_starformation_and_feedback.c
 *
 * Created: 2025-11-23 (Issue 1.3.5 - Hardcoded Magic Numbers)
 */

#ifndef SAGE_STARFORMATION_FEEDBACK_CONSTANTS_H
#define SAGE_STARFORMATION_FEEDBACK_CONSTANTS_H

// ============================================================================
// STAR FORMATION PHYSICS
// ============================================================================

/**
 * @brief   Effective star-forming radius factor
 *
 * Multiplier to convert disk scale radius to effective star-forming radius.
 * The factor of 3.0 represents approximately 3 scale lengths, which contains
 * most of the disk mass in an exponential disk profile.
 *
 * Physics:
 *   R_eff = EFFECTIVE_RADIUS_FACTOR * R_disk
 *
 * In an exponential disk with surface density Σ(r) = Σ₀ exp(-r/Rd),
 * 3 scale lengths encloses ~95% of the disk mass.
 *
 * Units: dimensionless
 * Value: 3.0
 * Reference: Standard exponential disk model
 */
#define EFFECTIVE_RADIUS_FACTOR 3.0

/**
 * @brief   Critical gas mass coefficient (Kauffmann 1996)
 *
 * Coefficient in the critical cold gas mass formula for star formation.
 * From Kauffmann (1996) eq. 7, the critical surface density for star
 * formation is Σ_crit = 0.19 * V_vir / R_eff.
 *
 * Physics:
 *   M_crit = CRITICAL_GAS_COEFF * V_vir * R_eff
 *
 * This represents the threshold gas mass needed to overcome the
 * stabilizing effects of stellar velocity dispersion in the disk.
 *
 * Units: dimensionless
 * Value: 0.19
 * Reference: Kauffmann (1996), eq. 7
 */
#define CRITICAL_GAS_COEFF 0.19

// ============================================================================
// METAL DISTRIBUTION PHYSICS
// ============================================================================

/**
 * @brief   Mass scale for metal ejection (Krumholz & Dekel 2011)
 *
 * Characteristic halo mass scale (in 1e10 Msun/h) for the mass-dependent
 * fraction of newly produced metals that bypass the disk and go directly
 * to the hot halo gas.
 *
 * Physics:
 *   FracZleaveDisk(M) = FracZleaveDisk_norm * exp(-M_vir / METAL_MASS_SCALE)
 *
 * Lower-mass halos (M < 30 × 1e10 Msun/h ≈ 3e11 Msun/h) have stronger
 * outflows that can carry metals directly to the hot phase, bypassing
 * the cold disk. Higher-mass halos retain metals in the disk.
 *
 * Units: 1e10 Msun/h
 * Value: 30.0
 * Reference: Krumholz & Dekel (2011), eq. 22
 */
#define METAL_MASS_SCALE 30.0

// ============================================================================
// NUMERICAL THRESHOLDS
// ============================================================================

/**
 * @brief   Minimum gas mass for metallicity calculation
 *
 * Threshold below which metallicity calculations are skipped to avoid
 * numerical issues with division by very small masses.
 *
 * When ColdGas < GAS_MASS_THRESHOLD, newly produced metals are sent
 * directly to the hot gas phase instead of being distributed between
 * cold and hot phases.
 *
 * Units: 1e10 Msun/h
 * Value: 1.0e-8 (equivalent to 100 Msun/h)
 * Purpose: Numerical stability
 */
#define GAS_MASS_THRESHOLD 1.0e-8

#endif  /* SAGE_STARFORMATION_FEEDBACK_CONSTANTS_H */
