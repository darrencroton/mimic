#ifndef SAGE_COOLING_CONSTANTS_H
#define SAGE_COOLING_CONSTANTS_H

/**
 * @file    sage_cooling_constants.h
 * @brief   Physical constants for SAGE cooling and AGN feedback physics
 * @author  Mimic Development Team
 * @date    2025-11-23
 *
 * This file contains physically meaningful constants used in the sage_cooling
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
 *   - Croton et al. (2006): "The many lives of active galactic nuclei: cooling
 *     flows, black holes and the luminosities and colours of galaxies", MNRAS, 365, 11
 *   - De Lucia & Blaizot (2007): "The hierarchical formation of the brightest
 *     cluster galaxies", MNRAS, 375, 2
 */

/* ============================================================================
 * COOLING RADIUS AND RATE CALCULATIONS
 * ============================================================================ * These constants are used in calculating the cooling radius (r_cool) and
 * gas cooling rates from hot halos.
 * ============================================================================ */

/**
 * @brief   Mean molecular weight factor for fully ionized gas
 * @details Factor appearing in cooling rate: 3/2 * mu where mu = 0.59
 *          Calculation: 1.5 * 0.59 = 0.885
 *          This relates the density at the cooling radius to the cooling time.
 *          Physics: rho(r_cool) = (m_cool/t_cool) * (3/2 * mu)
 * @units   Dimensionless
 * @value   0.885
 */
#define COOLING_MU_FACTOR 0.885

/**
 * @brief   Sphere volume coefficient (4π approximation)
 * @details Used in: rho_0 = M_hot / (4π * R_vir)
 *          The exact coefficient is 4π/3 ≈ 4.19, but simplified to 4.0 in
 *          the original SAGE implementation for computational efficiency.
 *          This is a legacy approximation maintained for consistency.
 * @units   Dimensionless
 * @value   4.0
 * @note    Approximation: exact value is 4π/3 ≈ 4.189
 */
#define SPHERE_VOLUME_COEFF 4.0

/**
 * @brief   Cooling timescale divisor
 * @details Used in: cooling_rate = (M_hot/R_vir) * (r_cool / (2 * t_cool)) * dt
 *          The factor of 2 relates the dynamical time to the cooling time.
 * @units   Dimensionless
 * @value   2.0
 */
#define COOLING_TIME_DIVISOR 2.0

/* ============================================================================
 * AGN MODE 2: BONDI-HOYLE ACCRETION
 * ============================================================================
 * Constants for spherical accretion onto supermassive black holes.
 * Formula: dM/dt = 2.5π * G * rho * M_BH^2 / c_s^3
 * ============================================================================ */

/**
 * @brief   Bondi-Hoyle accretion coefficient
 * @details Leading coefficient in Bondi-Hoyle formula: 2.5π
 *          This represents the geometrical factor for spherical accretion
 *          in the subsonic regime.
 * @units   Dimensionless
 * @value   2.5
 * @ref     Bondi (1952), Hoyle & Lyttleton (1939)
 */
#define BONDI_HOYLE_COEFF 2.5

/**
 * @brief   Bondi-Hoyle density factor
 * @details Factor appearing in density term: 0.375 * 0.6 = 0.225
 *          The 0.375 relates to the density profile, and 0.6 is a
 *          calibration factor from simulations.
 * @units   Dimensionless
 * @value   0.375
 */
#define BONDI_DENSITY_FACTOR 0.375

/**
 * @brief   Bondi-Hoyle sound speed factor
 * @details Sound speed calibration factor from simulations
 * @units   Dimensionless
 * @value   0.6
 */
#define BONDI_SOUND_SPEED_FACTOR 0.6

/* ============================================================================
 * AGN MODE 3: COLD CLOUD ACCRETION
 * ============================================================================
 * Threshold-triggered accretion when black hole mass exceeds a critical
 * fraction of the halo virial mass.
 * ============================================================================ */

