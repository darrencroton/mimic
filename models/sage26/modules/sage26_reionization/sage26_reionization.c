/**
 * @file    sage26_reionization.c
 * @brief   SAGE26 reionization module - suppression of gas accretion onto low-mass halos
 *
 * Calculates the reionization suppression factor of SAGE26's do_reionization
 * (model_infall.c:286-337): the Gnedin (2000) filtering-mass model with the fitting
 * formulas of Kravtsov et al. (2004) Appendix B. Each galaxy's HaloBaryonFraction is
 * set to the once-rounded product modifier * GlobalBaryonFraction, the leading
 * product SAGE26 forms in infall_recipe (model_infall.c:128-130) and
 * strip_from_satellite (model_infall.c:243-244); those modules finish the
 * expression with the halo's Mvir.
 *
 * Every floating-point expression is transcribed from SAGE26 with its operand
 * order, grouping and types (pow(..., -0.5) and / sqrt(...) where SAGE26 writes
 * each, the CUBE grouping, dmax), so that compiler contraction treats both alike.
 *
 * Reference: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "types.h"

/** SAGE26 macros.h:55, expanded textually exactly as SAGE26 expands it */
#define SAGE26_CUBE(X) ((X) * (X) * (X))

// ============================================================================
// MODULE PARAMETERS AND CONSTANTS
// ============================================================================

static double GLOBAL_BARYON_FRAC;

/* Reionization redshifts of the R0 configuration (SAGE26 Reionization_z0 = 8,
 * Reionization_zr = 7). SAGE26 derives a0 and ar at run time
 * (core_init.c:70-71); they are derived the same way in init(). */
static const double REIONIZATION_Z0 = 8.0;
static const double REIONIZATION_ZR = 7.0;
static double REIONIZATION_A0;
static double REIONIZATION_AR;

/* SAGE26 model_infall.c:36-55, same names and values */
static const double GNEDIN_FILTER_COEFF = 0.26;
static const double KRAVTSOV04_MJEANS_0 = 25.0;
static const double KRAVTSOV04_MU_NEG1PT5 = 2.21;
static const double BN98_DELTA_LIN = 82.0;
static const double BN98_DELTA_QUAD = 39.0;
static const double TVIR_VCHAR_COEFF = 36.0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief SAGE26 dmax (model_misc.c:440-442): comparison-based maximum
 *
 * SAGE parity: SAGE26 uses this comparison rather than fmax, so a NaN in the
 * first argument selects the second exactly as SAGE26 does (model_misc.c:440-442).
 */
static inline double sage26_dmax(const double x, const double y) { return (x > y) ? x : y; }

/**
 * @brief SAGE26 do_reionization (model_infall.c:286-337) for one galaxy
 *
 * @param Zcurr  Redshift of the call (SAGE26 ZZ[halo_snapnum]; Mimic ctx->redshift)
 * @param Mvir   The galaxy's virial mass as SAGE26 holds it, a float
 * @return       The suppression factor 1 / (1 + 0.26 M / Mvir)^3; Mvir = 0 gives 0
 */
