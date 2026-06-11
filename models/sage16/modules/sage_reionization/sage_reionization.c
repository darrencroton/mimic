/**
 * @file    sage_reionization.c
 * @brief   SAGE reionization module - suppression of gas accretion onto low-mass halos
 *
 * Calculates reionization suppression factor using Gnedin (2000) filtering mass model
 * with fitting formulas from Kravtsov et al. (2004). Sets HaloBaryonFraction property
 * used by infall and stripping modules.
 *
 * Reference: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double GLOBAL_BARYON_FRAC;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

// Calculate reionization suppression factor for gas accretion.
// Implements Gnedin (2000) model with Kravtsov et al. (2004) fitting formulas.
static double calculate_reionization_modifier(const struct ModuleContext *ctx, float mvir) {
  // Alpha gives best fit to Gnedin data, Tvir = 10^4 K is virial temperature threshold
  const double alpha = 6.0;
  const double Tvir = 1e4;

  const double redshift = ctx->redshift;
  const double omega = ctx->params->Omega;
  const double omega_lambda = ctx->params->OmegaLambda;

  // Calculate filtering mass using Kravtsov et al. (2004) fitting formula
  const double a = 1.0 / (1.0 + redshift);
  const double a0 = 1.0 / (1.0 + 8.0); // z0 = 8.0 (UV background turns on)
  const double ar = 1.0 / (1.0 + 7.0); // zr = 7.0 (full reionization)
  const double a_on_a0 = a / a0;
  const double a_on_ar = a / ar;

  double f_of_a;
  if (a <= a0) {
    f_of_a = 3.0 * a / ((2.0 + alpha) * (5.0 + 2.0 * alpha)) * pow(a_on_a0, alpha);
  } else if (a < ar) {
    f_of_a =
        (3.0 / a) * a0 * a0 * (1.0 / (2.0 + alpha) - 2.0 / sqrt(a_on_a0) / (5.0 + 2.0 * alpha)) +
        a * a / 10.0 - (a0 * a0 / 10.0) * (5.0 - 4.0 / sqrt(a_on_a0));
  } else {
    f_of_a =
        (3.0 / a) * (a0 * a0 * (1.0 / (2.0 + alpha) - 2.0 / sqrt(a_on_a0) / (5.0 + 2.0 * alpha)) +
                     (ar * ar / 10.0) * (5.0 - 4.0 / sqrt(a_on_ar)) -
                     (a0 * a0 / 10.0) * (5.0 - 4.0 / sqrt(a_on_a0)) + a * ar / 3.0 -
                     (ar * ar / 3.0) * (3.0 - 2.0 / sqrt(a_on_ar)));
  }

  // Jeans mass in units of 1e10 Msun/h. Note mu=0.59 for fully ionized gas, mu^-1.5 = 2.21
  const double Mjeans = 25.0 / sqrt(omega) * 2.21;
  const double Mfiltering = Mjeans * pow(f_of_a, 1.5);

  // Calculate characteristic mass corresponding to halo with Tvir = 10^4 K
  const double Vchar = sqrt(Tvir / 36.0); // V^2 = T/36 in code units
  const double omegaZ =
      omega * (1.0 + redshift) * (1.0 + redshift) * (1.0 + redshift) /
      (omega * (1.0 + redshift) * (1.0 + redshift) * (1.0 + redshift) + omega_lambda);
  const double xZ = omegaZ - 1.0;
  const double deltacritZ =
      18.0 * M_PI * M_PI + 82.0 * xZ - 39.0 * xZ * xZ; // Bryan & Norman (1998)
  /* H(z) in code units. Lengths are Mpc/h, so H0 in code units is ~100 km/s/(Mpc/h)
   * with NO factor of h (SAGE parity: run_params->Hubble = HUBBLE * UnitTime_in_s).
   * Using 100*h here would inflate the characteristic mass Mchar by 1/h (~37%). */
  const double HubbleZ =
      ctx->params->Hubble *
      sqrt(omega * (1.0 + redshift) * (1.0 + redshift) * (1.0 + redshift) + omega_lambda);

  const double G_code = ctx->params->G; // G in code units
  const double Mchar = Vchar * Vchar * Vchar / (G_code * HubbleZ * sqrt(0.5 * deltacritZ));

  // Use maximum of filtering mass and characteristic mass
  const double mass_to_use = fmax(Mfiltering, Mchar);
  const double modifier =
      1.0 / ((1.0 + 0.26 * mass_to_use / mvir) * (1.0 + 0.26 * mass_to_use / mvir) *
             (1.0 + 0.26 * mass_to_use / mvir));

  return modifier;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_reionization_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  INFO_LOG("SAGE reionization module initialized");
  VERBOSE_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);
  VERBOSE_LOG("  Physics: HaloBaryonFraction = GlobalBaryonFraction * f_reion(Mvir, z)");

  return 0;
}

int sage_reionization_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL || halos[i].Type == 3) {
      continue;
    }

    if (halos[i].Mvir > EPSILON_SMALL) {
      // Centrals and satellites alike use their own Mvir for the modifier.
      const double reionization_modifier = calculate_reionization_modifier(ctx, halos[i].Mvir);
      halos[i].galaxy->HaloBaryonFraction = GLOBAL_BARYON_FRAC * reionization_modifier;

      DEBUG_LOG("Halo %d (Type=%d): Mvir=%.3e, f_reion=%.4f, HaloBaryonFraction=%.4f, z=%.3f", i,
                halos[i].Type, halos[i].Mvir, reionization_modifier,
                halos[i].galaxy->HaloBaryonFraction, ctx->redshift);
    } else {
      halos[i].galaxy->HaloBaryonFraction = 0.0;
    }
  }

  return 0;
}

int sage_reionization_cleanup(void) {
  VERBOSE_LOG("SAGE reionization module cleaned up");
  return 0;
}
