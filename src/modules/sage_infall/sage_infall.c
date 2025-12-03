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
 * - add_infall_to_hot(): Add infalling gas to hot reservoir with metallicity tracking
 *
 * Reference: Croton et al. (2006, 2016), based on SAGE model_infall.c
 */

#include <math.h>
#include <stdio.h>   /* Required for error.h logging macros */
#include <stdlib.h>  /* Required for error.h logging macros */

#include "constants.h"
#include "error.h"
#include "../_shared/metallicity.h"  // Shared utility for metallicity calculations
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_infall.h"
#include "types.h"

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
  double tot_stellarMass, tot_coldMass, tot_hotMass, tot_ejected;
  double tot_hotMetals, tot_ejectedMetals;
  double tot_ICS, tot_ICSMetals;
  double tot_BHMass;
  double tot_satBaryons;
  double infallingMass;

  tot_stellarMass = tot_coldMass = tot_hotMass = tot_hotMetals = tot_ejected =
      tot_ejectedMetals = tot_ICS = tot_ICSMetals = tot_BHMass = tot_satBaryons = 0.0;

  /* Sum baryonic components across FOF group */
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL)
      continue;

    tot_stellarMass += halos[i].galaxy->StellarMass;
    tot_coldMass += halos[i].galaxy->ColdGas;
    tot_hotMass += halos[i].galaxy->HotGas;
    tot_hotMetals += halos[i].galaxy->MetalsHotGas;
    tot_ejected += halos[i].galaxy->EjectedMass;
    tot_ejectedMetals += halos[i].galaxy->MetalsEjectedMass;
    tot_ICS += halos[i].galaxy->ICS;
    tot_ICSMetals += halos[i].galaxy->MetalsICS;
    tot_BHMass += halos[i].galaxy->BlackHoleMass;

    if (i != central_idx) {
      tot_satBaryons +=
          halos[i].galaxy->StellarMass + halos[i].galaxy->ColdGas +
          halos[i].galaxy->HotGas + halos[i].galaxy->BlackHoleMass;
    }

    /* Transfer satellite ejected gas and ICS to central */
    if (i != central_idx) {
      halos[i].galaxy->EjectedMass = 0.0f;
      halos[i].galaxy->MetalsEjectedMass = 0.0f;
      halos[i].galaxy->ICS = 0.0f;
      halos[i].galaxy->MetalsICS = 0.0f;
    }
  }

  /* Calculate infalling gas from HaloBaryonFraction (set by sage_reionization) */
  infallingMass =
      halos[central_idx].galaxy->HaloBaryonFraction * halos[central_idx].Mvir -
      (tot_stellarMass + tot_coldMass + tot_hotMass + tot_ejected + tot_BHMass + tot_ICS);

  /* Consolidate ejected mass to central */
  halos[central_idx].galaxy->EjectedMass = (float)tot_ejected;
  halos[central_idx].galaxy->MetalsEjectedMass = (float)tot_ejectedMetals;
  if (halos[central_idx].galaxy->MetalsEjectedMass >
      halos[central_idx].galaxy->EjectedMass) {
    halos[central_idx].galaxy->MetalsEjectedMass =
        halos[central_idx].galaxy->EjectedMass;
  }
  if (halos[central_idx].galaxy->EjectedMass < 0.0f) {
    halos[central_idx].galaxy->EjectedMass = 0.0f;
    halos[central_idx].galaxy->MetalsEjectedMass = 0.0f;
  }
  if (halos[central_idx].galaxy->MetalsEjectedMass < 0.0f) {
    halos[central_idx].galaxy->MetalsEjectedMass = 0.0f;
  }

  /* Consolidate ICS to central with physical constraints */
  halos[central_idx].galaxy->ICS = (float)tot_ICS;
  halos[central_idx].galaxy->MetalsICS = (float)tot_ICSMetals;
  if (halos[central_idx].galaxy->MetalsICS >
      halos[central_idx].galaxy->ICS) {
    halos[central_idx].galaxy->MetalsICS = halos[central_idx].galaxy->ICS;
  }
  if (halos[central_idx].galaxy->ICS < 0.0f) {
    halos[central_idx].galaxy->ICS = 0.0f;
    halos[central_idx].galaxy->MetalsICS = 0.0f;
  }
  if (halos[central_idx].galaxy->MetalsICS < 0.0f) {
    halos[central_idx].galaxy->MetalsICS = 0.0f;
  }

  halos[central_idx].galaxy->TotalSatelliteBaryons = (float)tot_satBaryons;

  return infallingMass;
}

