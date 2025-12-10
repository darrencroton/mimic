/**
 * @file    sage_satellite_stripping.c
 * @brief   SAGE satellite stripping module implementation
 *
 * Implements environmental gas removal from satellites via ram pressure and tidal
 * stripping. Stripped gas transfers to central galaxy's hot reservoir with
 * metallicity preserved. Only processes Type 1 satellites.
 *
 * Physics: strippedGas = -(HaloBaryonFraction × Mvir - total_baryons) / num_substeps
 *
 * Key functions:
 * - strip_from_satellite(): Remove hot gas from satellite and transfer to central
 *
 * Reference: Gnedin (2000), Kravtsov et al. (2004), Croton et al. (2006, 2016)
 */

#include "sage_satellite_stripping.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "../_system/parameter_helpers.h"  // Parameter loading and validation macros
#include "../_shared/metallicity.h"
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

/**
 * @brief   Strip hot gas from satellite and transfer to central
 *
 * @param   halos       Array of halos in FOF group
 * @param   central_idx Index of central galaxy
 * @param   sat_idx     Index of satellite galaxy being stripped
 * @param   num_substeps Number of substeps for distributing stripping
 */
static void strip_from_satellite(struct Halo *halos, int central_idx, int sat_idx,
                                  int num_substeps) {
  double strippedGas, strippedGasMetals;
  float metallicity;

  /* Calculate amount to strip from HaloBaryonFraction (set by sage_reionization)
   * Distributed over substeps for numerical stability */
  strippedGas = -1.0 *
                (halos[sat_idx].galaxy->HaloBaryonFraction * halos[sat_idx].Mvir -
                 (halos[sat_idx].galaxy->StellarMass +
                  halos[sat_idx].galaxy->ColdGas +
                  halos[sat_idx].galaxy->HotGas +
                  halos[sat_idx].galaxy->EjectedMass +
                  halos[sat_idx].galaxy->BlackHoleMass +
                  halos[sat_idx].galaxy->ICS)) /
                (double)num_substeps;

  if (strippedGas > 0.0) {
    metallicity = mimic_get_metallicity(halos[sat_idx].galaxy->HotGas,
                                   halos[sat_idx].galaxy->MetalsHotGas);
    strippedGasMetals = strippedGas * metallicity;

    /* Limit to available hot gas and metals */
    if (strippedGas > halos[sat_idx].galaxy->HotGas) {
      strippedGas = halos[sat_idx].galaxy->HotGas;
    }
    if (strippedGasMetals > halos[sat_idx].galaxy->MetalsHotGas) {
      strippedGasMetals = halos[sat_idx].galaxy->MetalsHotGas;
    }

    /* Transfer from satellite to central */
    halos[sat_idx].galaxy->HotGas -= (float)strippedGas;
    halos[sat_idx].galaxy->MetalsHotGas -= (float)strippedGasMetals;
    halos[central_idx].galaxy->HotGas += (float)strippedGas;
    halos[central_idx].galaxy->MetalsHotGas += (float)strippedGasMetals;
  }
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize sage_satellite_stripping module
 *
 * @return  0 on success
 */
static int sage_satellite_stripping_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");
  
  INFO_LOG("SAGE satellite stripping module initialized");
  INFO_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);
  INFO_LOG("  Physics: strippedGas = -(HaloBaryonFraction * Mvir - baryons) / num_substeps");
  INFO_LOG("  Requires: sage_reionization module to set HaloBaryonFraction");

  return 0;
}

/**
 * @brief   Process halos for satellite stripping
 *
 * Strips hot gas from Type 1 satellites and transfers to central galaxy.
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
static int sage_satellite_stripping_process(struct ModuleContext *ctx,
                                             struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  /* Find central galaxy */
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

  /* Strip gas from Type 1 satellites */
  for (int i = 0; i < ngal; i++) {
    if (i == central_idx)
      continue;
    if (halos[i].galaxy == NULL)
      continue;
    if (halos[i].galaxy->HotGas <= 0.0f)
      continue;

    if (halos[i].galaxy->HaloBaryonFraction == -1.0) {
      halos[i].galaxy->HaloBaryonFraction = (float)(GLOBAL_BARYON_FRAC);
    }

    strip_from_satellite(halos, central_idx, i, ctx->num_substeps);
  }

  return 0;
}

/**
 * @brief   Cleanup sage_satellite_stripping module
 *
 * @return  0 on success
 */
static int sage_satellite_stripping_cleanup(void) {
  INFO_LOG("SAGE satellite stripping module cleaned up");
  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/* Extern reference to generated loop mode array */
extern const enum LoopMode sage_satellite_stripping_supported_modes[];

static struct Module sage_satellite_stripping_module = {
    .name = "sage_satellite_stripping",
    .init = sage_satellite_stripping_init,
    .process = sage_satellite_stripping_process,
    .cleanup = sage_satellite_stripping_cleanup,
    .supported_loop_modes = sage_satellite_stripping_supported_modes,
    .num_supported_modes = 2  /* Default: supports both once and all */
};

void sage_satellite_stripping_register(void) {
  module_registry_add(&sage_satellite_stripping_module);
}
