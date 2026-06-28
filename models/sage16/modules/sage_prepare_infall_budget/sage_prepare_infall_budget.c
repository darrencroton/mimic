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

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "shared/central_link.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double GLOBAL_BARYON_FRAC;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Clamp mass and metal components to ensure conservation and non-negativity
 */
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

/**
 * @brief Consolidate satellite ejected gas and ICS to the central; compute cosmological infall
 *
 * Returns HaloBaryonFraction × Mvir minus total baryon content across the FoF group.
 * Satellite ejected gas and ICS are surrendered to the central before the infall
 * calculation; hot gas is NOT consolidated (orphans cool their own hot reservoir
 * independently until merging — SAGE parity).
 */
static double infall_recipe(struct Halo *halos, int ngal, int central_idx) {
  double tot_stellarMass = 0.0;
  double tot_BHMass = 0.0;
  double tot_coldMass = 0.0;
  double tot_hotMass = 0.0;
  double tot_ICS = 0.0;
  double tot_ejected = 0.0;
  double tot_ICSMetals = 0.0;
  double tot_ejectedMetals = 0.0;

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

  /* Ordering: sage_reionization is optional (HaloBaryonFraction falls back to
     GlobalBaryonFraction when unset), but when configured in pre_timestep it must run
     earlier so HaloBaryonFraction is set before the infall budget reads it. */
  if (module_configured_in_phase("sage_reionization", MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO) &&
      !module_precedes_in_phase("sage_reionization", "sage_prepare_infall_budget",
                                MimicConfig.pre_timestep, MimicConfig.num_pre_timestep)) {
    ERROR_LOG("sage_prepare_infall_budget requires sage_reionization to run earlier in "
              "pre_timestep — the infall budget would read HaloBaryonFraction before "
              "reionization sets it");
    return -1;
  }

  VERBOSE_LOG("SAGE prepare infall budget module initialized");
  VERBOSE_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);

  return 0;
}

int sage_prepare_infall_budget_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  // No FOF central is a legal no-op (the tree build enforces centrals upstream)
  const int central_idx = mimic_find_fof_central_index(halos, ngal);
  if (central_idx == -1) {
    return 0;
  }

  if (halos[central_idx].galaxy == NULL) {
    ERROR_LOG("Central galaxy (index %d) has NULL galaxy data", central_idx);
    return -1;
  }

  // -1.0 sentinel: first snapshot for this central; fall back to GlobalBaryonFraction
  if (halos[central_idx].galaxy->HaloBaryonFraction == -1.0) {
    halos[central_idx].galaxy->HaloBaryonFraction = GLOBAL_BARYON_FRAC;
  }

  // Double property: SAGE keeps InfallingGas as a double local through the whole snapshot interval
  const double infallingMass = infall_recipe(halos, ngal, central_idx);
  halos[central_idx].galaxy->InfallingGas = infallingMass;

  DEBUG_LOG("Infall: Mvir=%.3e, HaloBaryonFrac=%.4f, InfallingGas=%.3e, z=%.3f",
            halos[central_idx].Mvir, halos[central_idx].galaxy->HaloBaryonFraction, infallingMass,
            ctx->redshift);

  return 0;
}

int sage_prepare_infall_budget_cleanup(void) {
  VERBOSE_LOG("SAGE prepare infall budget module cleaned up");
  return 0;
}
