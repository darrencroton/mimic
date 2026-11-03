/**
 * @file    sage26_prepare_infall_budget.c
 * @brief   SAGE26 infall budget preparation - consolidates satellite reservoirs and computes
 * cosmological gas infall
 *
 * Ports SAGE26's infall_recipe (model_infall.c:67-213) on the R0 path (CGMrecipeOn 0):
 * sums every galaxy's baryons in the FoF group (CGM included), surrenders the
 * satellites' ejected gas, ICS (with its assembly history) and CGM to the central,
 * computes the infall budget HaloBaryonFraction * Mvir minus those baryons, and
 * applies SAGE26's consolidation clamps in its order. Stores the budget in
 * InfallingGas for sage26_apply_infall.
 *
 * Every floating-point expression is transcribed from SAGE26 with its operand
 * order and types, so that compiler contraction treats both alike.
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
 * @brief SAGE26 infall_recipe (model_infall.c:67-213), CGMrecipeOn 0
 *
 * Returns HaloBaryonFraction * Mvir minus the FoF group's total baryons (CGM
 * included). HaloBaryonFraction is SAGE26's once-rounded reionization_modifier *
 * BaryonFrac, carried from sage26_reionization; the remaining "* Mvir - S" stays
 * one expression, so it contracts to the single fused multiply-subtract SAGE26's
 * build performs (model_infall.c:128-130).
 */
