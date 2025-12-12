/**
 * @file    sage_calculate_infall.c
 * @brief   SAGE calculate infall module implementation
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
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double GLOBAL_BARYON_FRAC;


// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief   Validate and clamp mass and metal components
 *
 * Ensures physical constraints: mass >= 0, metals >= 0, metals <= mass.
 *
 * @param   mass    Pointer to mass component
 * @param   metals  Pointer to metal component
 */
static inline void validate_mass_metals(float *mass, float *metals) {
  if (*mass < 0.0f) {
    *mass = 0.0f;
    *metals = 0.0f;
  } else {
    if (*metals < 0.0f) *metals = 0.0f;
    if (*metals > *mass) *metals = *mass;
  }
}

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
  double tot_stellarMass = 0.0, tot_BHMass = 0.0, tot_coldMass = 0.0;
  double tot_hotMass = 0.0, tot_ICS = 0.0, tot_ejected = 0.0;
  double tot_ICSMetals = 0.0, tot_ejectedMetals = 0.0;
  double orphan_hotMass = 0.0, orphan_hotMassMetals = 0.0;

  /* Sum baryonic components across all galaxies and transfer satellite reservoirs */
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL || halos[i].Type == 3)
      continue;

    struct GalaxyData *gal = halos[i].galaxy;

    tot_stellarMass += gal->StellarMass;
    tot_BHMass += gal->BlackHoleMass;
    tot_coldMass += gal->ColdGas;
    tot_hotMass += gal->HotGas;
    tot_ICS += gal->ICS;
    tot_ejected += gal->EjectedMass;
    tot_ICSMetals += gal->MetalsICS;
    tot_ejectedMetals += gal->MetalsEjectedMass;
    
    if (halos[i].Type == 2) {
      orphan_hotMass += gal->HotGas;
      orphan_hotMassMetals += gal->MetalsHotGas;
    }

    if (i != central_idx) {
      gal->ICS = 0.0f;
      gal->MetalsICS = 0.0f;
      gal->EjectedMass = 0.0f;
      gal->MetalsEjectedMass = 0.0f;

      if (halos[i].Type == 2) {
        gal->HotGas = 0.0f;
        gal->MetalsHotGas = 0.0f;
      }
    }
  }

  struct GalaxyData *central = halos[central_idx].galaxy;

  /* Consolidate ejected mass, ICS and type 2 hot mass to central */
  central->EjectedMass = (float)tot_ejected;
  central->MetalsEjectedMass = (float)tot_ejectedMetals;
  validate_mass_metals(&central->EjectedMass, &central->MetalsEjectedMass);

  central->ICS = (float)tot_ICS;
  central->MetalsICS = (float)tot_ICSMetals;
  validate_mass_metals(&central->ICS, &central->MetalsICS);

  central->HotGas += (float)orphan_hotMass;
  central->MetalsHotGas += (float)orphan_hotMassMetals;
  validate_mass_metals(&central->HotGas, &central->MetalsHotGas);

  /* Calculate infalling gas from HaloBaryonFraction */
  return central->HaloBaryonFraction * halos[central_idx].Mvir -
         (tot_stellarMass + tot_coldMass + tot_hotMass + tot_ejected + tot_BHMass + tot_ICS);
}


// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize sage_calculate_infall module
 *
 * @return  0 on success
 */
static int sage_calculate_infall_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  INFO_LOG("SAGE calculate infall module initialized");
  VERBOSE_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);
  VERBOSE_LOG("  Physics: InfallingGas = HaloBaryonFraction * Mvir - baryons");

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
static int sage_calculate_infall_process(struct ModuleContext *ctx, struct Halo *halos,
                                int ngal) {
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
    ERROR_LOG("No central galaxy found in FOF group (ngal=%d)", ngal);
    return 0;
  }

  if (halos[central_idx].galaxy == NULL) {
    ERROR_LOG("Central galaxy (index %d) has NULL galaxy data", central_idx);
    return -1;
  }

  /* Initialize HaloBaryonFraction to GlobalBaryonFraction if first time */
  if (halos[central_idx].galaxy->HaloBaryonFraction == -1.0f) {
    halos[central_idx].galaxy->HaloBaryonFraction = (float)GLOBAL_BARYON_FRAC;
  }

  /* Calculate and store infalling mass */
  double infallingMass = infall_recipe(halos, ngal, central_idx);
  halos[central_idx].galaxy->InfallingGas = (float)infallingMass;

  DEBUG_LOG("Infall: Mvir=%.3e, HaloBaryonFrac=%.4f, InfallingGas=%.3e, z=%.3f",
            halos[central_idx].Mvir,
            halos[central_idx].galaxy->HaloBaryonFraction,
            infallingMass, ctx->redshift);

  return 0;
}

/**
 * @brief   Cleanup sage_calculate_infall module
 *
 * @return  0 on success
 */
static int sage_calculate_infall_cleanup(void) {
  VERBOSE_LOG("SAGE calculate infall module cleaned up");
  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/* Extern reference to generated loop mode array */
extern const enum ProcessingMode sage_calculate_infall_supported_modes[];

static struct Module sage_calculate_infall_module = {
    .name = "sage_calculate_infall",
    .init = sage_calculate_infall_init,
    .process = sage_calculate_infall_process,
    .cleanup = sage_calculate_infall_cleanup,
    .supported_processing_modes = sage_calculate_infall_supported_modes,
    .num_supported_modes = 1  /* Only supports PROCESSING_MODE_FULL_HALO */
};

void sage_calculate_infall_register(void) { module_registry_add(&sage_calculate_infall_module); }
