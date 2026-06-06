/**
 * @file    sage_prepare_infall_budget.c
 * @brief   SAGE infall budget preparation - consolidates satellite reservoirs and computes
 * cosmological gas infall
 *
 * Calculates infalling gas mass for central galaxies based on halo growth and
 * reionization suppression. Also consolidates satellite ejected gas and intracluster
 * stars to centrals, preserving metallicity. Stores result in InfallingGas property.
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "module_system/parameter_helpers.h"
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

// Validate and clamp mass and metal components to ensure physical constraints
static inline void validate_mass_metals(float *mass, float *metals) {
  if (*mass < 0.0f) {
    *mass = 0.0f;
    *metals = 0.0f;
  } else {
    if (*metals < 0.0f)
      *metals = 0.0f;
    if (*metals > *mass)
      *metals = *mass;
  }
}

// Calculate infalling gas mass for central galaxy. Computes gas accretion from
// HaloBaryonFraction × Mvir minus current baryon content. Consolidates ejected gas
// and intracluster stars from satellites to central, preserving metallicity.
static double infall_recipe(struct Halo *halos, int ngal, int central_idx) {
  double tot_stellarMass = 0.0;
  double tot_BHMass = 0.0;
  double tot_coldMass = 0.0;
  double tot_hotMass = 0.0;
  double tot_ICS = 0.0;
  double tot_ejected = 0.0;
  double tot_ICSMetals = 0.0;
  double tot_ejectedMetals = 0.0;

  // Sum baryonic components across all galaxies and transfer satellite reservoirs
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL || halos[i].Type == 3) {
      continue;
    }

    struct GalaxyData *gal = halos[i].galaxy;

    tot_stellarMass += gal->StellarMass;
    tot_BHMass += gal->BlackHoleMass;
    tot_coldMass += gal->ColdGas;
    tot_hotMass += gal->HotGas;
    tot_ICS += gal->ICS;
    tot_ejected += gal->EjectedGas;
    tot_ICSMetals += gal->MetalsICS;
    tot_ejectedMetals += gal->MetalsEjectedGas;

    if (i != central_idx) {
      // SAGE parity (model_infall.c infall_recipe): satellites surrender
      // only their ejected gas and ICS to the central. Hot gas is NOT
      // consolidated — Type 2 orphans keep their hot reservoir and cool it
      // independently until they merge.
      gal->EjectedGas = 0.0f;
      gal->MetalsEjectedGas = 0.0f;

      gal->ICS = 0.0f;
      gal->MetalsICS = 0.0f;
    }
  }

  struct GalaxyData *central = halos[central_idx].galaxy;

  // The central galaxy keeps all the ejected mass
  central->EjectedGas = (float)tot_ejected;
  central->MetalsEjectedGas = (float)tot_ejectedMetals;
  validate_mass_metals(&central->EjectedGas, &central->MetalsEjectedGas);

  // The central galaxy keeps all the ICS (mostly for numerical convenience)
  central->ICS = (float)tot_ICS;
  central->MetalsICS = (float)tot_ICSMetals;
  validate_mass_metals(&central->ICS, &central->MetalsICS);

  // Calculate infalling gas from HaloBaryonFraction
  const double infallingMass =
      central->HaloBaryonFraction * halos[central_idx].Mvir -
      (tot_stellarMass + tot_coldMass + tot_hotMass + tot_ejected + tot_BHMass + tot_ICS);

  return infallingMass;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_prepare_infall_budget_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  INFO_LOG("SAGE prepare infall budget module initialized");
  VERBOSE_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);

  return 0;
}

int sage_prepare_infall_budget_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
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
    ERROR_LOG("No central galaxy found in FOF group (ngal=%d)", ngal);
    return 0;
  }

  if (halos[central_idx].galaxy == NULL) {
    ERROR_LOG("Central galaxy (index %d) has NULL galaxy data", central_idx);
    return -1;
  }

  // Initialize HaloBaryonFraction to GlobalBaryonFraction if first time
  if (halos[central_idx].galaxy->HaloBaryonFraction == -1.0f) {
    halos[central_idx].galaxy->HaloBaryonFraction = (float)GLOBAL_BARYON_FRAC;
  }

  // Calculate and store infalling mass
  const double infallingMass = infall_recipe(halos, ngal, central_idx);
  halos[central_idx].galaxy->InfallingGas = (float)infallingMass;

  DEBUG_LOG("Infall: Mvir=%.3e, HaloBaryonFrac=%.4f, InfallingGas=%.3e, z=%.3f",
            halos[central_idx].Mvir, halos[central_idx].galaxy->HaloBaryonFraction, infallingMass,
            ctx->redshift);

  return 0;
}

int sage_prepare_infall_budget_cleanup(void) {
  VERBOSE_LOG("SAGE prepare infall budget module cleaned up");
  return 0;
}
