/**
 * @file    reionization.h
 * @brief   Shared reionization suppression calculation (Gnedin 2000)
 *
 * SWAPPABLE REIONIZATION MODEL
 * ============================
 * This header contains a complete reionization model implementation that can
 * be swapped out entirely for different reionization prescriptions.
 *
 * To use a different reionization model:
 * 1. Create alternative header (e.g., reionization_okamoto2008.h)
 * 2. Archive this file: mv reionization.h _archive/shared/reionization_gnedin2000.h
 * 3. Install new model: cp new_model.h reionization.h
 * 4. Rebuild: make clean && make
 *
 * No changes to module code required - all modules include this shared header.
 *
 * HARDCODED PARAMETERS
 * ====================
 * Reionization parameters are hardcoded in this header, not passed as module
 * parameters. This design choice enables easy model swapping since different
 * reionization models have different parameter sets.
 *
 * Current Model: Gnedin (2000) with Kravtsov et al. (2004) fitting formulas
 *
 * PHYSICS
 * =======
 * Calculates suppression factor for gas accretion onto low-mass halos after
 * cosmic reionization. The suppression depends on the ratio between halo mass
 * and a characteristic mass (maximum of filtering mass and mass corresponding
 * to virial temperature of 10^4 K).
 *
 * Three regimes based on scale factor:
 * 1. Before UV background turns on (a ≤ a0)
 * 2. During partial reionization (a0 < a < ar)
 * 3. After full reionization (a ≥ ar)
 *
 * References:
 *   - Gnedin (2000) - Reionization model
 *   - Kravtsov et al. (2004) - Fitting formulas (Appendix B)
 *   - Bryan & Norman (1998) - Critical overdensity
 *
 * Vision Principles:
 *   - Single Source of Truth: Reionization physics in ONE place
 *   - Runtime Modularity: Swap models without recompilation
 *   - Type Safety: Static inline for compile-time optimization
 */

#ifndef SHARED_REIONIZATION_H
#define SHARED_REIONIZATION_H

#include <math.h>

#include "module_interface.h"  /* For ModuleContext and Params */
#include "constants.h"
#include "numeric.h"

// ============================================================================
// REIONIZATION MODEL PARAMETERS (Gnedin 2000)
// ============================================================================

/* Enable/disable reionization suppression */
#define REIONIZATION_ON 1

/* Redshift when UV background turns on */
#define REIONIZATION_Z0 8.0

/* Redshift of full reionization */
#define REIONIZATION_ZR 7.0

/* Derived scale factors (calculated at compile time) */
#define REIONIZATION_A0 (1.0 / (1.0 + REIONIZATION_Z0))
#define REIONIZATION_AR (1.0 / (1.0 + REIONIZATION_ZR))

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

/* Gnedin (2000) model parameters */
#define REIONIZATION_ALPHA 6.0     /* Best fit to Gnedin data */
#define REIONIZATION_TVIR 1e4      /* Virial temperature threshold (K) */

/* Jeans mass calculation (Gnedin 2000, Bryan & Norman 1998) */
/* M_J = 25.0 * Omega^-0.5 * mu^-1.5, where mu^-1.5 = 2.21 for ionized gas */
#define MJEANS_BASE_COEFF 25.0        /* Jeans mass coefficient (1e10 Msun/h) */
#define IONIZED_GAS_MU_FACTOR 2.21    /* mu^-1.5 for fully ionized gas (mu=0.59) */
#define FILTERING_MASS_EXPONENT 1.5   /* M_filter exponent */

/* Characteristic mass and velocity */
#define TEMP_TO_VEL_COEFF 36.0        /* V_char from T_vir: V^2 = T/36 (km/s, K) */
#define HUBBLE_CONVERSION 100.0       /* H_0 = 100h km/s/Mpc */

/* Critical overdensity (Bryan & Norman 1998): δ_c = 18π² + 82x - 39x² */
#define DELTACRIT_COEFF_0 18.0
#define DELTACRIT_COEFF_1 82.0
#define DELTACRIT_COEFF_2 39.0
#define DELTACRIT_FACTOR 0.5          /* Factor in M_char calculation */

/* Reionization suppression (Kravtsov 2004 Appendix B) */
#define GNEDIN_SUPPRESSION_COEFF 0.26 /* Suppression strength */
#define GNEDIN_SUPPRESSION_POWER 3.0  /* Power-law exponent */

// ============================================================================
// REIONIZATION SUPPRESSION CALCULATION
// ============================================================================

/**
 * @brief   Calculate reionization suppression factor for gas accretion
 *
 * Implements the Gnedin (2000) reionization model with fitting formulas from
 * Kravtsov et al. (2004) Appendix B. After cosmic reionization, gas accretion
 * onto low-mass halos is suppressed due to increased gas temperature and
 * Jeans mass.
 *
 * The suppression depends on the ratio between the halo mass and a
 * characteristic mass (the maximum of the filtering mass and the mass
 * corresponding to a virial temperature of 10^4 K).
 *
 * @param   ctx          Module context (for accessing params->G)
 * @param   mvir         Virial mass of halo (1e10 Msun/h)
 * @param   redshift     Current redshift
 * @param   omega        Matter density parameter (Omega_m)
 * @param   omega_lambda Dark energy density parameter (Omega_Lambda)
 * @param   hubble_h     Hubble parameter (H_0 / 100 km/s/Mpc)
 * @return  Suppression modifier factor (0 to 1)
 *          0 = complete suppression, 1 = no suppression
 *
 * @note This function uses hardcoded reionization parameters defined at top
 *       of this header. To change reionization model, swap entire header file.
 */