/**
 * @brief   Add infalling gas to hot gas reservoir with metallicity tracking
 *
 * For negative infall (mass loss), removes from ejected reservoir first, then hot gas.
 *
 * @param   galaxy       Pointer to galaxy data
 * @param   infallingGas Amount of gas to add (can be negative)
 */
static void add_infall_to_hot(struct GalaxyData *galaxy, double infallingGas) {
  float metallicity;

  /* For mass loss, first remove from ejected reservoir */
  if (infallingGas < 0.0 && galaxy->EjectedMass > 0.0f) {
    metallicity = mimic_get_metallicity(galaxy->EjectedMass, galaxy->MetalsEjectedMass);

    galaxy->MetalsEjectedMass += (float)(infallingGas * metallicity);
    if (galaxy->MetalsEjectedMass < 0.0f) {
      galaxy->MetalsEjectedMass = 0.0f;
    }

    galaxy->EjectedMass += (float)infallingGas;

    /* If ejected reservoir depleted, continue removing from hot gas */
    if (galaxy->EjectedMass < 0.0f) {
      infallingGas = galaxy->EjectedMass;
      galaxy->EjectedMass = 0.0f;
      galaxy->MetalsEjectedMass = 0.0f;
    } else {
      infallingGas = 0.0;
    }
  }

  /* Continue removing from hot gas if still mass loss */
  if (infallingGas < 0.0 && galaxy->MetalsHotGas > 0.0f) {
    metallicity = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);

    galaxy->MetalsHotGas += (float)(infallingGas * metallicity);
    if (galaxy->MetalsHotGas < 0.0f) {
      galaxy->MetalsHotGas = 0.0f;
    }
  }

  galaxy->HotGas += (float)infallingGas;

  if (galaxy->HotGas < 0.0f) {
    galaxy->HotGas = 0.0f;
    galaxy->MetalsHotGas = 0.0f;
  }
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
  INFO_LOG("SAGE infall module initialized");
  INFO_LOG("  Physics: InfallingGas = HaloBaryonFraction * Mvir - baryons");
  INFO_LOG("  Requires: sage_reionization module to set HaloBaryonFraction");

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Calculates and applies cosmological infall for central galaxy.
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
static int sage_infall_process(struct ModuleContext *ctx, struct Halo *halos,
                                int ngal) {
#define STEPS 1  /* TODO: Will be replaced by global STEPS when multi-step integration loop implemented in core */

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
    DEBUG_LOG("No central galaxy found in FOF group (ngal=%d)", ngal);
    return 0;
  }

  if (halos[central_idx].galaxy == NULL) {
    ERROR_LOG("Central galaxy (index %d) has NULL galaxy data", central_idx);
    return -1;
  }

  double infallingMass = infall_recipe(halos, ngal, central_idx);

  halos[central_idx].galaxy->InfallingGas = (float)infallingMass;

  add_infall_to_hot(halos[central_idx].galaxy, infallingMass / STEPS);

  DEBUG_LOG("Infall: central Mvir=%.3e, HaloBaryonFrac=%.4f, infall=%.3e, HotGas=%.3e, z=%.3f",
            halos[central_idx].Mvir,
            halos[central_idx].galaxy->HaloBaryonFraction,
            infallingMass,
            halos[central_idx].galaxy->HotGas, z);

  return 0;
#undef STEPS
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

static struct Module sage_infall_module = {
    .name = "sage_infall",
    .init = sage_infall_init,
    .process_halos = sage_infall_process,
    .cleanup = sage_infall_cleanup};

void sage_infall_register(void) { module_registry_add(&sage_infall_module); }
