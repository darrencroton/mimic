/**
 * @file    sage26_finalize_sfr_histories.c
 * @brief   SAGE26's end-of-snapshot averaging of the per-substep star-formation histories
 *
 * Disk star formation (sage26_apply_star_formation_supernova) and every burst (the
 * shared kernel in shared/sage_starburst_physics.h) sample the package's copies of
 * SAGE26's six per-substep histories, SfrDisk, SfrBulge and the cold gas and metals
 * sampled with each, ten bins each (R/src/core_allvars.h:158-163, R = the staged
 * SAGE26 source). SAGE26 records the substep count on every galaxy of the halo
 * (R/src/core_build_model.c:466-468) and its HDF5 writer averages the histories into
 * the four output scalars SfrDisk, SfrBulge (Msun/yr), SfrDiskZ and SfrBulgeZ
 * (dimensionless) (R/src/io/save_gals_hdf5.c:934-959).
 *
 * This module does both once per FoF group in post_timestep, after the last substep
 * and its merger phase:
 *
 *   SubstepsUsed = N                     for every galaxy row, merged ones included;
 *   SfrDisk, SfrBulge, SfrDiskZ, SfrBulgeZ = the writer's averaging of the histories
 *                                        (mimic_sage_average_sfr_histories)
 *
 * N is the package's substep count for the halo (ctx->num_substeps), the count the
 * substep phases ran and binned with; SAGE26's is its effective_steps. The four
 * outputs then hold their final values and are written as they are (no
 * output_convert). The averaging reads the run's UnitMass_in_g and UnitTime_in_s,
 * which the core forms as SAGE26 does (R/src/core_init.c:94).
 *
 * It reads and writes no field sage26_finalize_cooling_heating touches (that module
 * normalises Cooling, Heating and SupernovaOutflowRate), so their order in
 * post_timestep does not matter; the run files place it after that module. It must
 * run once per FoF step, after the substeps: in a substep phase it would average
 * histories still being sampled. init() therefore requires exactly one post_timestep
 * entry as process_full_halo and no entry in pre_timestep or any substep phase.
 */

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "shared/sage_sfr_history.h"

static const char *MODULE_NAME = "sage26_finalize_sfr_histories";

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Require exactly one post_timestep process_full_halo entry and no other FoF entry
 *
 * Phases accept repeated entries (the registry initialises a module once), so the
 * entries are counted: an entry in pre_timestep or a substep phase would average
 * histories that are still being sampled, and a second post_timestep entry would
 * repeat the averaging needlessly.
 */
static int check_placement(void) {
  if (module_count_phase_entries(MODULE_NAME, MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO,
                                 NULL) > 0) {
    ERROR_LOG("%s is configured in modules.pre_timestep; it must run only in "
              "modules.post_timestep, once per FoF step after the substeps",
              MODULE_NAME);
    return -1;
  }
  for (int p = 0; p < MimicConfig.num_substep_phases; p++) {
    const struct ModulePhaseConfig *phase = &MimicConfig.substep_phases[p];
    if (module_count_phase_entries(MODULE_NAME, phase->modules, phase->num_modules,
                                   PROCESSING_MODE_FULL_HALO, NULL) > 0) {
      ERROR_LOG("%s is configured in substep phase '%s'; it must run only in "
                "modules.post_timestep, after every substep has sampled the histories",
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
  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_finalize_sfr_histories_init(void) {
  if (check_placement() != 0) {
    return -1;
  }
  VERBOSE_LOG("SAGE finalize SFR histories module initialized");
  return 0;
}

int sage26_finalize_sfr_histories_process(struct ModuleContext *ctx, struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }
  if (ctx == NULL || ctx->params == NULL || ctx->num_substeps < 1) {
    ERROR_LOG("%s needs a module context with a positive substep count", MODULE_NAME);
    return -1;
  }

  const double unit_mass_in_g = ctx->params->UnitMass_in_g;
  const double unit_time_in_s = ctx->params->UnitTime_in_s;

  for (int i = 0; i < ngal; i++) {
    // Workspace rows without a galaxy are not galaxies. Merged (Type 3) rows are
    // averaged too: SAGE26 records SubstepsUsed on them as well (:461-468)
    if (halos[i].galaxy == NULL) {
      continue;
    }
    struct GalaxyData *gal = halos[i].galaxy;
    gal->SubstepsUsed = ctx->num_substeps;
    mimic_sage_average_sfr_histories(gal, unit_mass_in_g, unit_time_in_s, NULL);
  }

  return 0;
}

int sage26_finalize_sfr_histories_cleanup(void) {
  VERBOSE_LOG("SAGE finalize SFR histories module cleaned up");
  return 0;
}
