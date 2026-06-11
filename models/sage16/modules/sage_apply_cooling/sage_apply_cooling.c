/**
 * @file    sage_apply_cooling.c
 * @brief   SAGE cooling application - commits CoolingGas budget to cold reservoir
 *
 * Transfers gas from hot to cold reservoir based on CoolingGas property
 * (calculated by sage_calculate_cooling_budget and modified by sage_radio_mode_heating).
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include "constants.h"
#include "error.h"
#include "shared/metallicity.h"
#include "module_interface.h"
#include "module_registry.h"
#include "globals.h"
#include "types.h"

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Transfers cooled gas from hot halo to cold disk
 */
static void cool_gas_onto_galaxy(struct Halo *halo, const double coolingGas, const float vvir) {
  if (coolingGas > 0.0) {
    if (coolingGas < halo->galaxy->HotGas) {
      const double metallicity =
          mimic_get_metallicity(halo->galaxy->HotGas, halo->galaxy->MetalsHotGas);
      halo->galaxy->ColdGas += coolingGas;
      halo->galaxy->MetalsColdGas += metallicity * coolingGas;
      halo->galaxy->HotGas -= coolingGas;
      halo->galaxy->MetalsHotGas -= metallicity * coolingGas;
    } else {
      halo->galaxy->ColdGas += halo->galaxy->HotGas;
      halo->galaxy->MetalsColdGas += halo->galaxy->MetalsHotGas;
      halo->galaxy->HotGas = 0.0;
      halo->galaxy->MetalsHotGas = 0.0;
    }

    // Track cooling energy: E_cool = 0.5 * m * V_vir^2
    // Only calculate cooling rate if dT is valid (> 0)
    // Newly formed halos (no progenitor) have dT = -1.0 (sentinel value)
    if (halo->dT > 0.0) {
      halo->galaxy->Cooling += (0.5 * coolingGas * vvir * vvir) / halo->dT;
    }
  }
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_apply_cooling_init(void) {
  /* Dependency check: sage_calculate_cooling_budget must precede this module
   * in the same substep phase */
  if (!module_precedes_in_substep_phase("sage_calculate_cooling_budget", PROCESSING_MODE_BY_GALAXY,
                                        "sage_apply_cooling", PROCESSING_MODE_BY_GALAXY)) {
    ERROR_LOG("sage_apply_cooling requires sage_calculate_cooling_budget to "
              "precede it in the same substep phase — CoolingGas will be 0 without it");
    return -1;
  }

  INFO_LOG("SAGE apply cooling module initialized");
  return 0;
}

int sage_apply_cooling_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  (void)ctx; // Unused in this module

  if (ngal != 1) {
    ERROR_LOG("process_by_galaxy expects ngal=1, got %d", ngal);
    return -1;
  }

  struct Halo *halo = &halos[0];

  // SAGE parity: cooling is applied for every non-merged galaxy, including
  // Type 2 orphans (they cool their own hot gas until merging). CoolingGas is
  // 0 for galaxies the budget module found could not cool.
  if (halo->galaxy == NULL) {
    return 0;
  }

  const double coolingGas = (double)halo->galaxy->CoolingGas;

  // SAGE parity: cool_gas_onto_galaxy applies for any coolingGas > 0.0
  if (coolingGas > 0.0) {
    cool_gas_onto_galaxy(halo, coolingGas, halo->Vvir);
  }

  return 0;
}

int sage_apply_cooling_cleanup(void) {
  VERBOSE_LOG("SAGE apply cooling module cleaned up");
  return 0;
}
