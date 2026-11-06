/**
 * @file    sage26_resolve_mergers_and_disruption.c
 * @brief   SAGE26's per-substep merger and disruption pass and its two transactions
 *
 * The pass reproduces SAGE26's (R/src/core_build_model.c:519-587, B below; R = the staged
 * SAGE26 source) once per substep, after every galaxy's physics: for each row in order, a
 * Type 1 or 2 not yet merged (B:523)
 *
 *   1. must hold a clock below 999 (B:524-527, where SAGE26 aborts);
 *   2. has its clock decremented by its substep interval deltaT / N, a float store
 *      (B:529-530);
 *   3. is eligible if its float StellarMass + ColdGas is exactly zero, or positive with
 *      the interpolated Mvir - deltaMvir * (1 - (step + 1) / N), not floored, at most
 *      ThresholdSatDisruption times it (B:534-537);
 *   4. targets the FoF central (Type 1) or its subhalo's central (Type 2), redirected
 *      once through that target's central if it has merged (B:539-543,
 *      shared/central_link.h);
 *   5. with a finite clock (B:547), at the event time Age - (step + 0.5) * (deltaT / N)
 *      (B:549), is disrupted to the target's ICS while its clock is positive (B:574-575),
 *      else merges into the target (B:576-581).
 *
 * The target is mutated at once, before the next row. A satellite that is not eligible,
 * or whose clock is not finite, stays and is reconsidered next substep; there is no
 * early orphan purge and no forced clock, so an orphan's removal is the pass's outcome
 * (an orphan's interpolated Mvir reaches 0 at the last substep, B:323-325).
 *
 * The merger transaction reproduces SAGE26's deal_with_galaxy_merger with BulgeSizeOn 0 or 3
 * (R/src/model_mergers.c:228-333, M below), in its statement order:
 *
 *   1. the mass ratio from the float StellarMass + ColdGas sums (M:235-249);
 *   2. the target's pre-transfer morphology flag (M:253-254), carried with the event;
 *   3. add_galaxies_together (M:259, :456-543), whose satellite disk stars credited to the
 *      instability channel update the target's instability-bulge radius with BulgeSizeOn 3
 *      (M:516-522);
 *   4. the merger event, whose consumers run immediately in the phase's YAML order:
 *      sage26_quasar_mode's grow_black_hole (M:262-264), then sage26_starburst_feedback's
 *      burst into the channel the carried flag selects (M:266-287), with the satellite's
 *      substep dt in the event for the burst's history bin;
 *   5. with BulgeSizeOn 3 (R1 onward): get_bulge_radius on the target (M:293), then the
 *      remnant radius by energy conservation from the post-burst target and the still
 *      populated satellite (M:297, :135-212, shared/sage_merger_physics.h);
 *   6. for a ratio strictly above ThresholdMajorMerger, make_bulge_from_burst (M:302), the
 *      remnant radius into MergerBulgeRadius, get_bulge_radius and TimeOfLastMajorMerger
 *      (M:299-312); otherwise TimeOfLastMinorMerger and, onto a target that was
 *      disk-dominated before the addition, get_bulge_radius alone (the remnant radius is
 *      discarded), onto a spheroid-dominated one the remnant radius into MergerBulgeRadius
 *      and get_bulge_radius (M:313-331). DiskScaleRadius is never zeroed. With BulgeSizeOn 0
 *      (R0) every get_bulge_radius of steps 5 and 6 zeroes the three radii
 *      (R/src/model_misc.c:242-247), so the remnant radius never reaches the state and is
 *      not formed;
 *   7. the satellite leaves the pass as Type 3 (SAGE26's mergeType 1 or 2, M:311, :315).
 *
 * A ratio of 0 (both baryon sums zero) is not suppressed: the transaction, the event and
 * its consumers run as in SAGE26.
 *
 * The disruption reproduces SAGE26's disrupt_satellite_to_ICS (M:845-902) through
 * shared/sage_merger_physics.h; the satellite leaves as Type 3 (SAGE26's mergeType 4). Its gas
 * goes to the PASSED target's selected pair: its CGM pair with the required CGMrecipeOn (campaign
 * decision D33) 1 and the target's own Regime 0, its hot pair otherwise (M:851,
 * R/src/model_misc.h:76-85). For an orphan the target is its subhalo's central (step 4), which need
 * not be the FoF central (B:539-543, :575). The merger's addition moves CGM to CGM and hot to hot
 * whatever either regime (M:464-465, :496-497).
 */

