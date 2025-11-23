#ifndef SAGE_MERGERS_CONSTANTS_H
#define SAGE_MERGERS_CONSTANTS_H

/**
 * @file    sage_mergers_constants.h
 * @brief   Physical constants for SAGE merger and starburst physics
 * @author  Mimic Development Team
 * @date    2025-11-23
 *
 * This file contains physically meaningful constants used in the sage_mergers
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
 *   - Kauffmann & Haehnelt (2000): "A unified model for the evolution of
 *     galaxies and quasars", MNRAS, 311, 576
 *   - Cox et al. (2006): "Feedback in simulations of disc-galaxy major mergers",
 *     MNRAS, 373, 1013
 *   - Binney & Tremaine (2008): "Galactic Dynamics", 2nd edition
 */

/* ============================================================================
 * GRAVITATIONAL CONSTANT IN CODE UNITS
 * ============================================================================
 * The gravitational constant G converted to Mimic's internal unit system.
 * ============================================================================ */

/**
 * @brief   Gravitational constant in code units
 * @details G in units of (Mpc/h) * (km/s)^2 / (1e10 Msun/h)
 *
 *          Derivation from CGS:
 *          G_CGS = 6.674e-8 cm^3 g^-1 s^-2
 *
 *          Unit conversions:
 *          - Length: 1 Mpc/h = 3.086e24 cm * h
 *          - Mass: 1e10 Msun/h = 1.989e43 g * h
 *          - Velocity: 1 km/s = 1e5 cm/s
 *
 *          Result: G_code = 43.0071 (Mpc/h) (km/s)^2 / (1e10 Msun/h)
 *
 *          This value is used in dynamical friction calculations and other
 *          gravitational processes where explicit G is needed.
 *
 * @units   (Mpc/h) * (km/s)^2 / (1e10 Msun/h)
 * @value   43.0071
 * @ref     Standard gravitational constant with Mimic unit conversion
 */
#define G_CODE_UNITS 43.0071

/* ============================================================================
 * DYNAMICAL FRICTION TIMESCALE (Chandrasekhar Formula)
 * ============================================================================
 * Constants for calculating merger timescales via dynamical friction.
 * Formula: T_merge = (2.0 * 1.17 * R^2 * V) / (ln(Λ) * G * M_sat)
 * ============================================================================ */

/**
 * @brief   Chandrasekhar dynamical friction coefficient
 * @details Leading coefficient in Chandrasekhar's dynamical friction formula.
 *          The factor of 2.0 comes from orbital averaging.
 * @units   Dimensionless
 * @value   2.0
 * @ref     Chandrasekhar (1943), Binney & Tremaine (2008)
 */
#define CHANDRASEKHAR_COEFF 2.0

/**
 * @brief   Coulomb logarithm approximation
 * @details ln(Λ) ≈ 1.17 for typical mass ratios in galaxy mergers
 *          The Coulomb logarithm ln(M_host/M_sat) is approximated as a
 *          constant for computational efficiency. Value 1.17 corresponds
 *          to a mass ratio of about 3:1, typical for minor mergers.
 * @units   Dimensionless
 * @value   1.17
 * @ref     Binney & Tremaine (2008), typical for mass ratio ~3
 */
#define COULOMB_LOG_APPROX 1.17

/* ============================================================================
 * BLACK HOLE GROWTH IN MERGERS
 * ============================================================================
 * Constants for black hole accretion during galaxy mergers.
 * Formula: dM_BH/dt ~ (M_sat/M_cen)^b / (1 + (V_0/V_vir)^2) * M_cold
 * ============================================================================ */

/**
 * @brief   Velocity threshold for black hole growth
 * @details V_0 in the denominator: 1 + (V_0/V_vir)^2
 *          This velocity scale (280 km/s) represents the threshold above
 *          which black hole growth efficiency increases significantly.
 *          Higher virial velocities indicate deeper potential wells where
 *          gas can more easily be funneled to the central BH.
 * @units   km/s
 * @value   280.0
 * @ref     Kauffmann & Haehnelt (2000)
 */
#define BH_GROWTH_VEL_THRESHOLD 280.0

/* ============================================================================
 * QUASAR-MODE FEEDBACK
 * ============================================================================
 * Constants for calculating energy injection from quasar-mode AGN winds.
 * ============================================================================ */

/**
 * @brief   Speed of light in code velocity units
 * @details c = 299792.458 km/s
 *          Used in calculating radiative energy from black hole accretion:
 *          E = η * ε_rad * M_BH * c^2
 * @units   km/s
 * @value   2.99792458e5
 * @ref     Physical constant (speed of light)
 */