static double do_reionization(const struct ModuleContext *ctx, const double Zcurr,
                              const float Mvir) {
  double f_of_a;

  // Two parameters Kravtsov et al. keep fixed; alpha gives the best fit to the Gnedin data
  const double alpha = 6.0;
  const double Tvir = 1e4;

  const double a0 = REIONIZATION_A0;
  const double ar = REIONIZATION_AR;

  // Filtering mass (SAGE26 model_infall.c:298-315)
  const double a = 1.0 / (1.0 + Zcurr);
  const double a_on_a0 = a / a0;
  const double a_on_ar = a / ar;

  if (a <= a0) {
    f_of_a = 3.0 * a / ((2.0 + alpha) * (5.0 + 2.0 * alpha)) * pow(a_on_a0, alpha);
  } else if ((a > a0) && (a < ar)) {
    f_of_a = (3.0 / a) * a0 * a0 *
                 (1.0 / (2.0 + alpha) - 2.0 * pow(a_on_a0, -0.5) / (5.0 + 2.0 * alpha)) +
             a * a / 10.0 - (a0 * a0 / 10.0) * (5.0 - 4.0 * pow(a_on_a0, -0.5));
  } else {
    // SAGE parity: SAGE26 mixes pow(x, -0.5) and 1 / sqrt(x) within this one
    // expression (model_infall.c:309-314); each form is kept where SAGE26 writes it.
    f_of_a = (3.0 / a) *
             (a0 * a0 * (1.0 / (2.0 + alpha) - 2.0 * pow(a_on_a0, -0.5) / (5.0 + 2.0 * alpha)) +
              (ar * ar / 10.0) * (5.0 - 4.0 * pow(a_on_ar, -0.5)) -
              (a0 * a0 / 10.0) * (5.0 - 4.0 / sqrt(a_on_a0)) + a * ar / 3.0 -
              (ar * ar / 3.0) * (3.0 - 2.0 / sqrt(a_on_ar)));
  }

  // Jeans mass in units of 1e10 Msun/h (Kravtsov et al. 2004 Appendix B; mu^-1.5 = 2.21)
  const double Mjeans = KRAVTSOV04_MJEANS_0 / sqrt(ctx->params->Omega) * KRAVTSOV04_MU_NEG1PT5;
  const double Mfiltering = Mjeans * pow(f_of_a, 1.5);

  // Characteristic mass of a halo with a virial temperature of 1e4 K
  const double Vchar = sqrt(Tvir / TVIR_VCHAR_COEFF);
  const double omegaZ = ctx->params->Omega *
                        (SAGE26_CUBE(1.0 + Zcurr) / (ctx->params->Omega * SAGE26_CUBE(1.0 + Zcurr) +
                                                     ctx->params->OmegaLambda));
  const double xZ = omegaZ - 1.0;
  // Bryan & Norman (1998) overdensity criterion
  const double deltacritZ = 18.0 * M_PI * M_PI + BN98_DELTA_LIN * xZ - BN98_DELTA_QUAD * xZ * xZ;
  /* SAGE parity: Hubble is H0 in code units with no factor of h, as SAGE26 sets
   * it (run_params->Hubble = HUBBLE * UnitTime_in_s, core_init.c:106); using
   * 100*h here would inflate Mchar by 1/h. */
  const double HubbleZ = ctx->params->Hubble * sqrt(ctx->params->Omega * SAGE26_CUBE(1.0 + Zcurr) +
                                                    ctx->params->OmegaLambda);

  const double Mchar = Vchar * Vchar * Vchar / (ctx->params->G * HubbleZ * sqrt(0.5 * deltacritZ));

  // The larger of the two masses (Gnedin 2000 / Kravtsov et al. 2004)
  const double mass_to_use = sage26_dmax(Mfiltering, Mchar);
  const double modifier = 1.0 / SAGE26_CUBE(1.0 + GNEDIN_FILTER_COEFF * (mass_to_use / Mvir));

  return modifier;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_reionization_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  /* SAGE26 core_init.c:70-71 */
  REIONIZATION_A0 = 1.0 / (1.0 + REIONIZATION_Z0);
  REIONIZATION_AR = 1.0 / (1.0 + REIONIZATION_ZR);

  /* Ordering: reionization writes HaloBaryonFraction, so it must run before any same-phase
     consumer. sage26_prepare_infall_budget is the pre_timestep consumer; later-phase consumers
     (e.g. sage26_satellite_stripping) are structurally guaranteed to run after pre_timestep. */
  if (module_configured_in_phase("sage26_prepare_infall_budget", MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO) &&
      !module_precedes_in_phase("sage26_reionization", "sage26_prepare_infall_budget",
                                MimicConfig.pre_timestep, MimicConfig.num_pre_timestep)) {
    ERROR_LOG("sage26_reionization must run before sage26_prepare_infall_budget in pre_timestep — "
              "it sets HaloBaryonFraction that the infall budget reads");
    return -1;
  }

  VERBOSE_LOG("SAGE reionization module initialized");
  VERBOSE_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);
  VERBOSE_LOG("  Physics: HaloBaryonFraction = GlobalBaryonFraction * f_reion(Mvir, z)");

  return 0;
}

int sage26_reionization_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL || halos[i].Type == 3) {
      continue;
    }

    /* Centrals and satellites alike use their own Mvir, as SAGE26 passes the
     * galaxy itself (model_infall.c:123, :231). SAGE26 evaluates the formula
     * for every Mvir with no gate: Mvir = 0 (an orphan) gives 1 / inf = 0. Mvir
     * is read as the float SAGE26 holds (core_allvars.h:129). */
    const double reionization_modifier = do_reionization(ctx, ctx->redshift, (float)halos[i].Mvir);
    halos[i].galaxy->HaloBaryonFraction = reionization_modifier * GLOBAL_BARYON_FRAC;

    DEBUG_LOG("Halo %d (Type=%d): Mvir=%.3e, f_reion=%.4f, HaloBaryonFraction=%.4f, z=%.3f", i,
              halos[i].Type, halos[i].Mvir, reionization_modifier,
              halos[i].galaxy->HaloBaryonFraction, ctx->redshift);
  }

  return 0;
}

int sage26_reionization_cleanup(void) {
  VERBOSE_LOG("SAGE reionization module cleaned up");
  return 0;
}
