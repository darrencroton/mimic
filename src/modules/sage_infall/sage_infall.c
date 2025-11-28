/**
 * @file    sage_infall.c
 * @brief   SAGE infall module implementation
 *
 * This module implements cosmological gas infall from the SAGE model.
 * It handles:
 * - Cosmological gas infall onto central galaxies
 * - Reionization suppression of gas accretion onto low-mass halos
 * - Redistribution of ejected gas and ICS from satellites to central
 *
 * Physics:
 *   infallingMass = f_reion * BaryonFrac * Mvir - (total baryon content)
 *   InfallingGas = infallingMass  (stored in property)
 *   HotGas += InfallingGas / STEPS
 *
 * The reionization suppression follows Gnedin (2000) with fitting formulas
 * from Kravtsov et al. (2004). Implemented in shared/reionization.h utility.
 *
 * Implementation Notes:
 * - Central galaxies accrete gas from the cosmic web
 * - All baryonic components are tracked for mass conservation
 * - Metal content is preserved during gas transfers
 * - InfallingGas property calculated once, applied over STEPS (currently STEPS=1)
 *
 * MODULAR DESIGN
 * ==============
 * This module has been refactored to focus solely on cosmological infall.
 * Related processes now in separate modules:
 * - sage_satellite_stripping: Environmental gas stripping from satellites
 * - shared/reionization.h: Reionization suppression calculations
 *
 * Reference:
 *   - Croton et al. (2016) - SAGE model description
 *   - Gnedin (2000) - Reionization model
 *   - Kravtsov et al. (2004) - Filtering mass formulas
 *   - SAGE: sage-code/model_infall.c
 *
 * Vision Principles:
 *   - Physics-Agnostic Core: Interacts only through module interface
 *   - Runtime Modularity: Configurable via parameter file
 *   - Single Source of Truth: Reionization in shared header, InfallingGas property
 */

#include <math.h>
#include <stdio.h>   /* Required for error.h logging macros */
#include <stdlib.h>  /* Required for error.h logging macros */

#include "constants.h"
#include "error.h"
#include "../shared/metallicity.h"  // Shared utility for metallicity calculations
#include "../shared/reionization.h" // Shared utility for reionization suppression
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_infall.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================
// Parameters defined in module_info.yaml (single source of truth).
// Loaded at runtime via model_get_double().
// Defaults and validation ranges come from metadata - no hardcoding.

static double BARYON_FRAC;

// ============================================================================
// HELPER FUNCTIONS (Physics Calculations)
// ============================================================================

// Metallicity calculation: mimic_get_metallicity() from shared/metallicity.h
// Reionization suppression: calculate_reionization_modifier() from shared/reionization.h

/**
 * @brief   Calculate the amount of gas infalling onto a central galaxy
 *
 * Calculates the amount of gas that should be accreted onto a halo based on:
 * 1. Cosmic baryon fraction × halo mass
 * 2. Current baryon content (all components)
 * 3. Reionization suppression (using shared reionization.h utility)
 *
 * Also handles:
 * - Consolidation of ejected gas from satellites to central
 * - Consolidation of ICS (intracluster stars) to central
 * - Mass conservation accounting
 *
 * @param   halos    Array of halos in FOF group
 * @param   ngal     Number of halos
 * @param   central_idx Index of central galaxy
 * @param   redshift Current redshift
 * @param   omega    Matter density parameter
 * @param   omega_lambda Dark energy density parameter
 * @param   hubble_h Hubble parameter
 * @return  Mass of infalling gas (can be negative for mass loss)
 */
