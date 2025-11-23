#ifndef SAGE_INFALL_CONSTANTS_H
#define SAGE_INFALL_CONSTANTS_H

/**
 * @file    sage_infall_constants.h
 * @brief   Physical constants for SAGE infall and reionization physics
 * @author  Mimic Development Team
 * @date    2025-11-23
 *
 * This file contains physically meaningful constants used in the sage_infall
 * module. Constants are grouped by physical process and documented with:
 *   - Physical meaning and interpretation
 *   - Units
 *   - Literature references where applicable
 *
 * Purpose: Extract magic numbers from formulas to improve code clarity and
 *          scientific documentation. All constants preserve exact numerical
 *          values to maintain bit-identical results.
 *
 * References:
 *   - Gnedin (2000): "Effect of Reionization on Structure Formation in the
 *     Cold Dark Matter Cosmology", ApJ, 542, 535
 *   - Kravtsov et al. (2004): "The Dark Side of the Halo Occupation
 *     Distribution", ApJ, 609, 35
 *   - Bryan & Norman (1998): "Statistical Properties of X-Ray Clusters:
 *     Analytic and Numerical Comparisons", ApJ, 495, 80
 */

/* ============================================================================
 * JEANS MASS CALCULATION
 * ============================================================================
 * The Jeans mass represents the critical mass above which gravity overcomes
 * thermal pressure, allowing gas to collapse and form structures.
 *
 * Formula: M_J = MJEANS_BASE_COEFF * Omega^-0.5 * IONIZED_GAS_MU_FACTOR
 * ============================================================================ */

/**
 * @brief   Base coefficient for Jeans mass calculation
 * @details Jeans mass formula: M_J = 25.0 * Omega^-0.5 * mu^-1.5
 *          This coefficient represents the mass scale in units of 1e10 Msun/h
 *          at which thermal pressure balances gravitational collapse.
 * @units   Coefficient in units of 1e10 Msun/h
 * @value   25.0
 * @ref     Gnedin (2000)
 */
#define MJEANS_BASE_COEFF 25.0

/**
 * @brief   Mean molecular weight factor for fully ionized gas
 * @details mu^-1.5 where mu = 0.59 for fully ionized primordial composition
 *          (mass fraction: X=0.76 hydrogen, Y=0.24 helium)
 *          Calculation: 0.59^-1.5 = 2.2067... ≈ 2.21
 *          This factor accounts for the reduced mean particle mass in ionized
 *          gas compared to neutral gas, affecting the Jeans mass.
 * @units   Dimensionless
 * @value   2.21
 * @ref     Standard primordial composition (Planck Collaboration 2018)
 */
#define IONIZED_GAS_MU_FACTOR 2.21

/* ============================================================================
 * CHARACTERISTIC VELOCITY AND MASS
 * ============================================================================
 * These constants define the characteristic velocity and mass scales for
 * reionization feedback. Halos below these scales are suppressed by
 * photoionization heating.
 * ============================================================================ */

/**
 * @brief   Temperature coefficient for characteristic velocity
 * @details V_char = sqrt(T_vir / TEMP_TO_VEL_COEFF)
 *          For T_vir = 10^4 K (reionization temperature), this gives the
 *          characteristic velocity for halos affected by reionization.
 *          Derivation: V^2 = k_B * T / (mu * m_p) yields coefficient ~36.0
 * @units   K/(km/s)^2
 * @value   36.0
 * @ref     Gnedin (2000), reionization physics
 */
#define TEMP_TO_VEL_COEFF 36.0

/* ============================================================================
 * CRITICAL OVERDENSITY (Spherical Collapse)
 * ============================================================================
 * The critical overdensity determines the threshold for gravitational
 * collapse in the spherical collapse model. It varies with cosmology and
 * redshift according to the Bryan & Norman (1998) fitting formula.
 *
 * Formula: δ_c(z) = 18π^2 + 82x - 39x^2, where x = Ω(z) - 1
 * ============================================================================ */