#include <math.h>

#include "shared/central_link.h"
#include "shared/sage_bulge_size_physics.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_constants.h"
#include "module_system/generated/event_contracts.h"
#include "sage_merger_ops.h"
#include "shared/time_parity.h"
#include "module_system/parameter_helpers.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "module_registry.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static double THRESHOLD_MAJOR_MERGER;
static double THRESHOLD_SAT_DISRUPTION;

/* SAGE26's BulgeSizeOn: 0 (R0) or 3 (Tonini sizes), the mode of add_galaxies_together's
 * satellite disk-star radius update (R/src/model_misc.c:395) and of the transaction's bulge
 * radii after the burst (M:293-330) */
static int BULGE_SIZE_ON;

/* SAGE26's CGMrecipeOn (0 hot-gas routing, 1 regime-aware CGM routing), required and checked at
 * init (campaign decision D33), which with the disruption target's Regime selects its pair
 * (R/src/model_misc.h:78) */
static int CGM_RECIPE_ON;

// ============================================================================
// TEST HOOKS
// ============================================================================

static void (*action_hook)(const char *action, int source_index, int target_index,
                           double mass_ratio) = NULL;

/**
 * @brief Install a unit-test observer for immediate merger/disruption actions.
 *
 * Production code leaves this NULL. Cleanup clears the hook so tests cannot leak
 * observer state across module lifecycles.
 */
void sage26_resolve_mergers_and_disruption_set_action_hook(
    void (*hook)(const char *action, int source_index, int target_index, double mass_ratio)) {
  action_hook = hook;
}

static void record_action(const char *action, int source_index, int target_index,
                          double mass_ratio) {
  if (action_hook != NULL) {
    action_hook(action, source_index, target_index, mass_ratio);
  }
}

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

static int validate_immediate_merger_context(struct ModuleContext *ctx) {
  if (ctx == NULL || ctx->num_substeps <= 0) {
    ERROR_LOG("Invalid immediate merger context (ctx=%p, num_substeps=%d)", (void *)ctx,
              (ctx != NULL) ? ctx->num_substeps : -1);
    return -1;
  }
  return 0;
}

static int validate_immediate_target(struct Halo *halos, int ngal, int source_idx, int target_idx) {
  if (target_idx < 0 || target_idx >= ngal || target_idx == source_idx ||
      halos[target_idx].galaxy == NULL) {
    ERROR_LOG("Invalid immediate merger target (satellite=%d, target=%d)", source_idx, target_idx);
    return -1;
  }
  return 0;
}

/**
 * @brief The production event sink: emit the merger event to the phase's consumers
 *
 * module_emit_event() dispatches it immediately to every subscribed process_per_event
 * consumer of the phase, in YAML order, before it returns.
 */
static int emit_merger_event(struct ModuleContext *ctx, struct Halo *halos, int source_idx,
                             int target_idx, double mass_ratio, double source_dt) {
  (void)halos;
  if (module_emit_event(ctx, SAGE26_RESOLVE_MERGERS_AND_DISRUPTION_EVENT_MERGER, source_idx,
                        target_idx, mass_ratio, source_dt) != 0) {
    ERROR_LOG("Failed to emit immediate merger event (source=%d, target=%d, ratio=%.6f)",
              source_idx, target_idx, mass_ratio);
    return -1;
  }
  return 0;
}

int sage26_resolve_mergers_and_disruption_disrupt(struct Halo *halos, int ngal, int satellite_idx,
                                                  int target_idx, double event_time,
                                                  struct MimicSageDisruptLocals *locals) {
  if (halos == NULL || satellite_idx < 0 || satellite_idx >= ngal ||
      halos[satellite_idx].galaxy == NULL) {
    ERROR_LOG("Invalid disruption (satellite=%d, target=%d, ngal=%d)", satellite_idx, target_idx,
              ngal);
    return -1;
  }
  // validate_immediate_target() reports an invalid target itself
  if (validate_immediate_target(halos, ngal, satellite_idx, target_idx) != 0) {
    return -1;
  }

  /* SAGE parity: the passed target's own regime routes the gas (M:851), the FoF central's only
   * when it is the target */
  mimic_sage_disrupt_satellite_to_ics(halos[target_idx].galaxy, halos[satellite_idx].galaxy,
                                      event_time, CGM_RECIPE_ON, locals);

  // SAGE parity: the satellite is marked disrupted (mergeType 4, M:902); here Type 3
  halos[satellite_idx].Type = 3;
  record_action("disrupt", satellite_idx, target_idx, 0.0);
  return 0;
}

