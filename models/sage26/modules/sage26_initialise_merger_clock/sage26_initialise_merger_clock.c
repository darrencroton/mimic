/**
 * @file    sage26_initialise_merger_clock.c
 * @brief   SAGE26's merger-clock and infall assignment of progenitor joining
 *
 * SAGE26 assigns merger clocks and infall values while it joins a halo's
 * progenitor galaxies, from each galaxy's Type before joining (its Type at the end
 * of the previous snapshot) and the Type joining gives it
 * (R/src/core_build_model.c:240-338, R = the staged SAGE26 source). Mimic's core
 * performs the joining itself (src/core/inheritance.c) and assigns the new Type,
 * the halo values and the three core infall values; this module makes SAGE26's
 * remaining joining writes once per FoF group in pre_timestep, reading the old Type
 * from the package property PreviousType that sage26_record_transition_state wrote
 * at the end of the previous snapshot:
 *
 *   previous Type 2 (or 3)  nothing: SAGE26 copies a Type 2 unchanged (B:240), and
 *                           the core drops a merged galaxy
 *   new Type 0              MergTime = 999.9f (B:288)
 *   new Type 1              previous Type 0: infallStellarMassState = StellarMass and
 *                           TimeOfInfallState = this snapshot's number (B:306-313);
 *                           previous Type 0 or MergTime > 999: MergTime =
 *                           estimate_merging_time(this halo, FoF's first) (B:315-318)
 *   new Type 2 (orphan)     MergTime > 999 or previous Type 0: MergTime = 0 and
 *                           infallStellarMassState = StellarMass (B:327-335)
 *
 * where B is R/src/core_build_model.c. A galaxy with no previous snapshot
 * (PreviousType -1) is one the core created this snapshot, a Type 0 whose clock is
 * already 999.9f; it takes the rows above with "previous Type 0" false, which
 * leaves it unchanged. The clock half of each condition depends on MergTime alone,
 * so without the recorder (PreviousType -1 throughout) clocks are still assigned,
 * but the infall state of a Type 0 -> 1 or 0 -> 2 transition is not: init() warns.
 *
 * The infall values the core writes (infallMvir, infallVvir and infallVmax for a
 * Type 0 that becomes a Type 1 or 2, src/core/inheritance.c:69-73, :88-92) and the
 * one case where SAGE26 also writes them and the core does not (a Type 1 with a
 * clock above 999 losing its subhalo, B:327-333, unreachable on R0 because every
 * Type 1 holds an estimated clock) are core-owned and not changed here.
 *
 * The clock estimate is shared/sage_merger_clock.h, with the run parameter
 * MergerTimeFactor (SAGE26 default 2.0, R/src/core_read_parameter_file.c:128,
 * which R0 does not override).
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "module_system/parameter_helpers.h"
#include "shared/central_link.h"
#include "shared/sage_constants.h"
#include "shared/sage_merger_clock.h"

static const char *MODULE_NAME = "sage26_initialise_merger_clock";
static const char *RECORDER_NAME = "sage26_record_transition_state";

// ============================================================================
// MODULE-LOCAL CONSTANTS AND PARAMETERS
// ============================================================================

/* SAGE parity: SAGE26's default MergerTimeFactor (R/src/core_read_parameter_file.c:128),
 * the value R0 runs with (R0.par does not set it) */
static const double MERGER_TIME_FACTOR_DEFAULT = 2.0;

/* SAGE parity: joining's "unset clock" tests compare the float clock with 999
 * (R/src/core_build_model.c:315 as float, :327 as double; both exact for a float
 * clock) */
static const float MERGTIME_UNSET_ABOVE = 999.0f;

static double MERGER_TIME_FACTOR = 2.0;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Require exactly one pre_timestep process_full_halo entry and no other FoF entry
 *
 * The joining writes happen once per snapshot, before any physics: an entry in a
 * substep phase or in post_timestep would re-estimate a new satellite's clock after
 * the merger pass had started decrementing it.
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
 * @brief SAGE26's joining writes for one galaxy row (R/src/core_build_model.c:284-338)
 *
 * @return 0 on success, -1 when a new satellite has no FoF central to merge into
 */