static double infall_recipe(struct Halo *halos, int ngal, int central_idx,
                             double redshift, double omega, double omega_lambda,
                             double hubble_h) {
  double tot_stellarMass, tot_coldMass, tot_hotMass, tot_ejected;
  double tot_hotMetals, tot_ejectedMetals;
  double tot_ICS, tot_ICSMetals;
  double tot_BHMass;
  double tot_satBaryons;
  double infallingMass, reionization_modifier;

  /* Initialize counters for all baryonic components */
  tot_stellarMass = tot_coldMass = tot_hotMass = tot_hotMetals = tot_ejected =
      tot_ejectedMetals = tot_ICS = tot_ICSMetals = tot_BHMass = tot_satBaryons = 0.0;

  /* Loop over all galaxies in FOF group to sum baryonic components */
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL)
      continue;

    /* Sum all baryonic mass components */
    tot_stellarMass += halos[i].galaxy->StellarMass;
    tot_coldMass += halos[i].galaxy->ColdGas;
    tot_hotMass += halos[i].galaxy->HotGas;
    tot_hotMetals += halos[i].galaxy->MetalsHotGas;
    tot_ejected += halos[i].galaxy->EjectedMass;
    tot_ejectedMetals += halos[i].galaxy->MetalsEjectedMass;
    tot_ICS += halos[i].galaxy->ICS;
    tot_ICSMetals += halos[i].galaxy->MetalsICS;
    tot_BHMass += halos[i].galaxy->BlackHoleMass;

    /* Track baryons in satellite galaxies separately */
    if (i != central_idx) {
      tot_satBaryons +=
          halos[i].galaxy->StellarMass + halos[i].galaxy->ColdGas +
          halos[i].galaxy->HotGas + halos[i].galaxy->BlackHoleMass;
    }

    /* Move satellite ejected gas to central galaxy's ejected reservoir */
    if (i != central_idx) {
      /* Zero satellite values - mass already transferred to central's tot_ejected above */
      halos[i].galaxy->EjectedMass = 0.0f;
      halos[i].galaxy->MetalsEjectedMass = 0.0f;
    }

    /* Move satellite intracluster stars to central galaxy */
    if (i != central_idx) {
      /* Zero satellite values - mass already transferred to central's tot_ICS above */
      halos[i].galaxy->ICS = 0.0f;
      halos[i].galaxy->MetalsICS = 0.0f;
    }
  }

  /* Calculate reionization suppression factor using shared utility */
  reionization_modifier = calculate_reionization_modifier(
      halos[central_idx].Mvir, redshift, omega, omega_lambda, hubble_h);

  /* Calculate infalling gas mass */
  infallingMass =
      reionization_modifier * BARYON_FRAC * halos[central_idx].Mvir -
      (tot_stellarMass + tot_coldMass + tot_hotMass + tot_ejected + tot_BHMass + tot_ICS);

  /* Assign all ejected mass to the central galaxy */
  halos[central_idx].galaxy->EjectedMass = (float)tot_ejected;
  halos[central_idx].galaxy->MetalsEjectedMass = (float)tot_ejectedMetals;

  /* Enforce physical constraints on ejected mass and metals */
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

  /* Assign all intracluster stars to the central galaxy */
  halos[central_idx].galaxy->ICS = (float)tot_ICS;
  halos[central_idx].galaxy->MetalsICS = (float)tot_ICSMetals;

  /* Enforce physical constraints on ICS mass and metals */
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

  /* Update tracking variable */
  halos[central_idx].galaxy->TotalSatelliteBaryons = (float)tot_satBaryons;

  return infallingMass;
}

/**
 * @brief   Add infalling gas to the hot gas component
 *
 * Handles addition/removal of gas to/from the hot gas reservoir.
 * For negative infall (mass loss):
 * 1. First remove from ejected gas reservoir
 * 2. Then remove from hot gas reservoir
 *
 * @param   galaxy       Pointer to galaxy data
 * @param   infallingGas Amount of gas to add (can be negative)
 */
