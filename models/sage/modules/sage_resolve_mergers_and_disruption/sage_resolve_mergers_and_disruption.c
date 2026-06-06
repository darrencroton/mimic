/**
 * @file    sage_resolve_mergers_and_disruption.c
 * @brief   SAGE single-pass merger/disruption ordering parity handler
 *
 * Reproduces SAGE's immediate in-loop merger handling:
 *   - decrement MergTime inside one satellite pass
 *   - decide disruption vs merger from live substep state
 *   - resolve the current execution target with one-hop redirect
 *   - mutate target state immediately before advancing to the next satellite
 *   - emit per-merger events immediately for downstream consumers
 */

#include <math.h>

#include "shared/central_link.h"
#include "module_system/generated/event_contracts.h"
#include "sage_merger_ops.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

static double THRESHOLD_MAJOR_MERGER;
static double THRESHOLD_SAT_DISRUPTION;

static void (*action_hook)(const char *action, int source_index, int target_index,
                           double mass_ratio) = NULL;

void sage_resolve_mergers_and_disruption_set_action_hook(
    void (*hook)(const char *action, int source_index, int target_index, double mass_ratio)) {
  action_hook = hook;
}

static void record_action(const char *action, int source_index, int target_index,
                          double mass_ratio) {
  if (action_hook != NULL) {
    action_hook(action, source_index, target_index, mass_ratio);
  }
}

int sage_resolve_mergers_and_disruption_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ThresholdMajorMerger", THRESHOLD_MAJOR_MERGER, 0.0, 1.0,
                                    "major merger mass ratio threshold");
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("ThresholdSatDisruption", THRESHOLD_SAT_DISRUPTION, 0.0, 1000.0,
                                    "halo-to-baryonic mass ratio threshold");

  /* Dependency advisory: sage_initialise_merger_clock should be in pre_timestep */
  if (!module_configured_in_phase("sage_initialise_merger_clock", MimicConfig.pre_timestep,
                                  MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO)) {
    WARNING_LOG("sage_resolve_mergers_and_disruption: sage_initialise_merger_clock "
                "is not configured in pre_timestep — MergTime values from tree "
                "load may be stale; not always wrong, but suspicious");
  }

  INFO_LOG("SAGE merger/disruption resolver initialized");
  VERBOSE_LOG("  ThresholdMajorMerger = %.3f", THRESHOLD_MAJOR_MERGER);
  VERBOSE_LOG("  ThresholdSatDisruption = %.3f", THRESHOLD_SAT_DISRUPTION);
  return 0;
}

int sage_resolve_mergers_and_disruption_cleanup(void) {
  action_hook = NULL;
  return 0;
}

int sage_resolve_mergers_and_disruption_process(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal) {
  if (ctx == NULL || ctx->num_substeps <= 0) {
    ERROR_LOG("Invalid immediate merger context (ctx=%p, num_substeps=%d)", (void *)ctx,
              (ctx != NULL) ? ctx->num_substeps : -1);
    return -1;
  }

  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  const int fof_central_idx = mimic_find_fof_central_index(halos, ngal);
  if (fof_central_idx < 0 || halos[fof_central_idx].galaxy == NULL) {
    return 0;
  }

  for (int i = 0; i < ngal; i++) {
    double source_dt = 0.0;
    enum MimicObjectTimeStatus dt_status;

    if ((halos[i].Type != 1 && halos[i].Type != 2) || halos[i].galaxy == NULL) {
      continue;
    }

    struct GalaxyData *satellite = halos[i].galaxy;

    dt_status = mimic_object_substep_dt(&halos[i], ctx, &source_dt);
    if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
      continue;
    }
    if (dt_status != MIMIC_OBJECT_TIME_OK) {
      ERROR_LOG("Invalid immediate-merger dt for halo %lld (SnapNum=%d, dT=%.3e, num_substeps=%d, "
                "status=%s)",
                halos[i].HaloNr, halos[i].SnapNum, halos[i].dT, ctx->num_substeps,
                mimic_object_time_status_str(dt_status));
      return -1;
    }

    if (satellite->MergTime >= 999.0) {
      ERROR_LOG("Satellite %lld has unset MergTime (%.1f)", halos[i].HaloNr, satellite->MergTime);
      return -1;
    }

    satellite->MergTime -= source_dt;

    const double fraction = ((double)ctx->substep_number + 1.0) / (double)ctx->num_substeps;
    double current_mvir = halos[i].Mvir - halos[i].deltaMvir * (1.0 - fraction);
    if (current_mvir < 0.0) {
      current_mvir = 0.0;
    }

    const double galaxy_baryons = satellite->StellarMass + satellite->ColdGas;
    const double virial_to_baryons =
        (galaxy_baryons > 0.0) ? (current_mvir / galaxy_baryons) : -1.0;
    const int eligible = (galaxy_baryons == 0.0) ||
                         (galaxy_baryons > 0.0 && (virial_to_baryons <= THRESHOLD_SAT_DISRUPTION));

    if (!eligible) {
      continue;
    }

    if (!isfinite(satellite->MergTime)) {
      WARNING_LOG("Satellite %lld has non-finite MergTime", halos[i].HaloNr);
      continue;
    }

    const int target_idx = mimic_resolve_immediate_target_index(halos, ngal, i, fof_central_idx);
    if (target_idx < 0 || target_idx >= ngal || target_idx == i ||
        halos[target_idx].galaxy == NULL) {
      ERROR_LOG("Invalid immediate merger target (satellite=%d, target=%d)", i, target_idx);
      return -1;
    }
    struct GalaxyData *target = halos[target_idx].galaxy;

    if (satellite->MergTime > 0.0) {
      mimic_sage_disruption_transfer(target, satellite);
      halos[i].Type = 3;
      record_action("disrupt", i, target_idx, 0.0);
      continue;
    }

    double source_time = 0.0;
    enum MimicObjectTimeStatus time_status =
        mimic_object_substep_time(&halos[i], ctx, &source_time);
    if (time_status != MIMIC_OBJECT_TIME_OK) {
      ERROR_LOG("Invalid immediate-merger event time for halo %lld (status=%s)", halos[i].HaloNr,
                mimic_object_time_status_str(time_status));
      return -1;
    }

    const double mass_ratio = mimic_sage_calculate_merger_mass_ratio(satellite, target);

    mimic_sage_merge_transfer(target, satellite);

    if (mass_ratio > 0.0) {
      if (module_emit_event(ctx, SAGE_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER, i, target_idx,
                            mass_ratio, source_dt) != 0) {
        ERROR_LOG("Failed to emit immediate merger event (source=%d, target=%d, ratio=%.6f)", i,
                  target_idx, mass_ratio);
        return -1;
      }
    }

    if (mass_ratio > 0.1) {
      target->TimeOfLastMinorMerger = source_time;
    }

    if (mass_ratio > THRESHOLD_MAJOR_MERGER) {
      target->BulgeMass = target->StellarMass;
      target->MetalsBulgeMass = target->MetalsStellarMass;
      target->TimeOfLastMajorMerger = source_time;
    }

    halos[i].Type = 3;
    record_action("merge", i, target_idx, mass_ratio);
  }

  return 0;
}
