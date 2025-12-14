/**
 * @file    sage_satellite_stripping.c
 * @brief   SAGE satellite stripping module implementation
 *
 * Implements environmental gas stripping from satellites via ram pressure and tidal
 * effects. Stripped gas transfers to central's hot reservoir with metallicity preserved.
 *
 * Physics: strippedGas = -(HaloBaryonFraction × Mvir - total_baryons) / SubSteps
 *
 * Key functions:
 * - sage_satellite_stripping_process(): Strip gas from satellites, transfer to central
 *
 * Mass flow: Satellite HotGas → Central HotGas (with metals)
 *
 * Reference: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2006, 2016)
 */

#include <stdio.h>
#include <stdlib.h>

#include "error.h"
#include "_system/parameter_helpers.h"
#include "_shared/metallicity.h"
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

/**
 * @brief   Initialize sage_satellite_stripping module
 *
 * @return  0 on success
 */
int sage_satellite_stripping_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  INFO_LOG("SAGE satellite stripping module initialized");
  VERBOSE_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);

  return 0;
}

/**
 * @brief   Process halos for satellite stripping
 *
 * Strips hot gas from satellites when baryon content exceeds HaloBaryonFraction × Mvir.
 * Transfers stripped gas and metals to central galaxy's hot reservoir. Stripping
 * distributed over substeps for numerical stability.
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
int sage_satellite_stripping_process(struct ModuleContext *ctx,
                                             struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  /* Find central galaxy (Type 0) */
  int central_idx = -1;
  for (int i = 0; i < ngal; i++) {
    if (halos[i].Type == 0) {
      central_idx = i;
      break;
    }
  }

  if (central_idx == -1) {
    DEBUG_LOG("No central galaxy in FOF group (ngal=%d)", ngal);
    return 0;
  }

  if (halos[central_idx].galaxy == NULL) {
    ERROR_LOG("Central galaxy (index %d) has NULL galaxy data", central_idx);
    return -1;
  }

  /* Strip hot gas from satellites and transfer to central */
  for (int i = 0; i < ngal; i++) {
    if (i == central_idx)
      continue;
    if (halos[i].galaxy == NULL)
      continue;
    if (halos[i].Type == 3)
      continue;
    if (halos[i].galaxy->HotGas <= 0.0f)
      continue;

    struct GalaxyData *sat_gal = halos[i].galaxy;
    struct GalaxyData *cen_gal = halos[central_idx].galaxy;

    /* Use HaloBaryonFraction (set by sage_reionization), fallback to global if unset */
    double halo_baryon_frac = (sat_gal->HaloBaryonFraction > 0.0f)
                                  ? sat_gal->HaloBaryonFraction
                                  : GLOBAL_BARYON_FRAC;

    /* Calculate total baryons in satellite */
    double total_baryons = sat_gal->StellarMass + sat_gal->ColdGas + sat_gal->HotGas +
                           sat_gal->EjectedGas + sat_gal->BlackHoleMass + sat_gal->ICS;

    /* Calculate amount to strip (distributed over substeps for stability) */
    double strippedGas = -1.0 * (halo_baryon_frac * halos[i].Mvir - total_baryons) /
                         (double)ctx->num_substeps;

    if (strippedGas > 0.0) {
      /* Calculate metallicity for metal transfer */
      float metallicity = mimic_get_metallicity(sat_gal->HotGas, sat_gal->MetalsHotGas);
      double strippedMetals = strippedGas * metallicity;

      /* Limit to available hot gas and metals */
      if (strippedGas > sat_gal->HotGas) {
        strippedGas = sat_gal->HotGas;
      }
      if (strippedMetals > sat_gal->MetalsHotGas) {
        strippedMetals = sat_gal->MetalsHotGas;
      }

      /* Transfer gas and metals from satellite to central */
      sat_gal->HotGas -= (float)strippedGas;
      sat_gal->MetalsHotGas -= (float)strippedMetals;
      cen_gal->HotGas += (float)strippedGas;
      cen_gal->MetalsHotGas += (float)strippedMetals;
    }
  }

  return 0;
}

/**
 * @brief   Cleanup sage_satellite_stripping module
 *
 * @return  0 on success
 */
int sage_satellite_stripping_cleanup(void) {
  VERBOSE_LOG("SAGE satellite stripping module cleaned up");
  return 0;
}
