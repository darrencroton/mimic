/**
 * @file    sage_add_infall.c
 * @brief   SAGE add infall module implementation
 *
 * Adds infalling gas to hot gas reservoir with metallicity tracking. Reads
 * InfallingGas property set by sage_infall module and distributes it over
 * substeps. For negative infall (mass loss), removes from ejected reservoir
 * first, then hot gas.
 *
 * Physics: Transfer InfallingGas / num_substeps → HotGas per substep
 *
 * Key functions:
 * - sage_add_infall_process(): Add infalling gas to hot reservoir with metallicity
 *
 * Reference: Croton et al. (2006, 2016), based on SAGE model_infall.c add_infall_to_hot()
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include "constants.h"
#include "error.h"
#include "../_shared/metallicity.h"
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_add_infall.h"
#include "types.h"

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize sage_add_infall module
 *
 * @return  0 on success
 */
static int sage_add_infall_init(void) {
  INFO_LOG("SAGE add infall module initialized");
  INFO_LOG("  Physics: Transfer InfallingGas → HotGas with metallicity");
  INFO_LOG("  Distributes InfallingGas over substeps");

  return 0;
}

/**
 * @brief   Process halos in a FOF group
 *
 * Adds infalling gas to hot reservoir for central galaxy only, distributed over substeps.
 *
 * @param   ctx     Module execution context (substep info, time)
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos in FOF group
 * @return  0 on success, non-zero on failure
 */
static int sage_add_infall_process(struct ModuleContext *ctx, struct Halo *halos,
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
    DEBUG_LOG("No central galaxy found in FOF group (ngal=%d)", ngal);
    return 0;
  }

  if (halos[central_idx].galaxy == NULL) {
    ERROR_LOG("Central galaxy (index %d) has NULL galaxy data", central_idx);
    return -1;
  }

  struct GalaxyData *galaxy = halos[central_idx].galaxy;

  /* Calculate infall amount for this substep */
  double infallingGas = (double)galaxy->InfallingGas / (double)ctx->num_substeps;
  float metallicity;

  /* For mass loss (negative infall), first remove from ejected reservoir */
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

  DEBUG_LOG("AddInfall: Mvir=%.3e, InfallingGas/step=%.3e, HotGas=%.3e, substep=%d/%d",
            halos[central_idx].Mvir, infallingGas, galaxy->HotGas, ctx->substep_number + 1,
            ctx->num_substeps);

  return 0;
}

/**
 * @brief   Cleanup sage_add_infall module
 *
 * @return  0 on success
 */
static int sage_add_infall_cleanup(void) {
  INFO_LOG("SAGE add infall module cleaned up");
  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/* Extern reference to generated loop mode array */
extern const enum LoopMode sage_add_infall_supported_modes[];

static struct Module sage_add_infall_module = {
    .name = "sage_add_infall",
    .init = sage_add_infall_init,
    .process = sage_add_infall_process,
    .cleanup = sage_add_infall_cleanup,
    .supported_loop_modes = sage_add_infall_supported_modes,
    .num_supported_modes = 1  /* Only supports LOOP_MODE_ONCE */
};

void sage_add_infall_register(void) { module_registry_add(&sage_add_infall_module); }
