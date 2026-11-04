/**
 * @file    sage26_record_transition_state.c
 * @brief   Records each galaxy's final Type for the next snapshot's joining, and
 *          writes SAGE26's infall outputs
 *
 * SAGE26's progenitor joining decides a galaxy's merger clock and infall values
 * from its Type before joining, the Type it ended the previous snapshot with
 * (R/src/core_build_model.c:240-338, R = the staged SAGE26 source). Mimic's core
 * assigns the new Type while it copies the galaxy (src/core/inheritance.c:66-75,
 * :79-95) and does not publish the old one. This module, the last post_timestep
 * module, records each galaxy row's final Type in the package property
 * PreviousType; the galaxy carries it into the next snapshot, where
 * sage26_initialise_merger_clock reads it in pre_timestep before anything changes
 * it.
 *
 * It also applies SAGE26's output rule for the two package infall values
 * (R/src/io/save_gals_hdf5.c:1004-1016): a Type 0 row is written with
 * infallStellarMass and TimeOfInfall 0, any other row with the values the galaxy
 * holds. SAGE26 keeps the members of a central (the non-main-branch transition to
 * Type 2 keeps an earlier TimeOfInfall, R/src/core_build_model.c:327-335), so the
 * package keeps them in the private infallStellarMassState and TimeOfInfallState
 * and sets the output properties from them here, once the final Type is known:
 *
 *   PreviousType      = Type
 *   infallStellarMass = Type != 0 ? infallStellarMassState : 0
 *   TimeOfInfall      = Type != 0 ? TimeOfInfallState : 0
 *
 * Merged rows (Type 3) are recorded too; the core drops them at the next copy.
 * init() requires exactly one post_timestep entry, as process_full_halo and last
 * in the phase, and none in pre_timestep or any substep phase: any later module
 * could still change a Type, and an earlier entry would record a Type the merger
 * pass has not finished changing.
 */

#include <string.h>

#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

static const char *MODULE_NAME = "sage26_record_transition_state";

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Require one post_timestep process_full_halo entry, last in the phase, and no other
 */
static int check_placement(void) {
  if (module_count_phase_entries(MODULE_NAME, MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO,
                                 NULL) > 0) {
    ERROR_LOG("%s is configured in modules.pre_timestep; it must run only as the last "
              "modules.post_timestep entry, after every module that can change a Type",
              MODULE_NAME);
    return -1;
  }
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    if (module_count_phase_entries(MODULE_NAME, phase->modules, phase->num_modules,
                                   PROCESSING_MODE_FULL_HALO, NULL) > 0) {
      ERROR_LOG("%s is configured in substep phase '%s'; it must run only as the last "
                "modules.post_timestep entry, after every module that can change a Type",
                MODULE_NAME, phase->name != NULL ? phase->name : "(unnamed)");
      return -1;
    }
  }
  int full_halo_entries = 0;
  const int entries = module_count_phase_entries(MODULE_NAME, MimicConfig.post_timestep,
                                                 MimicConfig.num_post_timestep,
                                                 PROCESSING_MODE_FULL_HALO, &full_halo_entries);
  if (entries != 1 || full_halo_entries != 1) {
    ERROR_LOG("%s must be configured exactly once in modules.post_timestep as process_full_halo "
              "(found %d entries, %d as process_full_halo)",
              MODULE_NAME, entries, full_halo_entries);
    return -1;
  }
  const struct PhaseModuleConfig *last =
      &MimicConfig.post_timestep[MimicConfig.num_post_timestep - 1];
  if (last->module_name == NULL || strcmp(last->module_name, MODULE_NAME) != 0) {
    ERROR_LOG("%s must be the last modules.post_timestep entry (the last is %s)", MODULE_NAME,
              last->module_name != NULL ? last->module_name : "(unnamed)");
    return -1;
  }
  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_record_transition_state_init(void) {
  if (check_placement() != 0) {
    return -1;
  }
  VERBOSE_LOG("SAGE record transition state module initialized");
  return 0;
}

int sage26_record_transition_state_process(struct ModuleContext *ctx, struct Halo *halos,
                                           int ngal) {
  (void)ctx;
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  for (int i = 0; i < ngal; i++) {
    // Workspace rows without a galaxy are not galaxies
    if (halos[i].galaxy == NULL) {
      continue;
    }
    struct GalaxyData *gal = halos[i].galaxy;
    const int type = halos[i].Type;

    gal->PreviousType = type;

    // SAGE parity: the writer stores the members for Type != 0 and 0.0 for a Type 0
    // (R/src/io/save_gals_hdf5.c:1004-1016), without changing the members
    if (type != 0) {
      gal->infallStellarMass = gal->infallStellarMassState;
      gal->TimeOfInfall = gal->TimeOfInfallState;
    } else {
      gal->infallStellarMass = 0.0f;
      gal->TimeOfInfall = 0.0f;
    }
  }

  return 0;
}

int sage26_record_transition_state_cleanup(void) {
  VERBOSE_LOG("SAGE record transition state module cleaned up");
  return 0;
}
