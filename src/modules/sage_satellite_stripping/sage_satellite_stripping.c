/**
 * @file    sage_satellite_stripping.c
 * @brief   Environmental gas stripping from satellite galaxies
 *
 * Implements environmental stripping of hot gas from satellite galaxies as
 * they orbit within their host halo. Stripped gas is transferred to the
 * central galaxy's hot gas reservoir.
 *
 * Physics:
 *   strippedGas = -(HaloBaryonFraction * Mvir - total_baryons) / STEPS
 *
 * HaloBaryonFraction is set by sage_reionization module (must run first).
 */

#include "sage_satellite_stripping.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "../_shared/metallicity.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "types.h"

/* Forward declarations */
static int sage_satellite_stripping_init(void);
static int sage_satellite_stripping_process(struct ModuleContext *ctx,
                                             struct Halo *halos, int ngal);
static int sage_satellite_stripping_cleanup(void);

/* Module-level parameters */
/* No parameters - uses HaloBaryonFraction property set by sage_reionization */

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================ */

/**
 * @brief   Strip hot gas from satellite galaxies
 *
 * Implements environmental stripping of hot gas from satellite galaxies
 * as they move through the hot halo of the central galaxy.
 *
 * @param   halos      Array of halos in FOF group
 * @param   central_idx Index of central galaxy
 * @param   sat_idx    Index of satellite galaxy being stripped
 */
static void strip_from_satellite(struct Halo *halos, int central_idx, int sat_idx) {
#define STEPS 1  /* TODO: Will be replaced by global STEPS when multi-step integration loop implemented in core */
  double strippedGas, strippedGasMetals;
  float metallicity;

  /* Calculate amount of gas to strip using halo-specific baryon fraction
   * (set by sage_reionization module with reionization suppression) */
  strippedGas = -1.0 *
                (halos[sat_idx].galaxy->HaloBaryonFraction * halos[sat_idx].Mvir -
                 (halos[sat_idx].galaxy->StellarMass +
                  halos[sat_idx].galaxy->ColdGas +
                  halos[sat_idx].galaxy->HotGas +
                  halos[sat_idx].galaxy->EjectedMass +
                  halos[sat_idx].galaxy->BlackHoleMass +
                  halos[sat_idx].galaxy->ICS)) /
                STEPS;

  /* Only proceed if there is positive stripping */
  if (strippedGas > 0.0) {
    /* Calculate metals in stripped gas */
    metallicity = mimic_get_metallicity(halos[sat_idx].galaxy->HotGas,
                                   halos[sat_idx].galaxy->MetalsHotGas);
    strippedGasMetals = strippedGas * metallicity;

    /* Limit stripping to available hot gas and metals */
    if (strippedGas > halos[sat_idx].galaxy->HotGas) {
      strippedGas = halos[sat_idx].galaxy->HotGas;
    }
    if (strippedGasMetals > halos[sat_idx].galaxy->MetalsHotGas) {
      strippedGasMetals = halos[sat_idx].galaxy->MetalsHotGas;
    }

    /* Remove gas and metals from satellite */
    halos[sat_idx].galaxy->HotGas -= (float)strippedGas;
    halos[sat_idx].galaxy->MetalsHotGas -= (float)strippedGasMetals;

    /* Add stripped gas and metals to central galaxy */
    halos[central_idx].galaxy->HotGas += (float)strippedGas;
    halos[central_idx].galaxy->MetalsHotGas += (float)strippedGasMetals;
  }
#undef STEPS
}

/* ============================================================================
 * MODULE LIFECYCLE FUNCTIONS
 * ============================================================================ */

/**
 * @brief   Initialize sage_satellite_stripping module
 *
 * No parameters to load - uses HaloBaryonFraction property set by sage_reionization.
 *
 * @return  0 on success, non-zero on error
 */
static int sage_satellite_stripping_init(void) {
  /* Log module configuration */
  INFO_LOG("SAGE satellite stripping module initialized");
  INFO_LOG("  Physics: strippedGas = -(HaloBaryonFraction * Mvir - baryons) / STEPS");
  INFO_LOG("  Requires: sage_reionization module to set HaloBaryonFraction");

  return 0;
}

/**
 * @brief   Process halos for satellite stripping
 *
 * For each FOF group:
 * 1. Find central galaxy (Type 0)
 * 2. Loop through Type 1 satellites
 * 3. Strip hot gas from satellites and transfer to central
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
static int sage_satellite_stripping_process(struct ModuleContext *ctx,
                                             struct Halo *halos, int ngal) {
  /* Suppress unused parameter warning */
  (void)ctx;

  /* Validate inputs */
  if (halos == NULL || ngal <= 0) {
    return 0; /* Nothing to process */
  }

  /* Find central galaxy (Type == 0) */
  int central_idx = -1;
  for (int i = 0; i < ngal; i++) {
    if (halos[i].Type == 0) {
      central_idx = i;
      break;
    }
  }

  if (central_idx == -1) {
    DEBUG_LOG("No central galaxy found in FOF group (ngal=%d)", ngal);
    return 0; /* Not an error - can happen in some tree structures */
  }

  /* Validate central galaxy has data */
  if (halos[central_idx].galaxy == NULL) {
    ERROR_LOG("Central galaxy (index %d) has NULL galaxy data", central_idx);
    return -1;
  }

  /* Strip gas from satellites */
  for (int i = 0; i < ngal; i++) {
    if (i == central_idx)
      continue; /* Skip central */
    if (halos[i].Type != 1)
      continue; /* Only process Type 1 satellites */
    if (halos[i].galaxy == NULL)
      continue; /* Skip if no galaxy data */
    if (halos[i].galaxy->HotGas <= 0.0f)
      continue; /* Skip if no hot gas */

    /* Strip hot gas from this satellite */
    strip_from_satellite(halos, central_idx, i);
  }

  return 0;
}

/**
 * @brief   Cleanup sage_satellite_stripping module
 *
 * No allocated resources to free for this module.
 *
 * @return  0 on success
 */
static int sage_satellite_stripping_cleanup(void) {
  INFO_LOG("SAGE satellite stripping module cleaned up");
  return 0;
}

/* ============================================================================
 * MODULE REGISTRATION
 * ============================================================================ */

/**
 * @brief   Module structure for sage_satellite_stripping module
 */
static struct Module sage_satellite_stripping_module = {
    .name = "sage_satellite_stripping",
    .init = sage_satellite_stripping_init,
    .process_halos = sage_satellite_stripping_process,
    .cleanup = sage_satellite_stripping_cleanup};

/**
 * @brief   Register the sage_satellite_stripping module
 */
void sage_satellite_stripping_register(void) {
  module_registry_add(&sage_satellite_stripping_module);
}