/**
 * @brief   Leading coefficient in critical overdensity formula
 * @details δ_c(z) = DELTACRIT_COEFF_0 * π^2 + DELTACRIT_COEFF_1 * x - DELTACRIT_COEFF_2 * x^2
 *          where x = Ω(z) - 1
 *          The factor of 18 comes from the spherical collapse model in an
 *          Einstein-de Sitter universe.
 * @units   Dimensionless
 * @value   18.0
 * @ref     Bryan & Norman (1998)
 */
#define DELTACRIT_COEFF_0 18.0

/**
 * @brief   Linear coefficient in critical overdensity formula
 * @details First-order correction to critical overdensity for non-EdS cosmology
 * @units   Dimensionless
 * @value   82.0
 * @ref     Bryan & Norman (1998)
 */
#define DELTACRIT_COEFF_1 82.0

/**
 * @brief   Quadratic coefficient in critical overdensity formula
 * @details Second-order correction to critical overdensity for non-EdS cosmology
 * @units   Dimensionless
 * @value   39.0
 * @ref     Bryan & Norman (1998)
 */
#define DELTACRIT_COEFF_2 39.0

/**
 * @brief   Factor in characteristic mass calculation
 * @details Used in M_char calculation: sqrt(DELTACRIT_FACTOR * δ_crit(z))
 *          The factor of 0.5 appears in the relationship between virial
 *          velocity and the Hubble parameter at collapse.
 * @units   Dimensionless
 * @value   0.5
 */
#define DELTACRIT_FACTOR 0.5

/* ============================================================================
 * HUBBLE CONSTANT CONVERSION
 * ============================================================================ */

/**
 * @brief   Hubble constant conversion factor
 * @details H_0 = HUBBLE_CONVERSION * h km/s/Mpc
 *          Standard cosmological definition: H_0 = 100 h km/s/Mpc where h is
 *          the dimensionless Hubble parameter (typically h ~ 0.7).
 * @units   km/s/Mpc per h
 * @value   100.0
 * @ref     Standard cosmological convention
 */
#define HUBBLE_CONVERSION 100.0

/* ============================================================================
 * REIONIZATION SUPPRESSION (Gnedin 2000)
 * ============================================================================
 * These constants parameterize the suppression of gas accretion onto halos
 * during and after cosmic reionization. The Gnedin (2000) fitting formula
 * captures how photoionization heating increases the Jeans mass, preventing
 * gas from cooling into low-mass halos.
 *
 * Formula: f_suppression = 1 / (1 + GNEDIN_SUPPRESSION_COEFF * M_filter/M_vir)^3
 * ============================================================================ */

/**
 * @brief   Gnedin (2000) reionization suppression coefficient
 * @details Suppression factor: 1 / (1 + GNEDIN_SUPPRESSION_COEFF * M_filter/M_vir)^GNEDIN_SUPPRESSION_POWER
 *          This coefficient controls the strength of reionization feedback.
 *          Larger values = stronger suppression of gas accretion.
 *          Value calibrated to match simulations of reionization effects.
 * @units   Dimensionless
 * @value   0.26
 * @ref     Kravtsov et al. (2004) Appendix B
 */
#define GNEDIN_SUPPRESSION_COEFF 0.26

/**
 * @brief   Power-law index for Gnedin suppression formula
 * @details Controls the steepness of the suppression as a function of mass ratio.
 *          Cubic dependence (power = 3) gives strong suppression for halos with
 *          M_vir < M_filter (filtering mass scale).
 * @units   Dimensionless
 * @value   3.0
 * @ref     Gnedin (2000)
 */
#define GNEDIN_SUPPRESSION_POWER 3.0

/* ============================================================================
 * EXPONENT VALUES (Physical Formulas)
 * ============================================================================
 * These are exponents used in various physics formulas. While they appear as
 * "magic numbers", they represent fundamental scaling relationships.
 * ============================================================================ */

/**
 * @brief   Exponent for filtering mass calculation
 * @details M_filter = M_J * f(a)^FILTERING_MASS_EXPONENT
 *          The 1.5 power-law relates filtering mass to the integral of the
 *          ionization fraction over time.
 * @units   Dimensionless
 * @value   1.5
 * @ref     Gnedin (2000)
 */
#define FILTERING_MASS_EXPONENT 1.5

#endif /* SAGE_INFALL_CONSTANTS_H */
