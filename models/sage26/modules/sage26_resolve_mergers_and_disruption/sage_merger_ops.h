/**
 * @file    sage_merger_ops.h
 * @brief   The resolver's exported entries: the per-substep pass and its two transactions
 *
 * Declares sage26_resolve_mergers_and_disruption_pass(), SAGE26's per-substep merger and
 * disruption pass (R/src/core_build_model.c:519-587), and the two transactions it calls:
 * sage26_resolve_mergers_and_disruption_merge(), SAGE26's deal_with_galaxy_merger composed
 * from the shared pieces in shared/sage_merger_physics.h and the merger event, and
 * sage26_resolve_mergers_and_disruption_disrupt(), SAGE26's disrupt_satellite_to_ICS
 * (shared/sage_merger_physics.h). All three are exported so the oracle test can run them
 * at SAGE26's call boundaries.
 */

#ifndef MIMIC_SHARED_SAGE_MERGER_OPS_H
#define MIMIC_SHARED_SAGE_MERGER_OPS_H

#include "module_interface.h"
#include "types.h"
#include "shared/sage_merger_physics.h"

/**
 * @brief Where the merger transaction sends its merger event
 *
 * Production passes the module's sink, which calls module_emit_event() and so runs the
 * phase's process_per_event consumers before it returns. A unit test that has no phase
 * dispatch passes its own, which runs the consumers in the same order. Returns 0 on
 * success.
 */
typedef int (*MimicSageMergerEmitFn)(struct ModuleContext *ctx, struct Halo *halos, int source_idx,
                                     int target_idx, double mass_ratio, double source_dt);

/**
 * @brief SAGE26's deal_with_galaxy_merger on the R0 path (R/src/model_mergers.c:228-333)
 *
 * Defined in sage26_resolve_mergers_and_disruption.c, where the pass calls it for each
 * merging satellite; exported so its oracle test can run one transaction at SAGE26's
 * call boundary.
 *
 * @param ctx            Module context of the substep
 * @param halos          The FoF workspace
 * @param ngal           Rows in halos
 * @param satellite_idx  The merging satellite (SAGE26's p)
 * @param target_idx     The merger target (merger_centralgal)
 * @param event_time     The event time, a lookback time in code units (SAGE26's time)
 * @param source_dt      The satellite's substep interval (SAGE26's dt), carried in the event
 * @param emit           The event sink (the module's own in production)
 * @param merger_locals  Optional: the ratio, morphology and split values
 * @param add_locals     Optional: add_galaxies_together's values
 * @return 0 on success, -1 on an invalid call or a failed emission
 */
int sage26_resolve_mergers_and_disruption_merge(struct ModuleContext *ctx, struct Halo *halos,
                                                int ngal, int satellite_idx, int target_idx,
                                                double event_time, double source_dt,
                                                MimicSageMergerEmitFn emit,
                                                struct MimicSageMergerLocals *merger_locals,
                                                struct MimicSageAddLocals *add_locals);

/**
 * @brief SAGE26's disrupt_satellite_to_ICS and the satellite's removal
 * (R/src/model_mergers.c:845-902)
 *
 * Runs mimic_sage_disrupt_satellite_to_ics() and marks the satellite's row Type 3, the
 * package's merged row, standing for SAGE26's mergeType 4; the action hook reports it as
 * "disrupt", the package's disruption marker.
 *
 * @param halos          The FoF workspace
 * @param ngal           Rows in halos
 * @param satellite_idx  The disrupted satellite (SAGE26's gal)
 * @param target_idx     The target (SAGE26's centralgal, the pass's merger_centralgal)
 * @param event_time     The event time, a lookback time in code units (B:549)
 * @param locals         Optional: the gas sums and flags
 * @return 0 on success, -1 on an invalid call
 */
int sage26_resolve_mergers_and_disruption_disrupt(struct Halo *halos, int ngal, int satellite_idx,
                                                  int target_idx, double event_time,
                                                  struct MimicSageDisruptLocals *locals);

/**
 * @brief What the pass did with one galaxy row, SAGE26's driver locals (for tests)
 *
 * A double is NAN and an int -1 where the pass does not form it (SAGE26's absent value),
 * except considered and action, which are always set.
 */
struct MimicSagePassRecord {
  int considered;        /**< a Type 1 or 2 not yet merged (B:523) */
  int action;            /**< 0 none, 1 merger, 4 disruption */
  double deltaT;         /**< the row's snapshot interval (B:529) */
  double currentMvir;    /**< the interpolated virial mass (B:534) */
  double galaxyBaryons;  /**< float StellarMass + ColdGas, widened (B:535) */
  int gate_zero_baryons; /**< galaxyBaryons == 0.0 (B:537) */
  int gate_passed;       /**< the B:537 gate */
  int merger_centralgal; /**< the target row (B:539-543) */
  int redirected;        /**< the one-hop redirect was taken (B:541-543) */
  int clock_finite;      /**< isfinite(MergTime) (B:547) */
  double event_time;     /**< B:549 */
  int step_bin;          /**< the merger's history bin (B:579-580) */
  double dt;             /**< the merger's substep interval (B:581) */
  int major_merger;      /**< the merger's split, mass_ratio > ThreshMajorMerger (M:299) */
};

/**
 * @brief SAGE26's per-substep merger and disruption pass (R/src/core_build_model.c:519-587)
 *
 * Defined in sage26_resolve_mergers_and_disruption.c, whose process() runs it with the
 * module's event sink; exported so its oracle test can run it at the substep boundary.
 *
 * @param ctx      Module context of the substep (substep_number, num_substeps, time)
 * @param halos    The FoF workspace
 * @param ngal     Rows in halos
 * @param emit     The merger-event sink of every merger transaction
 * @param records  Optional: ngal records, one per row
 * @return 0 on success, -1 on an invalid state SAGE26 aborts on or the package rejects
 */
int sage26_resolve_mergers_and_disruption_pass(struct ModuleContext *ctx, struct Halo *halos,
                                               int ngal, MimicSageMergerEmitFn emit,
                                               struct MimicSagePassRecord *records);

#endif /* MIMIC_SHARED_SAGE_MERGER_OPS_H */