/**
 * @brief   Black hole mass threshold fraction (0.01% of virial mass)
 * @details Condition: M_BH > BH_MASS_THRESHOLD_FRAC * M_vir * (r_cool/R_vir)^3
 *          This represents the minimum BH mass required to trigger cold
 *          cloud accretion mode, scaled by the cooling region size.
 * @units   Dimensionless fraction
 * @value   0.0001 (0.01%)
 * @ref     Croton et al. (2006)
 */
#define BH_MASS_THRESHOLD_FRAC 0.0001

/**
 * @brief   Cold cloud AGN accretion rate fraction
 * @details Rate: dM/dt = COLD_CLOUD_ACCRETION_FRAC * (M_cool/dt)
 *          When triggered, BH accretes 0.01% of the current cooling rate.
 * @units   Dimensionless fraction
 * @value   0.0001 (0.01%)
 * @ref     Croton et al. (2006)
 */
#define COLD_CLOUD_ACCRETION_FRAC 0.0001

/* ============================================================================
 * AGN MODE 1: EMPIRICAL ACCRETION (Default)
 * ============================================================================
 * Empirical scaling formula calibrated to simulations and observations.
 * Formula: dM/dt ~ η * (M_BH/0.01) * (V_vir/200)^3 * (f_hot/0.1)
 * ============================================================================ */

/**
 * @brief   Black hole mass normalization (10^8 Msun in code units)
 * @details Normalization: M_BH / BH_MASS_NORM
 *          Value 0.01 in code units of 1e10 Msun/h = 10^8 Msun/h
 *          This is a typical BH mass scale for massive galaxies.
 * @units   Code units (1e10 Msun/h)
 * @value   0.01
 * @ref     Croton et al. (2006)
 */
#define BH_MASS_NORM 0.01

/**
 * @brief   Virial velocity normalization for AGN accretion
 * @details Normalization: (V_vir / VVIR_NORM)^3
 *          Value 200 km/s is characteristic of group/cluster-scale halos
 *          where radio-mode AGN feedback is important.
 * @units   km/s
 * @value   200.0
 * @ref     Croton et al. (2006)
 */
#define VVIR_AGN_NORM 200.0

/**
 * @brief   Hot gas fraction normalization
 * @details Normalization: (f_hot / HOT_GAS_FRAC_NORM)
 *          Value 0.1 (10%) is a typical hot gas fraction in massive halos.
 * @units   Dimensionless fraction
 * @value   0.1
 * @ref     Croton et al. (2006)
 */
#define HOT_GAS_FRAC_NORM 0.1

/**
 * @brief   Eddington velocity scale (sqrt of 2 * η * c^2)
 * @details V_Edd = sqrt(2 * η * c^2) where η = 0.1 (radiative efficiency)
 *          Calculation: sqrt(2 * 0.1 * (3e5 km/s)^2) ≈ 1.34e5 km/s
 *          This velocity scale relates the Eddington accretion rate to
 *          the virial velocity.
 * @units   km/s
 * @value   1.34e5
 * @ref     Standard Eddington accretion theory
 */
#define EDDINGTON_VELOCITY_SCALE 1.34e5

/* ============================================================================
 * ENERGY CALCULATIONS
 * ============================================================================
 * Constants for kinetic energy calculations in cooling and heating.
 * ============================================================================ */

/**
 * @brief   Kinetic energy factor (1/2 in E = 1/2 * m * v^2)
 * @details Used in:
 *          - Cooling energy: E_cool = 0.5 * M_cool * V_vir^2
 *          - Heating energy: E_heat = 0.5 * M_heat * V_vir^2
 *          - Binding energy: E_bind = 0.5 * M * V_vir^2
 *          This represents the change in gravitational potential energy
 *          as gas moves within the halo potential well.
 * @units   Dimensionless
 * @value   0.5
 * @note    Fundamental physics constant (kinetic energy formula)
 */
#define KINETIC_ENERGY_FACTOR 0.5

#endif /* SAGE_COOLING_CONSTANTS_H */
