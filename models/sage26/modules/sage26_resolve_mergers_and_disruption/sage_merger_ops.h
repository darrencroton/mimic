/**
 * @file    sage_merger_ops.h
 * @brief   The resolver's merger transaction entry and its disruption transfer
 *
 * Declares sage26_resolve_mergers_and_disruption_merge(), SAGE26's deal_with_galaxy_merger
 * composed from the shared pieces in shared/sage_merger_physics.h and the merger event,
 * and holds the package's disruption transfer, unchanged until SAGE26's
 * disrupt_satellite_to_ICS is ported.
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
 * @brief Apply SAGE disruption transfer: heat gas, preserve ICS, and move stars to ICS.
 */
static inline void mimic_sage_disruption_transfer(struct GalaxyData *target,
                                                  const struct GalaxyData *satellite) {
  target->HotGas += satellite->ColdGas + satellite->HotGas;
  target->MetalsHotGas += satellite->MetalsColdGas + satellite->MetalsHotGas;

  target->EjectedGas += satellite->EjectedGas;
  target->MetalsEjectedGas += satellite->MetalsEjectedGas;

  target->ICS += satellite->ICS;
  target->MetalsICS += satellite->MetalsICS;

  target->ICS += satellite->StellarMass;
  target->MetalsICS += satellite->MetalsStellarMass;
}

#endif /* MIMIC_SHARED_SAGE_MERGER_OPS_H */