static double infall_recipe(struct Halo *halos, int ngal, int central_idx) {
  double tot_stellarMass = 0.0;
  double tot_BHMass = 0.0;
  double tot_coldMass = 0.0;
  double tot_hotMass = 0.0;
  double tot_ejected = 0.0;
  double tot_ejectedMetals = 0.0;
  double tot_ICS = 0.0;
  double tot_ICSMetals = 0.0;
  double tot_CGMgas = 0.0;
  double tot_MetalsCGMgas = 0.0;

  struct GalaxyData *central = halos[central_idx].galaxy;

  // Add up all the baryonic mass associated with the full halo (model_infall.c:78-119)
  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL || halos[i].Type == 3) {
      continue;
    }

    struct GalaxyData *gal = halos[i].galaxy;

    tot_stellarMass += gal->StellarMass;
    tot_BHMass += gal->BlackHoleMass;
    tot_coldMass += gal->ColdGas;
    tot_hotMass += gal->HotGas;
    tot_ejected += gal->EjectedGas;
    tot_ejectedMetals += gal->MetalsEjectedGas;
    tot_ICS += gal->ICS;
    tot_ICSMetals += gal->MetalsICS;
    tot_CGMgas += gal->CGMgas;
    tot_MetalsCGMgas += gal->MetalsCGMgas;

    if (i != central_idx) {
      // SAGE parity (model_infall.c:79-109): satellites surrender only their
      // ejected gas, ICS and CGM to the central. Hot gas is NOT consolidated:
      // Type 2 orphans keep their hot reservoir and cool it themselves until they
      // merge (orphans pass through cooling before destruction,
      // core_build_model.c:473-516).
      gal->EjectedGas = gal->MetalsEjectedGas = 0.0f;

      // A satellite carrying ICS was a former group central: its ICS and its
      // assembly history go to the central's ex-situ (accreted) channel
      // (model_infall.c:96-108). SAGE parity: this runs every snapshot for every
      // satellite, so ICS a progenitor group built in situ is re-booked as
      // accreted once it falls in (model_infall.c:97-100).
      if (gal->ICS > 0.0) {
        central->ICS_accrete += gal->ICS;
        central->ICS_sum_mt += gal->ICS_sum_mt;
        gal->ICS_disrupt = 0.0f;
        gal->ICS_accrete = 0.0f;
        gal->ICS_sum_mt = 0.0f;
      }
      gal->ICS = gal->MetalsICS = 0.0f;

      // CGMrecipeOn != 1: satellite CGM is unused and zeroed (model_infall.c:115-117)
      gal->CGMgas = gal->MetalsCGMgas = 0.0f;
    }
  }

  // Mvir is read as the float SAGE26 holds (core_allvars.h:129)
  const float central_Mvir = (float)halos[central_idx].Mvir;
  const double infallingMass = central->HaloBaryonFraction * central_Mvir -
                               (tot_stellarMass + tot_coldMass + tot_hotMass + tot_ejected +
                                tot_BHMass + tot_ICS + tot_CGMgas);

  // The central galaxy keeps all the ejected mass. SAGE parity
  // (model_infall.c:132-146): SAGE26 caps metals to mass first, then zeros a
  // negative mass and its metals, then floors negative metals.
  central->EjectedGas = tot_ejected;
  central->MetalsEjectedGas = tot_ejectedMetals;

  if (central->MetalsEjectedGas > central->EjectedGas) {
    central->MetalsEjectedGas = central->EjectedGas;
  }

  if (central->EjectedGas < 0.0) {
    central->EjectedGas = central->MetalsEjectedGas = 0.0f;
  }

  if (central->MetalsEjectedGas < 0.0) {
    central->MetalsEjectedGas = 0.0f;
  }

  // SAGE parity (model_infall.c:177-181): with the CGM recipe off, the FoF
  // group's original CGM totals, the central's own CGM included, are added to
  // the central's hot gas, and the central's CGMgas is NOT cleared, so a
  // carried central CGM stays in both reservoirs. This is SAGE26's deliberate
  // source quirk, mirrored; it is inert while CGM stays zero, as R0 keeps it.
  central->HotGas += tot_CGMgas;
  central->MetalsHotGas += tot_MetalsCGMgas;

  // Safety checks on the central's CGM (model_infall.c:183-194)
  if (central->MetalsCGMgas > central->CGMgas) {
    central->MetalsCGMgas = central->CGMgas;
  }

  if (central->CGMgas < 0.0) {
    central->CGMgas = central->MetalsCGMgas = 0.0f;
  }

  if (central->MetalsCGMgas < 0.0) {
    central->MetalsCGMgas = 0.0f;
  }

  // The central galaxy keeps all the ICS, mostly for numerical convenience
  // (model_infall.c:196-210), clamped in the same order as the ejected gas
  central->ICS = tot_ICS;
  central->MetalsICS = tot_ICSMetals;

  if (central->MetalsICS > central->ICS) {
    central->MetalsICS = central->ICS;
  }

  if (central->ICS < 0.0) {
    central->ICS = central->MetalsICS = 0.0f;
  }

  if (central->MetalsICS < 0.0) {
    central->MetalsICS = 0.0f;
  }

  return infallingMass;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_prepare_infall_budget_init(void) {
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("GlobalBaryonFraction", GLOBAL_BARYON_FRAC, 0.0, 1.0,
                                    "cosmic baryon fraction must be physical");

  /* Ordering: sage26_reionization is optional (HaloBaryonFraction falls back to
     GlobalBaryonFraction when unset), but when configured in pre_timestep it must run
     earlier so HaloBaryonFraction is set before the infall budget reads it. */
  if (module_configured_in_phase("sage26_reionization", MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO) &&
      !module_precedes_in_phase("sage26_reionization", "sage26_prepare_infall_budget",
                                MimicConfig.pre_timestep, MimicConfig.num_pre_timestep)) {
    ERROR_LOG("sage26_prepare_infall_budget requires sage26_reionization to run earlier in "
              "pre_timestep — the infall budget would read HaloBaryonFraction before "
              "reionization sets it");
    return -1;
  }

  VERBOSE_LOG("SAGE prepare infall budget module initialized");
  VERBOSE_LOG("  GlobalBaryonFraction = %.4f", GLOBAL_BARYON_FRAC);

  return 0;
}

int sage26_prepare_infall_budget_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
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

  /* -1.0 sentinel: sage26_reionization is not configured (SAGE26 ReionizationOn 0,
   * whose modifier is 1.0, model_infall.c:122-126), so the fraction is the global one. */
  if (halos[central_idx].galaxy->HaloBaryonFraction == -1.0) {
    halos[central_idx].galaxy->HaloBaryonFraction = GLOBAL_BARYON_FRAC;
  }

  // Double property: SAGE26 keeps infallingGas as a double local through the whole snapshot
  // interval (core_build_model.c:415)
  const double infallingMass = infall_recipe(halos, ngal, central_idx);
  halos[central_idx].galaxy->InfallingGas = infallingMass;

  DEBUG_LOG("Infall: Mvir=%.3e, HaloBaryonFrac=%.4f, InfallingGas=%.3e, z=%.3f",
            halos[central_idx].Mvir, halos[central_idx].galaxy->HaloBaryonFraction, infallingMass,
            ctx->redshift);

  return 0;
}

int sage26_prepare_infall_budget_cleanup(void) {
  VERBOSE_LOG("SAGE prepare infall budget module cleaned up");
  return 0;
}
