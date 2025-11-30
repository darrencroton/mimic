/**
 * @file    physical_constants.h
 * @brief   Universal physical constants for galaxy evolution modules
 *
 * Single source of truth for physical constants shared across modules.
 * One constant per line with inline comment: value, symbol/formula, reference.
 * For detailed physics, see module documentation or cited papers.
 *
 * Location: src/modules/_system/physical_constants.h
 * Framework infrastructure - do not modify unless adding universal constants.
 *
 * Vision Principle: Single Source of Truth - eliminate duplication across modules.
 */

#ifndef MIMIC_SYSTEM_PHYSICAL_CONSTANTS_H
#define MIMIC_SYSTEM_PHYSICAL_CONSTANTS_H

/* ===== CGS Fundamental Constants ===== */
/* Used for unit conversions in core (init.c::set_units()) */

#define GRAVITY 6.672e-8               /* G in CGS: cm³/(g·s²) */
#define SOLAR_MASS 1.989e33            /* M_sun in grams */
#define CM_PER_MPC 3.085678e24         /* Conversion: cm per Mpc */
#define HUBBLE 3.2407789e-18           /* H_0 in h/sec */
#define SEC_PER_MEGAYEAR 3.155e13      /* Seconds per megayear */
#define SEC_PER_YEAR 3.155e7           /* Seconds per year */
#define PROTONMASS 1.6726e-24          /* Proton mass in grams */
#define BOLTZMANN 1.3806e-16           /* Boltzmann constant in erg/K */

/* ===== Speed of Light (various units) ===== */

static const double C_CGS = 2.9979e10;               /* c in cm/s */
static const double C_KM_S = 2.99792458e5;           /* c in km/s */
static const double C_SQUARED_CGS = 8.9875e20;       /* c² in cm²/s² (exact: C_CGS²) */

/* ===== Stellar Populations & Metallicity ===== */

static const double SOLAR_METALLICITY = 0.02;        /* Z_sun by mass (Asplund+ 2009) */

/* ===== Supernovae ===== */

static const double SN_ENERGY_ERG = 1.0e51;          /* E_SN per event (erg) */

/* ===== Accretion & Black Holes ===== */

static const double RADIATIVE_EFFICIENCY = 0.1;      /* η, thin disk efficiency (Shakura & Sunyaev 1973) */

/* ===== Feedback & Ejection ===== */

static const double METAL_MASS_SCALE = 30.0;         /* Metal ejection scale, 10^10 Msun/h (calibrated) */
static const double KINETIC_ENERGY_FACTOR = 0.5;     /* E = ½mv² coefficient */

#endif /* MIMIC_SYSTEM_PHYSICAL_CONSTANTS_H */
