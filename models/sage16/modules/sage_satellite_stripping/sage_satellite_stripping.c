/**
 * @file    sage_satellite_stripping.c
 * @brief   SAGE satellite stripping module - environmental gas stripping from satellites
 *
 * Strips hot gas from satellites when baryon content exceeds HaloBaryonFraction × Mvir.
 * Stripped gas transfers to central's hot reservoir with metallicity preserved.
 * Stripping distributed over substeps for numerical stability.
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

int sage_satellite_stripping_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  INFO_LOG("SAGE satellite stripping module initialized");
  VERBOSE_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);

  return 0;
}

int sage_satellite_stripping_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  // SAGE parity: stripping runs process_by_galaxy so it interleaves with
  // cooling in the galaxy-major loop, exactly as SAGE strips a Type 1
  // satellite just before that satellite cools (model_infall.c
  // strip_from_satellite, called inside evolve_galaxies). Because the FOF
  // central is normally processed earlier in the galaxy loop, it has already
  // cooled before satellites strip, so stripped gas reaches the central's hot
  // reservoir and cools on the next substep (not the current one).
  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  // Only Type 1 satellites with hot gas are stripped (SAGE: Type == 1 && HotGas > 0).
  if (halo->galaxy == NULL || halo->Type != 1 || halo->galaxy->HotGas <= 0.0f) {
    return 0;
  }

  if (ctx == NULL || ctx->central_galaxy == NULL || ctx->central_galaxy->galaxy == NULL) {
    DEBUG_LOG("No FOF central available for stripping target");
    return 0;
  }

  struct GalaxyData *sat_gal = halo->galaxy;
  struct GalaxyData *cen_gal = ctx->central_galaxy->galaxy;

  // Use HaloBaryonFraction (set by sage_reionization), fallback to global if unset
  const double halo_baryon_frac =
      (sat_gal->HaloBaryonFraction > 0.0f) ? sat_gal->HaloBaryonFraction : GLOBAL_BARYON_FRAC;

  // Calculate total baryons in satellite
  const double total_baryons = sat_gal->StellarMass + sat_gal->ColdGas + sat_gal->HotGas +
                               sat_gal->EjectedGas + sat_gal->BlackHoleMass + sat_gal->ICS;

  // Calculate amount to strip (distributed over substeps for stability)
  double strippedGas =
      -1.0 * (halo_baryon_frac * halo->Mvir - total_baryons) / (double)ctx->num_substeps;

  if (strippedGas > 0.0) {
    /* SAGE parity: strip_from_satellite uses a double-precision metallicity,
     * computes the metal transfer from the UNCLAMPED gas, clamps gas and
     * metals independently, and credits the central with clamped-gas *
     * metallicity (model_infall.c:106-118). When a clamp engages, the
     * satellite's metal loss and the central's metal gain differ — this
     * non-conservation is faithful to SAGE and must not be "fixed" without
     * regenerating the physics baseline. */
    const double metallicity = mimic_get_metallicity(sat_gal->HotGas, sat_gal->MetalsHotGas);
    double strippedMetals = strippedGas * metallicity;

    // Limit to available hot gas and metals
    if (strippedGas > sat_gal->HotGas)
      strippedGas = sat_gal->HotGas;
    if (strippedMetals > sat_gal->MetalsHotGas)
      strippedMetals = sat_gal->MetalsHotGas;

    // Transfer gas and metals from satellite to central (SAGE parity:
    // double-precision products accumulated into the float reservoirs)
    sat_gal->HotGas -= strippedGas;
    sat_gal->MetalsHotGas -= strippedMetals;
    cen_gal->HotGas += strippedGas;
    cen_gal->MetalsHotGas += strippedGas * metallicity;
  }

  return 0;
}

int sage_satellite_stripping_cleanup(void) {
  VERBOSE_LOG("SAGE satellite stripping module cleaned up");
  return 0;
}
