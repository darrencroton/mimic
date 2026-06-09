/**
 * @file    sage_apply_infall.c
 * @brief   SAGE infall application - distributes infalling gas budget to hot reservoir over
 * substeps
 *
 * Distributes infalling gas (from sage_prepare_infall_budget) to hot reservoir over substeps
 * with metallicity tracking. For negative infall (mass loss), removes from ejected
 * reservoir first, then hot gas.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "shared/metallicity.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "types.h"

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_apply_infall_init(void) {
  /* Dependency check: sage_prepare_infall_budget must be present in pre_timestep */
  if (!module_configured_in_phase("sage_prepare_infall_budget", MimicConfig.pre_timestep,
                                  MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO)) {
    ERROR_LOG("sage_apply_infall requires sage_prepare_infall_budget in "
              "pre_timestep as process_full_halo — InfallingGas will be 0 "
              "without it, producing no infall");
    return -1;
  }

  INFO_LOG("SAGE apply infall module initialized");
  return 0;
}

int sage_apply_infall_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  // Find central galaxy
  int central_idx = -1;
  for (int i = 0; i < ngal; i++) {
    if (halos[i].Type == 0) {
      central_idx = i;
      break;
    }
  }

  if (central_idx == -1) {
    DEBUG_LOG("No central galaxy found in FOF group (ngal=%d)", ngal);
    return 0;
  }

  if (halos[central_idx].galaxy == NULL) {
    ERROR_LOG("Central galaxy (index %d) has NULL galaxy data", central_idx);
    return -1;
  }

  struct GalaxyData *galaxy = halos[central_idx].galaxy;

  // Calculate infall amount for this substep
  double infallingGas = (double)galaxy->InfallingGas / (double)ctx->num_substeps;

  // If the halo has lost mass, subtract baryons from the ejected mass first, then the hot gas
  if (infallingGas < 0.0 && galaxy->EjectedGas > 0.0f) {
    const float metallicity = mimic_get_metallicity(galaxy->EjectedGas, galaxy->MetalsEjectedGas);
    galaxy->MetalsEjectedGas += (float)(infallingGas * metallicity);
    if (galaxy->MetalsEjectedGas < 0.0f) {
      galaxy->MetalsEjectedGas = 0.0f;
    }

    galaxy->EjectedGas += (float)infallingGas;
    if (galaxy->EjectedGas < 0.0f) {
      infallingGas = galaxy->EjectedGas;
      galaxy->EjectedGas = 0.0f;
      galaxy->MetalsEjectedGas = 0.0f;
    } else {
      infallingGas = 0.0;
    }
  }

  // If the halo has lost mass, subtract hot metals mass next, then the hot gas
  if (infallingGas < 0.0 && galaxy->MetalsHotGas > 0.0f) {
    const float metallicity = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);
    galaxy->MetalsHotGas += (float)(infallingGas * metallicity);
    if (galaxy->MetalsHotGas < 0.0f) {
      galaxy->MetalsHotGas = 0.0f;
    }
  }

  // Add (subtract) the ambient (enriched) infalling gas to the central galaxy hot component
  galaxy->HotGas += (float)infallingGas;
  if (galaxy->HotGas < 0.0f) {
    galaxy->HotGas = 0.0f;
    galaxy->MetalsHotGas = 0.0f;
  }

  DEBUG_LOG("AddInfall: Mvir=%.3e, InfallingGas/step=%.3e, HotGas=%.3e, substep=%d/%d",
            halos[central_idx].Mvir, infallingGas, galaxy->HotGas, ctx->substep_number + 1,
            ctx->num_substeps);

  return 0;
}

int sage_apply_infall_cleanup(void) {
  VERBOSE_LOG("SAGE apply infall module cleaned up");
  return 0;
}
