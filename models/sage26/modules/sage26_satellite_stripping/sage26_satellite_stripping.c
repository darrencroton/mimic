/**
 * @file    sage26_satellite_stripping.c
 * @brief   SAGE26 satellite stripping module - environmental gas stripping from satellites
 *
 * Ports SAGE26's strip_from_satellite (model_infall.c:225-273) on the R0 path
 * (CGMrecipeOn 0): each substep a live Type 1 satellite gives the central's hot
 * reservoir 1/N of its baryon excess over HaloBaryonFraction * Mvir, taken from its
 * own hot gas. HaloBaryonFraction is the satellite's own once-rounded
 * reionization_modifier * BaryonFrac, carried from sage26_reionization.
 *
 * Every floating-point expression is transcribed from SAGE26 with its operand
 * order and types, so that compiler contraction treats both alike.
 *
 * Reference: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2006, 2016)
 */

#include "error.h"
#include "module_system/parameter_helpers.h"
#include "shared/metallicity.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double GLOBAL_BARYON_FRAC;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_satellite_stripping_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  VERBOSE_LOG("SAGE satellite stripping module initialized");
  VERBOSE_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);

  return 0;
}

int sage26_satellite_stripping_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  // SAGE parity: stripping runs process_by_galaxy so it interleaves with
  // cooling in the galaxy-major loop, exactly as SAGE26 strips a Type 1
  // satellite just before that satellite cools (core_build_model.c:493-509).
  // Because the FOF central is normally processed earlier in the galaxy loop,
  // it has already cooled before satellites strip, so stripped gas reaches the
  // central's hot reservoir and cools on the next substep (not the current one).
  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  // Every live Type 1 satellite is stripped, whatever its hot gas
  // (core_build_model.c:493-498)
  if (halo->galaxy == NULL || halo->Type != 1) {
    return 0;
  }

  if (ctx == NULL || ctx->central_galaxy == NULL || ctx->central_galaxy->galaxy == NULL) {
    DEBUG_LOG("No FOF central available for stripping target");
    return 0;
  }

  struct GalaxyData *sat_gal = halo->galaxy;
  struct GalaxyData *cen_gal = ctx->central_galaxy->galaxy;

  /* The satellite's evaluated reionization_modifier * BaryonFrac, used as SAGE26
   * uses it even when zero (model_infall.c:230-244). Only the -1.0 sentinel
   * (sage26_reionization not configured: SAGE26 ReionizationOn 0, modifier 1.0)
   * selects the global fraction. */
  const double halo_baryon_frac =
      (sat_gal->HaloBaryonFraction == -1.0) ? GLOBAL_BARYON_FRAC : sat_gal->HaloBaryonFraction;

  /* SAGE parity: SAGE26's baryon sum is float arithmetic, its CGM term included,
   * rounded once to double on assignment (model_infall.c:236-241). CGMgas is zero
   * for satellites when the CGM recipe is off. */
  const double satBaryons = sat_gal->StellarMass + sat_gal->ColdGas + sat_gal->HotGas +
                            sat_gal->CGMgas + sat_gal->EjectedGas + sat_gal->BlackHoleMass +
                            sat_gal->ICS;

  // Mvir is read as the float SAGE26 holds (core_allvars.h:129)
  const float sat_Mvir = (float)halo->Mvir;

  /* SAGE parity: divides the recomputed excess by num_substeps every substep
   * (model_infall.c:238-244), not a fixed budget partitioned once like
   * sage26_apply_infall. The fraction stripped over one interval is therefore
   * 1-(1-1/N)^N: 100% at N=1, converging to ~63% (1-1/e) as N grows. SAGE26's
   * nearby comment (core_build_model.c:494-497) says the per-snapshot fraction
   * does not depend on N; this recurrence is what its code implements, so it
   * is mirrored. The "* Mvir - satBaryons" stays one expression so it contracts
   * to the single fused multiply-add SAGE26's build performs. */
  double strippedGas = -1.0 * (halo_baryon_frac * sat_Mvir - satBaryons) / ctx->num_substeps;

  if (strippedGas > 0.0) {
    // CGMrecipeOn != 1: both the satellite's source and the central's target
    // reservoir are HotGas (model_infall.c:250-254)
    const double metallicity = mimic_get_metallicity(sat_gal->HotGas, sat_gal->MetalsHotGas);
    double strippedGasMetals = strippedGas * metallicity;

    if (strippedGas > sat_gal->HotGas)
      strippedGas = sat_gal->HotGas;
    if (strippedGasMetals > sat_gal->MetalsHotGas)
      strippedGasMetals = sat_gal->MetalsHotGas;

    sat_gal->HotGas -= strippedGas;
    sat_gal->MetalsHotGas -= strippedGasMetals;

    /* SAGE parity: asymmetric metal transfer (model_infall.c:256-270). The
     * satellite is debited the unclamped-gas metals capped at its own metals,
     * but the central is credited the clamped gas times the original
     * metallicity. When the gas clamp engages with metals above gas (Z capped
     * at 1), the two differ and metals are not conserved. This is SAGE26's
     * deliberate source quirk, mirrored exactly. */
    cen_gal->HotGas += strippedGas;
    cen_gal->MetalsHotGas += strippedGas * metallicity;
  }

  return 0;
}

int sage26_satellite_stripping_cleanup(void) {
  VERBOSE_LOG("SAGE satellite stripping module cleaned up");
  return 0;
}
