/**
 * @file    sage_satellite_stripping.c
 * @brief   Environmental gas stripping from satellite galaxies
 *
 * Implements environmental stripping of hot gas from satellite galaxies as
 * they orbit within their host halo. Stripped gas is transferred to the
 * central galaxy's hot gas reservoir.
 */

#include "sage_satellite_stripping.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "../shared/metallicity.h"
#include "../shared/reionization.h"
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
static double BARYON_FRAC;

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
 * @param   redshift   Current redshift
 * @param   omega      Matter density parameter
 * @param   omega_lambda Dark energy density parameter
 * @param   hubble_h   Hubble parameter
 */
static void strip_from_satellite(struct Halo *halos, int central_idx,
                                  int sat_idx, double redshift, double omega,
                                  double omega_lambda, double hubble_h) {
#define STEPS 1  /* TODO: Will be replaced by global STEPS when multi-step integration loop implemented in core */
  double reionization_modifier;
  double strippedGas, strippedGasMetals;
  float metallicity;

  /* Apply reionization modifier using shared utility */
  reionization_modifier = calculate_reionization_modifier(
      halos[sat_idx].Mvir, redshift, omega, omega_lambda, hubble_h);

  /* Calculate amount of gas to strip */
  strippedGas = -1.0 *
                (reionization_modifier * BARYON_FRAC * halos[sat_idx].Mvir -
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
 * Reads module parameters from configuration file.
 *
 * @return  0 on success, non-zero on error
 */
static int sage_satellite_stripping_init(void) {
  /* Read and validate parameters from model configuration.
   * All parameters are REQUIRED in input file (no defaults). */
  if (model_get_double("BaryonFrac", &BARYON_FRAC) != 0) {
    return -1;
  }

  INFO_LOG("SAGE satellite stripping module initialized");
  INFO_LOG("  BaryonFrac = %.4f", BARYON_FRAC);
  INFO_LOG("  Reionization model: Gnedin (2000) - hardcoded in "
           "shared/reionization.h");

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
  /* Validate inputs */
  if (halos == NULL || ngal <= 0) {
    return 0; /* Nothing to process */
  }

  /* Extract cosmological parameters from context */
  double z = ctx->redshift;
  double omega = ctx->params->Omega;
  double omega_lambda = ctx->params->OmegaLambda;
  double hubble_h = ctx->params->Hubble_h;

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
    strip_from_satellite(halos, central_idx, i, z, omega, omega_lambda,
                         hubble_h);
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