static void add_infall_to_hot(struct GalaxyData *galaxy, double infallingGas) {
  float metallicity;

  /* Handle mass loss case (negative infall) */
  if (infallingGas < 0.0 && galaxy->EjectedMass > 0.0f) {
    /* First remove from ejected gas reservoir */
    metallicity = mimic_get_metallicity(galaxy->EjectedMass, galaxy->MetalsEjectedMass);

    /* Update ejected metals */
    galaxy->MetalsEjectedMass += (float)(infallingGas * metallicity);
    if (galaxy->MetalsEjectedMass < 0.0f) {
      galaxy->MetalsEjectedMass = 0.0f;
    }

    /* Update ejected gas mass */
    galaxy->EjectedMass += (float)infallingGas;

    /* If ejected reservoir is depleted, continue removing from hot gas */
    if (galaxy->EjectedMass < 0.0f) {
      infallingGas = galaxy->EjectedMass;
      galaxy->EjectedMass = 0.0f;
      galaxy->MetalsEjectedMass = 0.0f;
    } else {
      infallingGas = 0.0;
    }
  }

  /* If we still have mass loss, remove from hot gas metals */
  if (infallingGas < 0.0 && galaxy->MetalsHotGas > 0.0f) {
    metallicity = mimic_get_metallicity(galaxy->HotGas, galaxy->MetalsHotGas);

    galaxy->MetalsHotGas += (float)(infallingGas * metallicity);
    if (galaxy->MetalsHotGas < 0.0f) {
      galaxy->MetalsHotGas = 0.0f;
    }
  }

  /* Finally update the hot gas component */
  galaxy->HotGas += (float)infallingGas;

  /* Ensure non-negative values */
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
 * Reads configuration parameters.
 * Parameters are automatically validated against ranges defined in module_info.yaml.
 *
 * Vision Principle 3 (Metadata-Driven): Parameters loaded and validated from metadata.
 * Vision Principle 4 (Single Source of Truth): No hardcoded ranges in C code.
 *
 * @return  0 on success, non-zero on failure
 */
static int sage_infall_init(void) {
  /* Read and validate parameters from model configuration.
   * All parameters are REQUIRED in input file (no defaults). */
  if (model_get_double("BaryonFrac", &BARYON_FRAC) != 0) {
    return -1;
  }

  /* Log module configuration */
  INFO_LOG("SAGE infall module initialized");
  INFO_LOG("  Physics: InfallingGas = f_reion * BaryonFrac * Mvir - baryons");
  INFO_LOG("  BaryonFrac = %.4f", BARYON_FRAC);
  INFO_LOG("  Reionization model: Gnedin (2000) - hardcoded in shared/reionization.h");

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Implements infall for central galaxy.
 * Process order:
 * 1. Find central galaxy (Type == 0)
 * 2. Calculate InfallingGas for central (stored in property)
 * 3. Add InfallingGas/STEPS to central's hot reservoir
 *
 * Note: Satellite stripping now handled by sage_satellite_stripping module.
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on failure
 */
static int sage_infall_process(struct ModuleContext *ctx, struct Halo *halos,
                                int ngal) {
#define STEPS 1  /* TODO: Will be replaced by global STEPS when multi-step integration loop implemented in core */

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

  /* Calculate infall for central galaxy */
  double infallingMass =
      infall_recipe(halos, ngal, central_idx, z, omega, omega_lambda, hubble_h);

  /* Store in InfallingGas property (for future STEPS integration) */
  halos[central_idx].galaxy->InfallingGas = (float)infallingMass;

  /* Add infalling gas to central's hot reservoir (divided by STEPS) */
  add_infall_to_hot(halos[central_idx].galaxy, infallingMass / STEPS);

  /* Debug logging */
  DEBUG_LOG("Infall: central Mvir=%.3e, infall=%.3e, HotGas=%.3e, z=%.3f",
            halos[central_idx].Mvir, infallingMass,
            halos[central_idx].galaxy->HotGas, z);

  return 0;
#undef STEPS
}

/**
 * @brief   Cleanup sage_infall module
 *
 * No allocated resources to free for this module.
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

/**
 * @brief   Module structure for sage_infall module
 */
static struct Module sage_infall_module = {
    .name = "sage_infall",
    .init = sage_infall_init,
    .process_halos = sage_infall_process,
    .cleanup = sage_infall_cleanup};

/**
 * @brief   Register the sage_infall module
 */
void sage_infall_register(void) { module_registry_add(&sage_infall_module); }
