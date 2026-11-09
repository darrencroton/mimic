/**
 * @file    sage26_finalize_sfr_histories.c
 * @brief   SAGE26's end-of-snapshot averaging of the per-substep star-formation histories
 *
 * Records the substep count (SubstepsUsed) on every galaxy row, merged ones included, and averages
 * the six per-substep star-formation histories (SfrDisk, SfrBulge and the cold gas and metals
 * sampled with each, ten bins each) into the output scalars SfrDisk, SfrBulge (Msun/yr), SfrDiskZ
 * and SfrBulgeZ (dimensionless), once per FoF in post_timestep. Implements the substep-count
 * record after SAGE26's last substep and its HDF5 writer's averaging
 * (mimic_sage_average_sfr_histories).
 *
 * init() requires exactly one post_timestep entry as process_full_halo and no other entry (in a
 * substep phase it would average histories still being sampled), and
 * sage26_initialise_merger_clock in pre_timestep: its reset at joining is the histories' only
 * reset, without which they would accumulate across snapshots.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"
#include "shared/sage_sfr_history.h"

static const char *MODULE_NAME = "sage26_finalize_sfr_histories";
static const char *CLOCK_NAME = "sage26_initialise_merger_clock";

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

  /* Dependency check: the histories are inherited (no init_repeat), and
   * sage26_initialise_merger_clock's reset at joining is their only reset (for a galaxy that
   * stays a Type 0 or 1, as SAGE26 resets them) */
  if (!module_configured_in_phase(CLOCK_NAME, MimicConfig.pre_timestep,
                                  MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO)) {
    ERROR_LOG("%s requires %s in pre_timestep as process_full_halo — it owns the SFR-history "
              "reset at joining; without it the histories accumulate across snapshots and "
              "SfrDisk, SfrBulge, SfrDiskZ and SfrBulgeZ are wrong on every row",
              MODULE_NAME, CLOCK_NAME);
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
    // averaged too: SAGE26 records SubstepsUsed on them as well
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