static inline double calculate_reionization_modifier(const struct ModuleContext *ctx,
                                                       float mvir,
                                                       double redshift,
                                                       double omega,
                                                       double omega_lambda,
                                                       double hubble_h) {
  double a, a_on_a0, a_on_ar, f_of_a;
  double Mjeans, Mfiltering, Vchar, omegaZ, xZ, deltacritZ, HubbleZ;
  double G_code, Mchar, mass_to_use, modifier;

  /* Return 1.0 (no suppression) if reionization is disabled */
  if (!REIONIZATION_ON) {
    return 1.0;
  }

  /* Calculate scale factor and ratios */
  a = safe_div(1.0, 1.0 + redshift, 0.0);
  a_on_a0 = safe_div(a, REIONIZATION_A0, 0.0);
  a_on_ar = safe_div(a, REIONIZATION_AR, 0.0);

  /* Calculate f_of_a from Kravtsov et al. (2004) fitting formula */
  if (a <= REIONIZATION_A0) {
    /* Before UV background turns on */
    f_of_a = 3.0 * a /
             ((2.0 + REIONIZATION_ALPHA) * (5.0 + 2.0 * REIONIZATION_ALPHA)) *
             pow(a_on_a0, REIONIZATION_ALPHA);
  } else if (a < REIONIZATION_AR) {
    /* During partial reionization */
    f_of_a = safe_div(3.0, a, 0.0) * REIONIZATION_A0 * REIONIZATION_A0 *
                 (1.0 / (2.0 + REIONIZATION_ALPHA) -
                  2.0 * pow(a_on_a0, -0.5) /
                      (5.0 + 2.0 * REIONIZATION_ALPHA)) +
             a * a / 10.0 -
             (REIONIZATION_A0 * REIONIZATION_A0 / 10.0) *
                 (5.0 - 4.0 * pow(a_on_a0, -0.5));
  } else {
    /* After full reionization */
    f_of_a =
        safe_div(3.0, a, 0.0) *
            (REIONIZATION_A0 * REIONIZATION_A0 *
                 (1.0 / (2.0 + REIONIZATION_ALPHA) -
                  2.0 * pow(a_on_a0, -0.5) /
                      (5.0 + 2.0 * REIONIZATION_ALPHA)) +
             (REIONIZATION_AR * REIONIZATION_AR / 10.0) *
                 (5.0 - 4.0 * pow(a_on_ar, -0.5)) -
             (REIONIZATION_A0 * REIONIZATION_A0 / 10.0) *
                 (5.0 - 4.0 * pow(a_on_a0, -0.5)) +
             a * REIONIZATION_AR / 3.0 -
             (REIONIZATION_AR * REIONIZATION_AR / 3.0) *
                 (3.0 - 2.0 * pow(a_on_ar, -0.5)));
  }

  /* Calculate filtering mass (in units of 1e10 Msun/h) */
  /* Jeans mass: M_J = 25 * Omega^-0.5 * mu^-1.5
   * For fully ionized gas: mu=0.59, so mu^-1.5 = 2.21 */
  Mjeans = MJEANS_BASE_COEFF * pow(omega, -0.5) * IONIZED_GAS_MU_FACTOR;
  Mfiltering = Mjeans * pow(f_of_a, FILTERING_MASS_EXPONENT);

  /* Calculate characteristic mass from virial temperature of 10^4 K */
  Vchar = sqrt(REIONIZATION_TVIR / TEMP_TO_VEL_COEFF);

  /* Cosmological parameters at current redshift */
  omegaZ = omega * pow(1.0 + redshift, 3.0) /
           (omega * pow(1.0 + redshift, 3.0) + omega_lambda + EPSILON_SMALL);
  xZ = omegaZ - 1.0;
  deltacritZ = DELTACRIT_COEFF_0 * M_PI * M_PI + DELTACRIT_COEFF_1 * xZ -
               DELTACRIT_COEFF_2 * xZ * xZ;

  /* Hubble parameter at redshift z (in km/s/Mpc) */
  HubbleZ = HUBBLE_CONVERSION * hubble_h *
            sqrt(omega * pow(1.0 + redshift, 3.0) + omega_lambda);

  /* Use pre-computed G in code units from params (Mpc/h)(km/s)^2/(1e10 Msun/h) */
  /* Value is ~43.0071 - see docs/developer/unit-system-guide.md */
  G_code = ctx->params->G;

  /* Calculate characteristic mass */
  Mchar = Vchar * Vchar * Vchar /
          (G_code * HubbleZ * sqrt(DELTACRIT_FACTOR * deltacritZ) +
           EPSILON_SMALL);

  /* Use maximum of filtering mass and characteristic mass */
  mass_to_use = fmax(Mfiltering, Mchar);

  /* Calculate suppression modifier using Gnedin (2000) fitting formula
   * Coefficient 0.26 from Kravtsov et al. (2004) Appendix B */
  modifier = 1.0 / pow(1.0 + GNEDIN_SUPPRESSION_COEFF * mass_to_use /
                                 (mvir + EPSILON_SMALL),
                       GNEDIN_SUPPRESSION_POWER);

  return modifier;
}

#endif /* SHARED_REIONIZATION_H */
