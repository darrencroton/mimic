/**
 * @file    sage_infall.c
 * @brief   SAGE infall module implementation
 *
 * Implements cosmological gas infall onto central galaxies. Central galaxies
 * accrete baryonic gas proportional to halo growth, modified by reionization
 * suppression (HaloBaryonFraction set by sage_reionization module). Also
 * consolidates satellite ejected gas and intracluster stars to centrals,
 * preserving metallicity throughout all transfers.
 *
 * Physics: InfallingGas = HaloBaryonFraction × Mvir - total_baryons
 *
 * Key functions:
 * - infall_recipe(): Calculate infalling gas mass and consolidate satellite reservoirs
 *
 * Note: Will only fully conserve FoF f_bar with stripping and mergers due to type 2s
 *
 * Reference: Croton et al. (2006, 2016), based on SAGE model_infall.c
 */

#include <math.h>
#include <stdio.h>   /* Required for error.h logging macros */
#include <stdlib.h>  /* Required for error.h logging macros */

#include "constants.h"
#include "error.h"
#include "../_system/parameter_helpers.h"  // Parameter loading and validation macros
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_infall.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double GLOBAL_BARYON_FRAC;


// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief   Calculate infalling gas mass for central galaxy
 *
 * Computes gas accretion from HaloBaryonFraction × Mvir minus current baryon
 * content. Consolidates ejected gas and intracluster stars from satellites to
 * central, preserving metallicity.
 *
 * @param   halos       Array of halos in FOF group
 * @param   ngal        Number of halos
 * @param   central_idx Index of central galaxy
 * @return  Mass of infalling gas (can be negative for mass loss)
 */
static double infall_recipe(struct Halo *halos, int ngal, int central_idx) {
  double tot_stellarMass, tot_BHMass, tot_coldMass, tot_hotMass, tot_ICS, tot_ejected;
  double tot_ICSMetals, tot_ejectedMetals;
  double infallingMass;

  /* Initialize counters for all baryonic components */
  tot_stellarMass = tot_BHMass = tot_coldMass = tot_hotMass = tot_ICS = tot_ejected =
      tot_ICSMetals = tot_ejectedMetals = 0.0;

  /* Loop over all galaxies in the FOF halo to sum baryonic components */
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL)
      continue;

    tot_stellarMass += halos[i].galaxy->StellarMass;
    tot_BHMass += halos[i].galaxy->BlackHoleMass;
    tot_coldMass += halos[i].galaxy->ColdGas;
    tot_hotMass += halos[i].galaxy->HotGas;
    tot_ICS += halos[i].galaxy->ICS;
    tot_ejected += halos[i].galaxy->EjectedMass;
    tot_ICSMetals += halos[i].galaxy->MetalsICS;
    tot_ejectedMetals += halos[i].galaxy->MetalsEjectedMass;

    /* Transfer satellite ejected gas and ICS to central */
    if (i != central_idx) {
      halos[i].galaxy->ICS = 0.0f;
      halos[i].galaxy->MetalsICS = 0.0f;
      halos[i].galaxy->EjectedMass = 0.0f;
      halos[i].galaxy->MetalsEjectedMass = 0.0f;
    }
  }

  /* Calculate infalling gas from HaloBaryonFraction */
  infallingMass =
      halos[central_idx].galaxy->HaloBaryonFraction * halos[central_idx].Mvir -
      (tot_stellarMass + tot_coldMass + tot_hotMass + tot_ejected + tot_BHMass + tot_ICS);

  /* Consolidate ejected mass to central galaxy */
  halos[central_idx].galaxy->EjectedMass = (float)tot_ejected;
  halos[central_idx].galaxy->MetalsEjectedMass = (float)tot_ejectedMetals;

  if (halos[central_idx].galaxy->MetalsEjectedMass > halos[central_idx].galaxy->EjectedMass) {
    halos[central_idx].galaxy->MetalsEjectedMass = halos[central_idx].galaxy->EjectedMass;
  }
  
  if (halos[central_idx].galaxy->EjectedMass < 0.0f) {
    halos[central_idx].galaxy->EjectedMass = 0.0f;
    halos[central_idx].galaxy->MetalsEjectedMass = 0.0f;
  }
  if (halos[central_idx].galaxy->MetalsEjectedMass < 0.0f) {
    halos[central_idx].galaxy->MetalsEjectedMass = 0.0f;
  }

  /* Consolidate ICS to central galaxy */
  halos[central_idx].galaxy->ICS = (float)tot_ICS;
  halos[central_idx].galaxy->MetalsICS = (float)tot_ICSMetals;

  if (halos[central_idx].galaxy->MetalsICS > halos[central_idx].galaxy->ICS) {
    halos[central_idx].galaxy->MetalsICS = halos[central_idx].galaxy->ICS;
  }
  if (halos[central_idx].galaxy->ICS < 0.0f) {
    halos[central_idx].galaxy->ICS = 0.0f;
    halos[central_idx].galaxy->MetalsICS = 0.0f;
  }
  if (halos[central_idx].galaxy->MetalsICS < 0.0f) {
    halos[central_idx].galaxy->MetalsICS = 0.0f;
  }

  return infallingMass;
}


// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize sage_infall module
 *
 * @return  0 on success
 */
static int sage_infall_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  INFO_LOG("SAGE infall module initialized");
  INFO_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);
  INFO_LOG("  Physics: InfallingGas = HaloBaryonFraction * Mvir - baryons");

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Calculates cosmological infall for central galaxy and stores in InfallingGas
 * property for distribution over substeps by sage_add_infall module.
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
static int sage_infall_process(struct ModuleContext *ctx, struct Halo *halos,
                                int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  double z = ctx->redshift;

  /* Find central galaxy */
  int central_idx = -1;
  for (int i = 0; i < ngal; i++) {
    if (halos[i].Type == 0) {
      central_idx = i;
      break;
    }
  }

  if (central_idx == -1) {
    ERROR_LOG("No central galaxy found in FOF group (ngal=%d)", ngal);
    return 0;
  }

  if (halos[central_idx].galaxy == NULL) {
    ERROR_LOG("Central galaxy (index %d) has NULL galaxy data", central_idx);
    return -1;
  }

  /* If first time, initialise HaloBaryonFraction to GlobalBaryonFraction */
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL) {
      ERROR_LOG("Halo %d has NULL galaxy data", i);
      return -1;
    }

    if (halos[i].galaxy->HaloBaryonFraction == -1.0) {
      halos[i].galaxy->HaloBaryonFraction = (float)(GLOBAL_BARYON_FRAC);
    }

  }

  double infallingMass = infall_recipe(halos, ngal, central_idx);

  /* Store infalling mass for distribution over substeps by sage_add_infall module */
  halos[central_idx].galaxy->InfallingGas = (float)infallingMass;

  DEBUG_LOG("Infall: central Mvir=%.3e, HaloBaryonFrac=%.4f, InfallingGas=%.3e, z=%.3f",
            halos[central_idx].Mvir,
            halos[central_idx].galaxy->HaloBaryonFraction,
            infallingMass, z);

  return 0;
}

/**
 * @brief   Cleanup sage_infall module
 *
 * @return  0 on success
 */
static int sage_infall_cleanup(void) {
  INFO_LOG("SAGE infall module cleaned up");
  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/* Extern reference to generated loop mode array */
extern const enum LoopMode sage_infall_supported_modes[];

static struct Module sage_infall_module = {
    .name = "sage_infall",
    .init = sage_infall_init,
    .process = sage_infall_process,
    .cleanup = sage_infall_cleanup,
    .supported_loop_modes = sage_infall_supported_modes,
    .num_supported_modes = 2  /* Default: supports both once and all */
};

void sage_infall_register(void) { module_registry_add(&sage_infall_module); }
