/**
 * @file    sage26_apply_infall.c
 * @brief   SAGE26 infall application - distributes infalling gas budget to the central's hot or
 * CGM reservoir over substeps
 *
 * Ports SAGE26's add_infall_to_hot (R = the staged SAGE26 source, R/src/model_infall.c:351-414),
 * with its call-site share infallingGas / effective_steps (R/src/core_build_model.c:488):
 * distributes the infalling gas budget from sage26_prepare_infall_budget to the central's selected
 * reservoir over substeps with metallicity tracking. For negative infall (mass loss), removes from
 * the ejected reservoir first, then from the selected reservoir. The required parameter CGMrecipeOn
 * selects it: the central's CGM pair (CGMgas, MetalsCGMgas) with CGMrecipeOn 1 and Regime 0
 * (R/src/model_infall.c:371-385), and its hot pair otherwise (:386-399 with CGMrecipeOn 1, :400-413
 * with CGMrecipeOn 0, the same arithmetic). Positive infall adds gas and no metals.
 *
 * Every floating-point expression is transcribed from SAGE26 with its operand
 * order and types, so that compiler contraction treats both alike.
 *
 * Reference: Croton et al. (2006, 2016)
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "shared/central_link.h"
#include "shared/metallicity.h"
#include "shared/sage_cgm_routing.h"
#include "module_system/parameter_helpers.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

#define MODULE_NAME "sage26_apply_infall"

/* SAGE26's CGMrecipeOn (0 hot-gas routing, 1 regime-aware CGM routing), required and
 * checked at init */
static int CGM_RECIPE_ON;

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_apply_infall_init(void) {
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (mimic_sage_validate_cgm_recipe_on(MODULE_NAME, CGM_RECIPE_ON) != 0) {
    return -1;
  }

  /* Dependency check: sage26_prepare_infall_budget must be present in pre_timestep */
  if (!module_configured_in_phase("sage26_prepare_infall_budget", MimicConfig.pre_timestep,
                                  MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO)) {
    ERROR_LOG("sage26_apply_infall requires sage26_prepare_infall_budget in "
              "pre_timestep as process_full_halo — InfallingGas will be 0 "
              "without it, producing no infall");
    return -1;
  }

  VERBOSE_LOG("SAGE apply infall module initialized");
  VERBOSE_LOG("  CGMrecipeOn = %d", CGM_RECIPE_ON);
  return 0;
}

int sage26_apply_infall_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
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

  struct GalaxyData *galaxy = halos[central_idx].galaxy;

  // The per-substep share of the snapshot budget (core_build_model.c:488)
  double infallingGas = galaxy->InfallingGas / (double)ctx->num_substeps;

  // SAGE parity: add_infall_to_hot removes from ejected first, then from the selected (CGM or
  // hot) reservoir, with a float metallicity and double products accumulated into float
  // reservoirs, no intermediate rounding (R/src/model_infall.c:354-368, :376-384, :390-398,
  // :404-412). The residual recovered when the ejected gas runs out is the already narrowed
  // negative float EjectedGas (:362-364).
  if (infallingGas < 0.0 && galaxy->EjectedGas > 0.0f) {
    const float metallicity = mimic_get_metallicity(galaxy->EjectedGas, galaxy->MetalsEjectedGas);
    galaxy->MetalsEjectedGas += infallingGas * metallicity;
    if (galaxy->MetalsEjectedGas < 0.0f) {
      galaxy->MetalsEjectedGas = 0.0f;
    }

    galaxy->EjectedGas += infallingGas;
    if (galaxy->EjectedGas < 0.0f) {
      infallingGas = galaxy->EjectedGas;
      galaxy->EjectedGas = 0.0f;
      galaxy->MetalsEjectedGas = 0.0f;
    } else {
      infallingGas = 0.0;
    }
  }

  // The central's CGM pair for CGMrecipeOn 1 and Regime 0, else its hot pair
  // (R/src/model_infall.c:371-372, :386, :400). The three branches of SAGE26 share one
  // arithmetic: the selected metals are debited only while positive and floored at zero, the
  // signed gas added, and both selected fields cleared if the gas becomes negative.
  const int to_cgm =
      (CGM_RECIPE_ON == MIMIC_SAGE_CGM_RECIPE_ON && galaxy->Regime == MIMIC_SAGE_REGIME_CGM);
  float *const gas = to_cgm ? &galaxy->CGMgas : &galaxy->HotGas;
  float *const metals = to_cgm ? &galaxy->MetalsCGMgas : &galaxy->MetalsHotGas;

  // If the halo has lost mass, debit the selected metals first (:376-380, :390-394, :404-408)
  if (infallingGas < 0.0 && *metals > 0.0f) {
    const float metallicity = mimic_get_metallicity(*gas, *metals);
    *metals += infallingGas * metallicity;
    if (*metals < 0.0f) {
      *metals = 0.0f;
    }
  }

  // Add (subtract) the infalling gas; a negative result clears both (:383-384, :397-398,
  // :411-412)
  *gas += infallingGas;
  if (*gas < 0.0f) {
    *gas = 0.0f;
    *metals = 0.0f;
  }

  DEBUG_LOG("AddInfall: Mvir=%.3e, InfallingGas/step=%.3e, %s=%.3e, substep=%d/%d",
            halos[central_idx].Mvir, infallingGas, to_cgm ? "CGMgas" : "HotGas", *gas,
            ctx->substep_number + 1, ctx->num_substeps);

  return 0;
}

int sage26_apply_infall_cleanup(void) {
  VERBOSE_LOG("SAGE apply infall module cleaned up");
  return 0;
}
