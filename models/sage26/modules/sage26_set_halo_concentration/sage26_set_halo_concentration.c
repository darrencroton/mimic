/**
 * @file    sage26_set_halo_concentration.c
 * @brief   SAGE26's halo-concentration assignment of progenitor joining (ConcentrationOn 0 or 3)
 *
 * SAGE26 assigns a galaxy's NFW concentration while it joins a halo's progenitor galaxies, inside
 * the FoF-central branch, before it sets Type = 0 (R/src/core_build_model.c:290-295, R = the
 * staged SAGE26 source):
 *
 *   if (ConcentrationOn > 0)
 *     Concentration = (float)get_halo_concentration(ngal, ZZ[SnapNum], galaxies, run_params);
 *
 * The branch is reached only by a galaxy that was a Type 0 or 1 before joining (:240), whose
 * progenitor is the first-occupied one (:252) and whose halo is its FoF's first (:284), so its
 * Type at the call is the old one. Mimic's core performs the joining (src/core/inheritance.c) and
 * gives that galaxy Type 0; this module makes SAGE26's concentration write once per FoF group in
 * pre_timestep, immediately after sage26_initialise_merger_clock (SAGE26's order: the clock at
 * :288, the concentration at :290-294, the disk and bulge radii at :297-298). The old Type is the
 * package property PreviousType, which sage26_record_transition_state wrote at the end of the
 * previous snapshot. With ConcentrationOn 3, for each row whose new Type is 0:
 *
 *   PreviousType 0 or 1   Concentration = (float) the inversion of the pair the mode-3 dispatch
 *                         selects by PreviousType: the current float Vmax and Vvir for 0, the
 *                         infall-frozen float infallVmax and infallVvir for 1
 *                         (R/src/model_halo_properties.c:339-350; shared/sage_halo_concentration.h)
 *   PreviousType -1       kept: a galaxy the core created this snapshot holds its creation
 *                         value 0, as SAGE26's init_galaxy is not followed by the call
 *                         (R/src/model_misc.c:93, R/src/core_build_model.c:347-351)
 *   PreviousType 2 or 3   kept: SAGE26's joining passes a former orphan over (:240)
 *
 * and every row whose new Type is not 0 keeps its value: a satellite or orphan carries its last
 * central value (Concentration is inherited, with no init_repeat). The core's double Vmax, Vvir,
 * infallVmax and infallVvir are each narrowed to SAGE26's float and widened on their own. Mode 3
 * does not read the redshift SAGE26 passes. With ConcentrationOn 0 the module writes nothing, as
 * SAGE26 makes no call, and every row keeps its creation value 0.
 *
 * init() refuses: a missing ConcentrationOn (required, no default), a value
 * other than 0 or 3, any placement but exactly one pre_timestep entry as process_full_halo with
 * no other entry, and, with ConcentrationOn 3, a run without sage26_record_transition_state in
 * post_timestep, which writes PreviousType (without it every central would silently keep 0).
 */

#include <math.h>
#include <stddef.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "module_system/parameter_helpers.h"
#include "shared/sage_halo_concentration.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static const char *MODULE_NAME = "sage26_set_halo_concentration";
static const char *RECORDER_NAME = "sage26_record_transition_state";

/* SAGE26's ConcentrationOn (0 off, 3 the Vmax/Vvir inversion), required and checked at init */
static int CONCENTRATION_ON = MIMIC_SAGE_CONCENTRATION_OFF;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Require exactly one pre_timestep process_full_halo entry and no other FoF entry
 *
 * SAGE26 assigns the concentration once per snapshot, at progenitor joining: an entry in a
 * substep phase or in post_timestep, a second pre_timestep entry or another processing mode would
 * run it where SAGE26 does not.
 *
 * @return 0 when placed correctly; -1 otherwise
 */
