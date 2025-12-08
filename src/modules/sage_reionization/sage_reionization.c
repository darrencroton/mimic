/**
 * @file    sage_reionization.c
 * @brief   SAGE reionization suppression module implementation
 *
 * Calculates halo-specific baryon fractions modified by reionization suppression
 * following the Gnedin (2000) model. After cosmic reionization, gas accretion onto
 * low-mass halos is suppressed due to increased gas temperature and Jeans mass.
 *
 * Physics: HaloBaryonFraction = GlobalBaryonFraction × f_reion(Mvir, z)
 *
 * Three regimes based on scale factor:
 * 1. Before UV background turns on (a ≤ a0): Partial suppression
 * 2. During partial reionization (a0 < a < ar): Increasing suppression
 * 3. After full reionization (a ≥ ar): Full suppression effect
 *
 * Key functions:
 * - calculate_reionization_modifier(): Compute f_reion(Mvir, z) using Gnedin (2000) model
 *
 * Reference: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2016)
 */

#include <math.h>
#include <stdio.h>   /* Required for error.h logging macros */
#include <stdlib.h>  /* Required for error.h logging macros */

#include "constants.h"
#include "error.h"
#include "../_system/parameter_helpers.h"  // Parameter loading and validation macros
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_reionization.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double GLOBAL_BARYON_FRAC;

// ============================================================================
// REIONIZATION MODEL PARAMETERS (Gnedin 2000)
// ============================================================================
// Hardcoded parameters specific to Gnedin (2000) model.

/* Enable/disable reionization suppression */
#define REIONIZATION_ON 1

/* Redshift when UV background turns on */
#define REIONIZATION_Z0 8.0

/* Redshift of full reionization */
#define REIONIZATION_ZR 7.0

/* Derived scale factors (calculated at compile time) */
#define REIONIZATION_A0 (1.0 / (1.0 + REIONIZATION_Z0))
#define REIONIZATION_AR (1.0 / (1.0 + REIONIZATION_ZR))

/* Gnedin (2000) model parameters */
#define REIONIZATION_ALPHA 6.0     /* Best fit to Gnedin data */
#define REIONIZATION_TVIR 1e4      /* Virial temperature threshold (K) */

/* Jeans mass: M_J = 25.0 * Omega^-0.5 * mu^-1.5 */
#define MJEANS_BASE_COEFF 25.0        /* Jeans mass coefficient (1e10 Msun/h) */
#define IONIZED_GAS_MU_FACTOR 2.21    /* mu^-1.5 for ionized gas (mu=0.59) */
#define FILTERING_MASS_EXPONENT 1.5   /* M_filter exponent */

#define TEMP_TO_VEL_COEFF 36.0        /* V_char from T_vir: V^2 = T/36 */
#define HUBBLE_CONVERSION 100.0       /* H_0 = 100h km/s/Mpc */

/* Critical overdensity (Bryan & Norman 1998) */
#define DELTACRIT_COEFF_0 18.0
#define DELTACRIT_COEFF_1 82.0
#define DELTACRIT_COEFF_2 39.0
#define DELTACRIT_FACTOR 0.5

/* Suppression strength (Kravtsov 2004 Appendix B) */
#define GNEDIN_SUPPRESSION_COEFF 0.26
#define GNEDIN_SUPPRESSION_POWER 3.0

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief   Calculate reionization suppression factor for gas accretion
 *
 * Implements Gnedin (2000) model with Kravtsov et al. (2004) fitting formulas.
 * Suppression depends on ratio between halo mass and characteristic mass (maximum
 * of filtering mass and mass corresponding to Tvir = 10^4 K).
 *
 * @param   ctx          Module context
 * @param   mvir         Virial mass of halo (1e10 Msun/h)
 * @param   redshift     Current redshift
 * @param   omega        Matter density parameter
 * @param   omega_lambda Dark energy density parameter
 * @param   hubble_h     Hubble parameter (H_0 / 100 km/s/Mpc)
 * @return  Suppression factor (0 = complete suppression, 1 = no suppression)
 */