#define SPEED_OF_LIGHT_KM_S 2.99792458e5

/**
 * @brief   Code velocity unit normalization
 * @details Mimic uses internal velocity units of 100 km/s
 *          Conversion: c_code = c / CODE_VEL_UNIT = 2997.92458
 * @units   km/s
 * @value   100.0
 */
#define CODE_VEL_UNIT 100.0

/**
 * @brief   Radiative efficiency for BH accretion (standard thin disk)
 * @details ε_rad = 0.1 is the standard radiative efficiency for a
 *          Schwarzschild black hole with a thin accretion disk.
 *          This fraction of rest mass energy is converted to radiation:
 *          L = ε_rad * dM/dt * c^2
 * @units   Dimensionless fraction
 * @value   0.1
 * @ref     Shakura & Sunyaev (1973), standard thin disk model
 */
#define RADIATIVE_EFFICIENCY_STANDARD 0.1

/* ============================================================================
 * MERGER-INDUCED STARBURSTS
 * ============================================================================
 * Constants for calculating starburst efficiency during galaxy mergers.
 * Formula: ε_burst = 0.56 * (M_sat/M_cen)^0.7
 * ============================================================================ */

/**
 * @brief   Starburst efficiency normalization
 * @details Normalization coefficient in starburst formula: ε_burst = STARBURST_NORM * mass_ratio^STARBURST_POWER
 *          Value 0.56 means 56% of cold gas is converted to stars in an
 *          equal-mass merger (mass_ratio = 1). Efficiency decreases for
 *          more unequal mergers following the power-law below.
 * @units   Dimensionless fraction
 * @value   0.56
 * @ref     Cox et al. (2006 thesis), calibrated to hydrodynamic simulations
 */
#define STARBURST_EFFICIENCY_NORM 0.56

/**
 * @brief   Starburst mass-ratio power-law index
 * @details Power-law index in: ε_burst = 0.56 * mass_ratio^STARBURST_POWER
 *          The value 0.7 indicates that more equal-mass mergers (higher
 *          mass_ratio) produce more efficient starbursts due to stronger
 *          tidal interactions and gas compression.
 * @units   Dimensionless
 * @value   0.7
 * @ref     Cox et al. (2006 thesis)
 */
#define STARBURST_MASS_RATIO_POWER 0.7

/* ============================================================================
 * METAL EJECTION EFFICIENCY
 * ============================================================================
 * Mass scale for metal ejection from disk to hot halo during minor mergers.
 * ============================================================================ */

/**
 * @brief   Characteristic mass scale for metal ejection
 * @details M_scale = 30.0 in units of 1e10 Msun/h = 3e11 Msun/h
 *          Used in: frac_eject = frac_base * exp(-M_vir / METAL_EJECTION_MASS_SCALE)
 *
 *          Physical interpretation: Low-mass halos (M_vir << 30) have shallow
 *          potential wells, allowing supernova-driven winds to eject metals
 *          from the disk to the hot halo. High-mass halos (M_vir >> 30) have
 *          deep potentials that retain metals in the cold disk (exponential
 *          suppression of ejection).
 *
 *          This mass scale (3e11 Msun/h) corresponds roughly to the transition
 *          between disk-dominated and bulge-dominated galaxies.
 *
 * @units   Code units (1e10 Msun/h)
 * @value   30.0 (= 3e11 Msun/h)
 * @ref     SAGE implementation, calibrated to observations
 */
#define METAL_EJECTION_MASS_SCALE 30.0

/* ============================================================================
 * BINDING ENERGY CALCULATIONS
 * ============================================================================
 * Constants for gravitational binding energy calculations.
 * ============================================================================ */

/**
 * @brief   Binding energy factor (1/2 in E_bind = 1/2 * M * V_vir^2)
 * @details Used in calculating the gravitational binding energy of gas
 *          reservoirs (cold gas, hot gas) in the halo potential:
 *          E_bind = BINDING_ENERGY_FACTOR * M_gas * V_vir^2
 *
 *          This represents the energy required to unbind gas from the halo.
 *          Quasar-mode feedback compares AGN energy injection to this
 *          binding energy to determine how much gas is ejected.
 * @units   Dimensionless
 * @value   0.5
 * @note    Fundamental physics constant (kinetic energy formula)
 */
#define BINDING_ENERGY_FACTOR 0.5

#endif /* SAGE_MERGERS_CONSTANTS_H */