static int check_placement(void) {
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    if (module_count_phase_entries(MODULE_NAME, phase->modules, phase->num_modules,
                                   PROCESSING_MODE_FULL_HALO, NULL) > 0) {
      ERROR_LOG("%s is configured in substep phase '%s'; it must run only in "
                "modules.pre_timestep, once per snapshot at progenitor joining",
                MODULE_NAME, phase->name != NULL ? phase->name : "(unnamed)");
      return -1;
    }
  }
  if (module_count_phase_entries(MODULE_NAME, MimicConfig.post_timestep,
                                 MimicConfig.num_post_timestep, PROCESSING_MODE_FULL_HALO,
                                 NULL) > 0) {
    ERROR_LOG("%s is configured in modules.post_timestep; it must run only in "
              "modules.pre_timestep, once per snapshot at progenitor joining",
              MODULE_NAME);
    return -1;
  }
  int full_halo_entries = 0;
  const int entries = module_count_phase_entries(MODULE_NAME, MimicConfig.pre_timestep,
                                                 MimicConfig.num_pre_timestep,
                                                 PROCESSING_MODE_FULL_HALO, &full_halo_entries);
  if (entries != 1 || full_halo_entries != 1) {
    ERROR_LOG("%s must be configured exactly once in modules.pre_timestep as process_full_halo "
              "(found %d entries, %d as process_full_halo)",
              MODULE_NAME, entries, full_halo_entries);
    return -1;
  }
  return 0;
}

/**
 * @brief Refuse ConcentrationOn 3 without the recorder of PreviousType
 *
 * The mode-3 assignment selects by the Type before joining, which only
 * sage26_record_transition_state records (in post_timestep). Without it every galaxy holds
 * PreviousType -1 and no central would ever be assigned. With ConcentrationOn 0 nothing is read,
 * so the check does not apply.
 *
 * @return 0 when satisfied; -1 otherwise
 */
static int check_recorder(void) {
  if (CONCENTRATION_ON != MIMIC_SAGE_CONCENTRATION_VMAX_VVIR) {
    return 0;
  }
  if (!module_configured_in_phase(RECORDER_NAME, MimicConfig.post_timestep,
                                  MimicConfig.num_post_timestep, PROCESSING_MODE_FULL_HALO)) {
    ERROR_LOG("%s: ConcentrationOn = 3 requires %s in modules.post_timestep as "
              "process_full_halo: it records each galaxy's Type for the next snapshot "
              "(PreviousType), which the assignment selects by; without it no central would be "
              "assigned a concentration",
              MODULE_NAME, RECORDER_NAME);
    return -1;
  }
  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_set_halo_concentration_init(void) {
  /* Required, no default: a missing key fails here */
  LOAD_PARAM_INT("ConcentrationOn", CONCENTRATION_ON);
  if (mimic_sage_validate_concentration_on(MODULE_NAME, CONCENTRATION_ON) != 0) {
    return -1;
  }
  if (check_placement() != 0) {
    return -1;
  }
  if (check_recorder() != 0) {
    return -1;
  }

  VERBOSE_LOG("SAGE set halo concentration module initialized");
  VERBOSE_LOG("  ConcentrationOn = %d%s", CONCENTRATION_ON,
              CONCENTRATION_ON == MIMIC_SAGE_CONCENTRATION_OFF
                  ? " (no concentration: Concentration keeps its creation value 0)"
                  : " (the Vmax/Vvir inversion)");
  return 0;
}

int sage26_set_halo_concentration_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  (void)ctx;

  // SAGE parity (R/src/core_build_model.c:291): no call with ConcentrationOn 0
  if (CONCENTRATION_ON == MIMIC_SAGE_CONCENTRATION_OFF) {
    return 0;
  }
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  for (int i = 0; i < ngal; i++) {
    struct GalaxyData *gal = halos[i].galaxy;
    // SAGE parity (R/src/core_build_model.c:284-300): only the FoF central of the joined halo,
    // whose new Type is 0, is assigned
    if (gal == NULL || halos[i].Type != 0) {
      continue;
    }
    const int previous_type = gal->PreviousType;
    // SAGE parity: a galaxy created this snapshot (-1) keeps its creation value 0
    // (R/src/model_misc.c:93, R/src/core_build_model.c:347-351), and joining passes a former
    // Type 2 over (:240)
    if (previous_type != 0 && previous_type != 1) {
      continue;
    }
    // SAGE parity (R/src/core_build_model.c:292-294, R/src/model_halo_properties.c:339-350): the
    // dispatch selects by the Type before joining; each core double is SAGE26's float
    const double concentration = mimic_sage_get_halo_concentration(
        CONCENTRATION_ON, previous_type, (float)halos[i].Vmax, (float)halos[i].Vvir,
        (float)halos[i].infallVmax, (float)halos[i].infallVvir, NULL);
    gal->Concentration = (float)concentration;
  }

  return 0;
}

int sage26_set_halo_concentration_cleanup(void) {
  VERBOSE_LOG("SAGE set halo concentration module cleaned up");
  return 0;
}