static double calculate_reionization_modifier(const struct ModuleContext *ctx,
                                                float mvir,
                                                double redshift,
                                                double omega,
                                                double omega_lambda,
                                                double hubble_h) {
  double a, a_on_a0, a_on_ar, f_of_a;
  double Mjeans, Mfiltering, Vchar, omegaZ, xZ, deltacritZ, HubbleZ;
  double G_code, Mchar, mass_to_use, modifier;

  if (!REIONIZATION_ON) {
    return 1.0;
  }

  a = safe_div(1.0, 1.0 + redshift, 0.0);
  a_on_a0 = safe_div(a, REIONIZATION_A0, 0.0);
  a_on_ar = safe_div(a, REIONIZATION_AR, 0.0);

  /* Calculate f_of_a from Kravtsov et al. (2004) fitting formula */
  if (a <= REIONIZATION_A0) {
    f_of_a = 3.0 * a /
             ((2.0 + REIONIZATION_ALPHA) * (5.0 + 2.0 * REIONIZATION_ALPHA)) *
             pow(a_on_a0, REIONIZATION_ALPHA);
  } else if (a < REIONIZATION_AR) {
    f_of_a = safe_div(3.0, a, 0.0) * REIONIZATION_A0 * REIONIZATION_A0 *
                 (1.0 / (2.0 + REIONIZATION_ALPHA) -
                  2.0 * pow(a_on_a0, -0.5) /
                      (5.0 + 2.0 * REIONIZATION_ALPHA)) +
             a * a / 10.0 -
             (REIONIZATION_A0 * REIONIZATION_A0 / 10.0) *
                 (5.0 - 4.0 * pow(a_on_a0, -0.5));
  } else {
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

  /* Calculate filtering mass */
  Mjeans = MJEANS_BASE_COEFF * pow(omega, -0.5) * IONIZED_GAS_MU_FACTOR;
  Mfiltering = Mjeans * pow(f_of_a, FILTERING_MASS_EXPONENT);

  /* Calculate characteristic mass from Tvir = 10^4 K */
  Vchar = sqrt(REIONIZATION_TVIR / TEMP_TO_VEL_COEFF);

  omegaZ = omega * pow(1.0 + redshift, 3.0) /
           (omega * pow(1.0 + redshift, 3.0) + omega_lambda + EPSILON_SMALL);
  xZ = omegaZ - 1.0;
  deltacritZ = DELTACRIT_COEFF_0 * M_PI * M_PI + DELTACRIT_COEFF_1 * xZ -
               DELTACRIT_COEFF_2 * xZ * xZ;

  HubbleZ = HUBBLE_CONVERSION * hubble_h *
            sqrt(omega * pow(1.0 + redshift, 3.0) + omega_lambda);

  G_code = ctx->params->G;  /* Pre-computed G in code units */

  Mchar = Vchar * Vchar * Vchar /
          (G_code * HubbleZ * sqrt(DELTACRIT_FACTOR * deltacritZ) +
           EPSILON_SMALL);

  mass_to_use = fmax(Mfiltering, Mchar);

  modifier = 1.0 / pow(1.0 + GNEDIN_SUPPRESSION_COEFF * mass_to_use /
                                 (mvir + EPSILON_SMALL),
                       GNEDIN_SUPPRESSION_POWER);

  return modifier;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize sage_reionization module
 *
 * @return  0 on success, non-zero on failure
 */
static int sage_reionization_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  INFO_LOG("SAGE reionization module initialized");
  INFO_LOG("  Physics: HaloBaryonFraction = GlobalBaryonFraction * f_reion(Mvir, z)");
  INFO_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);
  INFO_LOG("  Reionization model: Gnedin (2000)");
  INFO_LOG("    z0 = %.1f (UV background turns on)", REIONIZATION_Z0);
  INFO_LOG("    zr = %.1f (full reionization)", REIONIZATION_ZR);
  INFO_LOG("    alpha = %.1f (suppression strength)", REIONIZATION_ALPHA);

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Calculates HaloBaryonFraction for each halo from GlobalBaryonFraction and
 * reionization suppression factor.
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
static int sage_reionization_process(struct ModuleContext *ctx,
                                       struct Halo *halos,
                                       int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  double z = ctx->redshift;
  double omega = ctx->params->Omega;
  double omega_lambda = ctx->params->OmegaLambda;
  double hubble_h = ctx->params->Hubble_h;

  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL) {
      ERROR_LOG("Halo %d has NULL galaxy data", i);
      return -1;
    }

    if (halos[i].Mvir > EPSILON_SMALL) {
      double reionization_modifier = calculate_reionization_modifier(
          ctx, halos[i].Mvir, z, omega, omega_lambda, hubble_h);
  
      halos[i].galaxy->HaloBaryonFraction =
      (float)(GLOBAL_BARYON_FRAC * reionization_modifier);
  
      if (halos[i].Type == 0) {
        DEBUG_LOG("Halo %d (Type=0): Mvir=%.3e, f_reion=%.4f, HaloBaryonFraction=%.4f, z=%.3f",
                  i, halos[i].Mvir, reionization_modifier,
                  halos[i].galaxy->HaloBaryonFraction, z);
                }
    } else {
      halos[i].galaxy->HaloBaryonFraction = 0.0;
    }
  }

  return 0;
}

/**
 * @brief   Cleanup sage_reionization module
 *
 * @return  0 on success
 */
static int sage_reionization_cleanup(void) {
  INFO_LOG("SAGE reionization module cleaned up");
  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

static struct Module sage_reionization_module = {
    .name = "sage_reionization",
    .init = sage_reionization_init,
    .process_halos = sage_reionization_process,
    .cleanup = sage_reionization_cleanup};

void sage_reionization_register(void) {
  module_registry_add(&sage_reionization_module);
}
