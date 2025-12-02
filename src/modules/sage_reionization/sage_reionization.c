/**
 * @file    sage_reionization.c
 * @brief   SAGE reionization suppression module implementation
 *
 * This module calculates halo-specific baryon fractions modified by
 * reionization suppression. It implements the Gnedin (2000) reionization
 * model with fitting formulas from Kravtsov et al. (2004).
 *
 * Physics:
 *   HaloBaryonFraction = GlobalBaryonFraction * f_reion(Mvir, z)
 *
 * After cosmic reionization, gas accretion onto low-mass halos is suppressed
 * due to increased gas temperature and Jeans mass. The suppression depends on
 * the ratio between halo mass and a characteristic mass.
 *
 * Three regimes based on scale factor:
 * 1. Before UV background turns on (a ≤ a0): Partial suppression
 * 2. During partial reionization (a0 < a < ar): Increasing suppression
 * 3. After full reionization (a ≥ ar): Full suppression effect
 *
 * Implementation Notes:
 * - This module MUST run before sage_infall and sage_satellite_stripping
 * - Sets HaloBaryonFraction property for each halo
 * - Reionization parameters are hardcoded (model-specific)
 * - To use different reionization model, create new module
 *
 * Reference:
 *   - Gnedin (2000) - Reionization model
 *   - Kravtsov et al. (2004) - Filtering mass formulas (Appendix B)
 *   - Bryan & Norman (1998) - Critical overdensity
 *   - Croton et al. (2016) - SAGE model description
 *
 * Vision Principles:
 *   - Single Source of Truth: HaloBaryonFraction property
 *   - Runtime Modularity: Configurable via parameter file
 *   - Physics-Agnostic Core: Interacts only through module interface
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
// Parameters loaded from input YAML file (required, no defaults).

static double GLOBAL_BARYON_FRAC;

// ============================================================================
// REIONIZATION MODEL PARAMETERS (Gnedin 2000)
// ============================================================================
// Hardcoded parameters specific to Gnedin (2000) model.
// To use different reionization model, create new module with different parameters.

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
// HELPER FUNCTIONS (Physics Calculations)
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

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize sage_reionization module
 *
 * Loads configuration parameters from input YAML file and validates them.
 * All parameters are REQUIRED in the input file (no defaults).
 *
 * @return  0 on success, non-zero on failure
 */
static int sage_reionization_init(void) {
  /* Load and validate parameters from input YAML file */
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  /* Log module configuration */
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
 * Calculates HaloBaryonFraction for each halo based on global baryon fraction
 * and reionization suppression.
 *
 * Process:
 * 1. For each halo, calculate reionization modifier f_reion(Mvir, z)
 * 2. Set HaloBaryonFraction = GlobalBaryonFraction * f_reion
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
static int sage_reionization_process(struct ModuleContext *ctx,
                                       struct Halo *halos,
                                       int ngal) {
  /* Validate inputs */
  if (halos == NULL || ngal <= 0) {
    return 0; /* Nothing to process */
  }

  /* Extract cosmological parameters from context */
  double z = ctx->redshift;
  double omega = ctx->params->Omega;
  double omega_lambda = ctx->params->OmegaLambda;
  double hubble_h = ctx->params->Hubble_h;

  /* Process each halo */
  for (int i = 0; i < ngal; i++) {
    /* Validate halo has galaxy data */
    if (halos[i].galaxy == NULL) {
      ERROR_LOG("Halo %d has NULL galaxy data", i);
      return -1;
    }

    /* Calculate reionization modifier for this halo */
    double reionization_modifier = calculate_reionization_modifier(
        ctx, halos[i].Mvir, z, omega, omega_lambda, hubble_h);

    /* Set halo-specific baryon fraction */
    halos[i].galaxy->HaloBaryonFraction =
        (float)(GLOBAL_BARYON_FRAC * reionization_modifier);

    /* Debug logging for central galaxies only */
    if (halos[i].Type == 0) {
      DEBUG_LOG("Halo %d (Type=0): Mvir=%.3e, f_reion=%.4f, HaloBaryonFraction=%.4f, z=%.3f",
                i, halos[i].Mvir, reionization_modifier,
                halos[i].galaxy->HaloBaryonFraction, z);
    }
  }

  return 0;
}

/**
 * @brief   Cleanup sage_reionization module
 *
 * No allocated resources to free for this module.
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

/**
 * @brief   Module structure for sage_reionization module
 */
static struct Module sage_reionization_module = {
    .name = "sage_reionization",
    .init = sage_reionization_init,
    .process_halos = sage_reionization_process,
    .cleanup = sage_reionization_cleanup};

/**
 * @brief   Register the sage_reionization module
 */
void sage_reionization_register(void) {
  module_registry_add(&sage_reionization_module);
}
