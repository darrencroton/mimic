/**
 * @file    sage26_satellite_stripping.c
 * @brief   SAGE26 satellite stripping module - environmental gas stripping from satellites
 *
 * Ports SAGE26's strip_from_satellite (R = the staged SAGE26 source, R/src/model_infall.c:225-273):
 * each substep a live Type 1 satellite gives the FoF central 1/N of its baryon excess over
 * HaloBaryonFraction * Mvir. HaloBaryonFraction is the satellite's own once-rounded
 * reionization_modifier * BaryonFrac, carried from sage26_reionization. The required parameter
 * CGMrecipeOn selects the reservoirs (R/src/model_infall.c:247-254): with CGMrecipeOn 1 the gas is
 * taken from the satellite's CGM pair if the satellite is in the CGM regime (its own Regime 0) and
 * given to the central's CGM pair if the central is (the central's Regime 0), each independently,
 * and from or to the hot pair otherwise; with CGMrecipeOn 0 both are the hot pair.
 *
 * Every floating-point expression is transcribed from SAGE26 with its operand
 * order and types, so that compiler contraction treats both alike.
 *
 * Reference: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2006, 2016)
 */

#include "error.h"
#include "module_system/parameter_helpers.h"
#include "shared/metallicity.h"
#include "shared/sage_cgm_routing.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

#define MODULE_NAME "sage26_satellite_stripping"

static double GLOBAL_BARYON_FRAC;

/* SAGE26's CGMrecipeOn (0 hot-gas routing, 1 regime-aware CGM routing), required and checked at
 * init */
static int CGM_RECIPE_ON;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_satellite_stripping_init(void) {
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (mimic_sage_validate_cgm_recipe_on(MODULE_NAME, CGM_RECIPE_ON) != 0) {
    return -1;
  }

  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  VERBOSE_LOG("SAGE satellite stripping module initialized");
  VERBOSE_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);
  VERBOSE_LOG("  CGMrecipeOn = %d", CGM_RECIPE_ON);

  return 0;
}

int sage26_satellite_stripping_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  // SAGE parity: stripping runs process_by_galaxy so it interleaves with
  // cooling in the galaxy-major loop, exactly as SAGE26 strips a Type 1
  // satellite just before that satellite cools (R/src/core_build_model.c:493-509).
  // Because the FOF central is normally processed earlier in the galaxy loop,
  // it has already cooled before satellites strip, so stripped gas reaches the
  // central's selected reservoir (its hot gas, or its CGM with CGMrecipeOn 1 and
  // the central's Regime 0) and cools on the next substep (not the current one).
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
   * rounded once to double on assignment (R/src/model_infall.c:236-241). CGMgas is zero
   * for satellites when the CGM recipe is off; with it on, a satellite keeps its CGM
   * (sage26_prepare_infall_budget), so the term counts. */
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
    /* SAGE parity (R/src/model_infall.c:247-254): the source pair is the one the satellite's
     * own regime uses and the destination pair the one the FoF central's regime uses, chosen
     * independently, so a satellite whose regime changed is stripped from the reservoir its
     * regime names even if its gas is held in the other (classification moves no gas). With
     * CGMrecipeOn 0 both are the hot pair. */
    const int take_from_cgm =
        (CGM_RECIPE_ON == MIMIC_SAGE_CGM_RECIPE_ON && sat_gal->Regime == MIMIC_SAGE_REGIME_CGM);
    const int give_to_cgm =
        (CGM_RECIPE_ON == MIMIC_SAGE_CGM_RECIPE_ON && cen_gal->Regime == MIMIC_SAGE_REGIME_CGM);

    float *const sat_gas = take_from_cgm ? &sat_gal->CGMgas : &sat_gal->HotGas;
    float *const sat_metals = take_from_cgm ? &sat_gal->MetalsCGMgas : &sat_gal->MetalsHotGas;

    // The double metallicity of the source pair, and the metals of the uncapped gas, formed
    // before the gas cap (:256-257)
    const double metallicity = mimic_get_metallicity(*sat_gas, *sat_metals);
    double strippedGasMetals = strippedGas * metallicity;

    // The gas and the source metal debit are capped independently (:259-260)
    if (strippedGas > *sat_gas)
      strippedGas = *sat_gas;
    if (strippedGasMetals > *sat_metals)
      strippedGasMetals = *sat_metals;

    *sat_gas -= strippedGas;
    *sat_metals -= strippedGasMetals;

    /* SAGE parity: asymmetric metal transfer (R/src/model_infall.c:256-270), on whichever
     * pairs are selected. The satellite is debited the unclamped-gas metals capped at its own
     * metals, but the central is credited the clamped gas times the original metallicity,
     * one expression that SAGE26's build fuses into a multiply-add (:267, :270). When the gas
     * clamp engages with metals above gas (Z capped at 1), the two differ and metals are not
     * conserved. This is SAGE26's deliberate source quirk, mirrored exactly. */
    if (give_to_cgm) {
      cen_gal->CGMgas += strippedGas;
      cen_gal->MetalsCGMgas += strippedGas * metallicity;
    } else {
      cen_gal->HotGas += strippedGas;
      cen_gal->MetalsHotGas += strippedGas * metallicity;
    }
  }

  return 0;
}

int sage26_satellite_stripping_cleanup(void) {
  VERBOSE_LOG("SAGE satellite stripping module cleaned up");
  return 0;
}