int sage26_resolve_mergers_and_disruption_merge(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal, int satellite_idx, int target_idx,
                                                double event_time, double source_dt,
                                                MimicSageMergerEmitFn emit,
                                                struct MimicSageMergerLocals *merger_locals,
                                                struct MimicSageAddLocals *add_locals) {
  mimic_sage_merger_locals_reset(merger_locals);
  mimic_sage_add_locals_reset(add_locals);
  if (ctx == NULL || halos == NULL || emit == NULL || satellite_idx < 0 || satellite_idx >= ngal ||
      halos[satellite_idx].galaxy == NULL ||
      validate_immediate_target(halos, ngal, satellite_idx, target_idx) != 0) {
    ERROR_LOG("Invalid merger transaction (satellite=%d, target=%d, ngal=%d)", satellite_idx,
              target_idx, ngal);
    return -1;
  }
  /* SAGE26's run_params->BulgeSizeOn and Hubble_h (R/src/model_misc.c:242, :249, :395, :398);
   * mode 3 reads Hubble_h, so it refuses a context without the core's parameters, as
   * sage26_set_disk_scale_radius does */
  if (BULGE_SIZE_ON == MIMIC_SAGE_BULGE_SIZE_TONINI && ctx->params == NULL) {
    ERROR_LOG("sage26_resolve_mergers_and_disruption: BulgeSizeOn 3 needs the core parameters "
              "(Hubble_h)");
    return -1;
  }
  const double hubble_h = ctx->params != NULL ? ctx->params->Hubble_h : 0.0;
  const struct MimicSageBulgeSize sizes = {BULGE_SIZE_ON, hubble_h};

  struct GalaxyData *satellite = halos[satellite_idx].galaxy;
  struct GalaxyData *target = halos[target_idx].galaxy;

  // SAGE parity: the ratio and the morphology flag before the addition (M:235-254)
  const double mass_ratio = mimic_sage_merger_mass_ratio(satellite, target, merger_locals);
  const int is_disk_dominated = mimic_sage_merger_target_disk_dominated(target, merger_locals);
  /* SAGE26 saves the target's disk radius before the addition for the burst (M:257); the burst
   * consumer reads the target's DiskScaleRadius at the event, which equals it: nothing here or
   * in the black-hole consumer writes DiskScaleRadius (sage26_set_disk_scale_radius, at
   * pre-timestep, is its only writer) */
  if (merger_locals != NULL) {
    merger_locals->old_disk_radius = target->DiskScaleRadius;
  }

  // SAGE parity: add_galaxies_together before the black hole and the burst (M:259)
  mimic_sage_add_galaxies_together(target, satellite, &sizes, add_locals);

  /* The flag travels with the event on the target, the halo its consumers receive: the
   * burst selects its channel from it (M:266-282), never from a re-read after the
   * transfer */
  target->MergerDiskDominated = is_disk_dominated;

  /* SAGE parity: grow_black_hole, then the burst, for every merger, ratio 0 included
   * (M:262-287); the consumers run inside the emission */
  if (emit(ctx, halos, satellite_idx, target_idx, mass_ratio, source_dt) != 0) {
    return -1;
  }

  // SAGE parity: strictly above the threshold is major, everything else minor (M:299-331)
  const int major_merger = mass_ratio > THRESHOLD_MAJOR_MERGER;
  if (BULGE_SIZE_ON == MIMIC_SAGE_BULGE_SIZE_TONINI) {
    /* SAGE parity: the target's bulge radius is synchronised after the addition and the burst
     * (M:293), then the remnant radius is formed from that post-burst target and the
     * satellite, which still holds its reservoirs (M:297) */
    (void)mimic_sage_get_bulge_radius(target, &sizes, NULL);
    const double new_merger_radius = mimic_sage_merger_remnant_radius(
        target, satellite, merger_locals != NULL ? &merger_locals->remnant : NULL);
    if (major_merger) {
      /* SAGE parity: the whole stellar mass becomes a merger bulge with the remnant radius,
       * then the radii are synchronised (M:302-308); DiskScaleRadius is not zeroed */
      mimic_sage_make_bulge_from_burst(target);
      target->MergerBulgeRadius = (float)new_merger_radius;
      (void)mimic_sage_get_bulge_radius(target, &sizes, NULL);
      target->TimeOfLastMajorMerger = event_time;
    } else {
      target->TimeOfLastMinorMerger = event_time;
      if (is_disk_dominated) {
        /* SAGE parity: a minor merger onto a target that was disk-dominated before the
         * addition discards the remnant radius it formed and only synchronises (M:318-325) */
        (void)mimic_sage_get_bulge_radius(target, &sizes, NULL);
      } else {
        // SAGE parity: onto a spheroid-dominated target the remnant radius is kept (M:326-330)
        target->MergerBulgeRadius = (float)new_merger_radius;
        (void)mimic_sage_get_bulge_radius(target, &sizes, NULL);
      }
    }
  } else {
    /* SAGE parity: with BulgeSizeOn 0 every get_bulge_radius of the transaction zeroes the
     * radii (M:293, :308, :325, :330; R/src/model_misc.c:242-247), so the remnant radius SAGE26
     * forms between them (M:297) never reaches the state and is not formed here */
    mimic_sage_reset_bulge_radii(target);
    if (major_merger) {
      mimic_sage_make_bulge_from_burst(target);
      mimic_sage_reset_bulge_radii(target);
      target->TimeOfLastMajorMerger = event_time;
    } else {
      target->TimeOfLastMinorMerger = event_time;
      mimic_sage_reset_bulge_radii(target);
    }
  }
  if (merger_locals != NULL) {
    merger_locals->major_merger = major_merger;
  }

  halos[satellite_idx].Type = 3;
  record_action("merge", satellite_idx, target_idx, mass_ratio);
  return 0;
}