static int join_galaxy(struct Halo *halos, const int i, const int fof_central_idx,
                       const struct ModuleContext *ctx) {
  struct GalaxyData *gal = halos[i].galaxy;
  const int previous_type = gal->PreviousType;

  // SAGE parity: joining's assignments run only for a galaxy that was a Type 0 or 1
  // (R/src/core_build_model.c:240); a Type 2 is copied unchanged
  if (previous_type == 2 || previous_type == 3) {
    return 0;
  }

  switch (halos[i].Type) {
  case 0:
    // SAGE parity: every FoF central's clock is reset (R/src/core_build_model.c:288)
    gal->MergTime = SAGE_MERGTIME_UNSET;
    break;

  case 1:
    if (previous_type == 0) {
      // SAGE parity: the infall values of a central becoming a satellite
      // (R/src/core_build_model.c:306-313); the core writes infallMvir/Vvir/Vmax, the package the
      // two package members. TimeOfInfall is the descendant halo's snapshot number as a float
      // (R/src/core_build_model.c:311)
      gal->infallStellarMassState = gal->StellarMass;
      gal->TimeOfInfallState = (float)ctx->snapshot_number;
    }
    // SAGE parity: R/src/core_build_model.c:315-318, the Type test first, then the float clock test
    if (previous_type == 0 || gal->MergTime > MERGTIME_UNSET_ABOVE) {
      if (fof_central_idx < 0) {
        ERROR_LOG("%s: satellite %lld has no Type 0 FoF central to estimate its clock against",
                  MODULE_NAME, halos[i].HaloNr);
        return -1;
      }
      const double mergtime =
          mimic_sage_estimate_merging_time(&halos[i], &halos[fof_central_idx], ctx->redshift,
                                           MERGER_TIME_FACTOR, ctx->params->G, NULL);
      gal->MergTime = mergtime;
      DEBUG_LOG("Satellite %lld: MergTime %.6g (previous Type %d)", halos[i].HaloNr, mergtime,
                previous_type);
    }
    break;

  case 2:
    // SAGE parity: R/src/core_build_model.c:327-335, the clock test first (as double), then the
    // Type test
    if (gal->MergTime > (double)MERGTIME_UNSET_ABOVE || previous_type == 0) {
      gal->MergTime = 0.0f;
      gal->infallStellarMassState = gal->StellarMass;
    }
    break;

  default:
    break;
  }

  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_initialise_merger_clock_init(void) {
  if (check_placement() != 0) {
    return -1;
  }

  // MergerTimeFactor is optional: SAGE26 registers it with a default
  // (R/src/core_read_parameter_file.c:128), so a run file may leave it out
  MERGER_TIME_FACTOR = MERGER_TIME_FACTOR_DEFAULT;
  if (model_has_param("MergerTimeFactor")) {
    LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("MergerTimeFactor", MERGER_TIME_FACTOR, 0.0, 1000.0,
                                      "dynamical-friction merger clock scaling");
  }

  if (!module_configured_in_phase(RECORDER_NAME, MimicConfig.post_timestep,
                                  MimicConfig.num_post_timestep, PROCESSING_MODE_FULL_HALO)) {
    WARNING_LOG("%s runs without %s in modules.post_timestep: merger clocks are assigned, but "
                "infallStellarMass and TimeOfInfall are not recorded at infall",
                MODULE_NAME, RECORDER_NAME);
  }

  VERBOSE_LOG("SAGE initialise merger clock initialized");
  VERBOSE_LOG("  MergerTimeFactor = %.3f", MERGER_TIME_FACTOR);
  return 0;
}

int sage26_initialise_merger_clock_process(struct ModuleContext *ctx, struct Halo *halos,
                                           int ngal) {
  if (ctx == NULL || ctx->params == NULL) {
    ERROR_LOG("Invalid merger-clock context (ctx=%p)", (void *)ctx);
    return -1;
  }

  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  // SAGE parity: the mother halo is the FoF's first halo (R/src/core_build_model.c:317), whose
  // galaxy row is the FoF's Type 0
  const int fof_central_idx = mimic_find_fof_central_index(halos, ngal);

  for (int i = 0; i < ngal; i++) {
    if (halos[i].galaxy == NULL) {
      continue;
    }
    if (join_galaxy(halos, i, fof_central_idx, ctx) != 0) {
      return -1;
    }
  }

  return 0;
}

int sage26_initialise_merger_clock_cleanup(void) {
  VERBOSE_LOG("SAGE initialise merger clock cleaned up");
  return 0;
}