static void pass_record_reset(struct MimicSagePassRecord *record) {
  if (record == NULL) {
    return;
  }
  record->considered = 0;
  record->action = 0;
  record->deltaT = NAN;
  record->currentMvir = NAN;
  record->galaxyBaryons = NAN;
  record->gate_zero_baryons = -1;
  record->gate_passed = -1;
  record->merger_centralgal = -1;
  record->redirected = -1;
  record->clock_finite = -1;
  record->event_time = NAN;
  record->step_bin = -1;
  record->dt = NAN;
  record->major_merger = -1;
}

int sage26_resolve_mergers_and_disruption_pass(struct ModuleContext *ctx, struct Halo *halos,
                                               int ngal, MimicSageMergerEmitFn emit,
                                               struct MimicSagePassRecord *records) {
  if (validate_immediate_merger_context(ctx) != 0 || emit == NULL) {
    return -1;
  }
  if (halos == NULL || ngal <= 0) {
    return 0;
  }
  for (int i = 0; i < ngal; i++) {
    pass_record_reset(records != NULL ? &records[i] : NULL);
  }

  /* SAGE26's centralgal, the FoF's Type 0 (B:393-400); only a Type 1's target reads it, so
   * a workspace without one fails at the first eligible Type 1 */
  const int fof_central_idx = mimic_find_fof_central_index(halos, ngal);
  const int step = ctx->substep_number;
  const int num_substeps = ctx->num_substeps;

  for (int i = 0; i < ngal; i++) {
    struct MimicSagePassRecord *record = (records != NULL) ? &records[i] : NULL;

    // SAGE parity: a Type 1 or 2 not yet merged (B:523); a merged row is the package's Type 3
    if ((halos[i].Type != 1 && halos[i].Type != 2) || halos[i].galaxy == NULL) {
      continue;
    }
    struct GalaxyData *satellite = halos[i].galaxy;

    /* The row's substep interval, Age[SnapNum] - halo_age over the substep count (B:529):
     * the core's dT is that difference of its Age table. A row the core marks as the first
     * snapshot's boundary (SnapNum < 0, dT <= 0) is not evolved; a satellite never is one */
    double dt = 0.0;
    const enum MimicObjectTimeStatus dt_status = mimic_object_substep_dt(&halos[i], ctx, &dt);
    if (dt_status == MIMIC_OBJECT_TIME_SKIP_INITIAL) {
      continue;
    }
    if (dt_status != MIMIC_OBJECT_TIME_OK) {
      ERROR_LOG("Invalid merger-pass dt for halo %lld (SnapNum=%d, dT=%.3e, num_substeps=%d, "
                "status=%s)",
                halos[i].HaloNr, halos[i].SnapNum, halos[i].dT, num_substeps,
                mimic_object_time_status_str(dt_status));
      return -1;
    }
    const double deltaT = halos[i].dT;
    if (record != NULL) {
      record->considered = 1;
      record->deltaT = deltaT;
    }

    // SAGE parity: SAGE26 aborts unless the clock is below 999 (B:524-527); a NaN fails too
    if (!(satellite->MergTime < SAGE_MERGTIME_UNSET_THRESHOLD)) {
      ERROR_LOG("Satellite %lld has MergTime %g, not below %.1f (SAGE26 aborts here, "
                "core_build_model.c:524-527)",
                halos[i].HaloNr, (double)satellite->MergTime, SAGE_MERGTIME_UNSET_THRESHOLD);
      return -1;
    }

    // SAGE parity: the float clock less the double substep interval (B:530)
    satellite->MergTime -= deltaT / num_substeps;

    /* SAGE parity: the interpolated Mvir with no floor at zero (B:534), one fused
     * multiply-subtract in SAGE26's objects (oracle README c06 table), written as fma() */
    const double current_mvir =
        fma(-halos[i].deltaMvir, 1.0 - ((double)step + 1.0) / (double)num_substeps, halos[i].Mvir);
    // SAGE parity: the float sum of two float members, widened on assignment (B:535)
    const double galaxy_baryons = satellite->StellarMass + satellite->ColdGas;
    const int gate_zero_baryons = (galaxy_baryons == 0.0);
    // SAGE parity: zero baryons, or positive baryons with the ratio at the threshold or below
    // (B:537)
    const int gate_passed =
        gate_zero_baryons ||
        (galaxy_baryons > 0.0 && (current_mvir / galaxy_baryons <= THRESHOLD_SAT_DISRUPTION));
    if (record != NULL) {
      record->currentMvir = current_mvir;
      record->galaxyBaryons = galaxy_baryons;
      record->gate_zero_baryons = gate_zero_baryons;
      record->gate_passed = gate_passed;
    }
    if (!gate_passed) {
      continue;
    }

    // SAGE parity: the target and its one-hop redirect (B:539-543)
    int redirected = 0;
    const int target_idx =
        mimic_resolve_immediate_target_index(halos, ngal, i, fof_central_idx, &redirected);
    if (record != NULL) {
      record->merger_centralgal = target_idx;
      record->redirected = redirected;
    }
    if (validate_immediate_target(halos, ngal, i, target_idx) != 0) {
      return -1;
    }

    // SAGE parity: a non-finite clock leaves the satellite as it is (B:547)
    const int clock_finite = isfinite(satellite->MergTime);
    if (record != NULL) {
      record->clock_finite = clock_finite;
    }
    if (!clock_finite) {
      DEBUG_LOG("Satellite %lld has a non-finite MergTime; not merged or disrupted",
                halos[i].HaloNr);
      continue;
    }

    /* SAGE parity: the event time Age[SnapNum] - (step + 0.5) * (deltaT / N) (B:549), one
     * fused multiply-subtract in SAGE26's objects, written as fma(); Age[SnapNum] is the
     * core's Age[current] (ctx->time) plus the row's dT, Age[SnapNum] - Age[current] */
    const double age = ctx->time + deltaT;
    const double event_time = fma(-((double)step + 0.5), deltaT / num_substeps, age);
    if (record != NULL) {
      record->event_time = event_time;
    }

    // SAGE parity: a positive clock disrupts the satellite to the ICS (B:574-575)
    if (satellite->MergTime > 0.0) {
      if (sage26_resolve_mergers_and_disruption_disrupt(halos, ngal, i, target_idx, event_time,
                                                        NULL) != 0) {
        return -1;
      }
      if (record != NULL) {
        record->action = 4;
      }
      continue;
    }

    // SAGE parity: otherwise a merger, in the substep's history bin (B:576-581)
    if (record != NULL) {
      record->step_bin = mimic_sage_sfr_step_bin(step, num_substeps);
      record->dt = dt;
    }
    struct MimicSageMergerLocals merger_locals;
    if (sage26_resolve_mergers_and_disruption_merge(ctx, halos, ngal, i, target_idx, event_time, dt,
                                                    emit, &merger_locals, NULL) != 0) {
      return -1;
    }
    if (record != NULL) {
      record->action = 1;
      record->major_merger = merger_locals.major_merger;
    }
  }

  return 0;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_resolve_mergers_and_disruption_init(void) {
  LOAD_AND_VALIDATE_RANGE_INCLUSIVE("ThresholdMajorMerger", THRESHOLD_MAJOR_MERGER, 0.0, 1.0,
                                    "major merger mass ratio threshold");
  LOAD_AND_VALIDATE_RANGE_EXCLUSIVE("ThresholdSatDisruption", THRESHOLD_SAT_DISRUPTION, 0.0, 1000.0,
                                    "halo-to-baryonic mass ratio threshold");
  LOAD_PARAM_INT("BulgeSizeOn", BULGE_SIZE_ON);
  if (mimic_sage_validate_bulge_size_on("sage26_resolve_mergers_and_disruption", BULGE_SIZE_ON) !=
      0) {
    return -1;
  }
  /* Required, no default (campaign decision D33): a missing key fails here */
  LOAD_PARAM_INT("CGMrecipeOn", CGM_RECIPE_ON);
  if (mimic_sage_validate_cgm_recipe_on("sage26_resolve_mergers_and_disruption", CGM_RECIPE_ON) !=
      0) {
    return -1;
  }

  /* sage26_initialise_merger_clock must be in pre_timestep so all satellites have a valid
   * MergTime before any substep runs. When both modules are in pre_timestep together,
   * the clock must also precede the resolver within that phase; a substep-placed resolver
   * is always safe because pre_timestep runs before all substep phases. */
  if (!module_configured_in_phase("sage26_initialise_merger_clock", MimicConfig.pre_timestep,
                                  MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO)) {
    ERROR_LOG("sage26_resolve_mergers_and_disruption requires sage26_initialise_merger_clock "
              "in pre_timestep as process_full_halo — satellites carry the sentinel "
              "MergTime (%.1f) from the tree load without it",
              SAGE_MERGTIME_UNSET_THRESHOLD);
    return -1;
  }
  if (module_configured_in_phase("sage26_resolve_mergers_and_disruption", MimicConfig.pre_timestep,
                                 MimicConfig.num_pre_timestep, PROCESSING_MODE_FULL_HALO) &&
      !module_precedes_in_phase("sage26_initialise_merger_clock",
                                "sage26_resolve_mergers_and_disruption", MimicConfig.pre_timestep,
                                MimicConfig.num_pre_timestep)) {
    ERROR_LOG("sage26_resolve_mergers_and_disruption requires sage26_initialise_merger_clock "
              "to precede it in pre_timestep — clock comes after the resolver");
    return -1;
  }

  /* SAGE parity: the merger transaction grows the black hole before the burst
   * (R/src/model_mergers.c:262-287); the event reaches its consumers in YAML order */
  if (modules_in_same_substep_phase("sage26_quasar_mode", PROCESSING_MODE_PER_EVENT,
                                    "sage26_starburst_feedback", PROCESSING_MODE_PER_EVENT) &&
      !module_precedes_in_substep_phase("sage26_quasar_mode", PROCESSING_MODE_PER_EVENT,
                                        "sage26_starburst_feedback", PROCESSING_MODE_PER_EVENT)) {
    ERROR_LOG("sage26_resolve_mergers_and_disruption requires sage26_quasar_mode to precede "
              "sage26_starburst_feedback as merger event consumers — SAGE26 grows the black "
              "hole before the merger burst");
    return -1;
  }

  VERBOSE_LOG("SAGE merger/disruption resolver initialized");
  VERBOSE_LOG("  ThresholdMajorMerger = %.3f", THRESHOLD_MAJOR_MERGER);
  VERBOSE_LOG("  ThresholdSatDisruption = %.3f", THRESHOLD_SAT_DISRUPTION);
  VERBOSE_LOG("  BulgeSizeOn = %d", BULGE_SIZE_ON);
  VERBOSE_LOG("  CGMrecipeOn = %d", CGM_RECIPE_ON);
  return 0;
}

int sage26_resolve_mergers_and_disruption_process(struct ModuleContext *ctx, struct Halo *halos,
                                                  int ngal) {
  return sage26_resolve_mergers_and_disruption_pass(ctx, halos, ngal, emit_merger_event, NULL);
}

int sage26_resolve_mergers_and_disruption_cleanup(void) {
  action_hook = NULL;
  BULGE_SIZE_ON = MIMIC_SAGE_BULGE_SIZE_OFF;
  CGM_RECIPE_ON = MIMIC_SAGE_CGM_RECIPE_OFF;
  VERBOSE_LOG("SAGE merger/disruption resolver cleaned up");
  return 0;
}
