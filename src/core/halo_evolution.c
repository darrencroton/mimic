/**
 * @file    halo_evolution.c
 *
 * Driver-neutral FoF evolution adapters shared by the tree and snapshot
 * drivers. Both drivers assemble a FoF workspace their own way (tree traversal
 * over the FoFWorkspace global; snapshot sweep over a per-run buffer) and then
 * hand it to the functions here, which own the module-context setup, the
 * physics-execution dispatch, the FoF chain count, and the halo-init payload.
 *
 * These live in their own file so build_model.c stays tree-driver-specific and
 * so the unit-test harness can link the shared adapters without pulling in the
 * tree traversal it deliberately stubs (tests/unit/test_stubs.c).
 *
 * The cross-format identity gate rests on both drivers passing through these
 * same bodies; a driver-specific variant of any of them would reopen the
 * divergence surface this file exists to close.
 */

#include <inttypes.h>

#include "config.h"
#include "globals.h"
#include "inheritance.h"
#include "module_registry.h"
#include "proto.h"
#include "types.h"
#include "generated/tree_property_accessors.h"

/*
 * Build the driver-neutral descendant payload from the halo input view. The
 * field population is generated from property metadata (see the included
 * file), so it cannot silently desync from struct HaloInitPayload when halo
 * properties are added. This is the only place index coupling touches halo
 * init; the consumer (init_halo_from_payload) is format-neutral.
 *
 * Shared by the tree and snapshot drivers; this is the only instantiation of
 * the generated populator.
 */
struct HaloInitPayload make_halo_init_payload(struct HaloInputView view, int halonr) {
  struct HaloInitPayload payload;

#include "../include/generated/populate_halo_payload.inc"

  return payload;
}

/* Shared by the tree and snapshot drivers. */
int count_fof_subhalos(struct HaloInputView view, int first_fof_halo) {
  int count = 0;
  int fofhalo = first_fof_halo;

  int64_t steps = 0;

  while (fofhalo >= 0) {
    /* The chain stays inside this view, so it can visit each halo at most once;
     * more steps than that means the input's FoF links form a cycle, which
     * would otherwise loop forever. The guard counter is int64_t so it cannot
     * itself overflow when view.count reaches the format's INT32_MAX ceiling;
     * count only increments after the guard, so it stays within int range. */
    if (++steps > view.count) {
      FATAL_ERROR("FoF chain from halo %d visits more than the %" PRId64
                  " halos of its input view; the NextHaloInFOFgroup links contain a cycle",
                  first_fof_halo, view.count);
    }
    count++;
    fofhalo = mimic_tree_get_NextHaloInFOFgroup(view, fofhalo);
  }

  return count;
}

/**
 * @brief   Setup module context for current snapshot and FOF group
 *
 * @param   ctx          Module context to populate
 * @param   view         Input view over this unit's raw halos
 * @param   workspace    FoF workspace holding this group's galaxies
 * @param   halonr       Index of main halo in the input view
 * @param   centralgal   Index of central galaxy in the workspace
 *
 * Shared by the tree and snapshot drivers, which own different workspaces (the
 * FoFWorkspace global and the snapshot driver's per-run buffer respectively).
 */
static void setup_module_context(struct ModuleContext *ctx, struct HaloInputView view,
                                 struct Halo *workspace, int halonr, int centralgal) {
  int snap = mimic_tree_get_SnapNum(view, halonr);

  /* Snapshot information */
  ctx->redshift = MimicConfig.ZZ[snap];
  ctx->time = Age[snap];
  ctx->snapshot_number = snap;

  /* Halo information */
  ctx->central_index = centralgal;
  ctx->central_galaxy = &workspace[centralgal];
  ctx->active_event = NULL;

  /* Configuration access */
  ctx->params = &MimicConfig;

  /* Calculate total time interval for this timestep.
   *
   * INVARIANT: workspace halos still carry their *progenitor's* SnapNum at
   * this point — inheritance copies it unchanged, and it is only advanced to
   * the current snapshot when the workspace is marshalled to the output
   * buffer. That is what makes Age[SnapNum] here the progenitor age. */
  if (workspace[centralgal].SnapNum >= 0) {
    int prev_snap = workspace[centralgal].SnapNum;
    ctx->time_interval = Age[prev_snap] - Age[snap];
  } else {
    ctx->time_interval = 0.0; /* First snapshot has no previous */
  }

  if (MimicConfig.TimestepScheme == TIMESTEP_SCHEME_DYNAMIC) {
    double rvir = get_virial_radius(view, halonr);
    double vvir = get_virial_velocity(view, halonr);
    double t_dyn = (vvir > 0.0) ? (rvir / vvir) : 0.0;
    ctx->num_substeps = compute_dynamic_substeps(ctx->time_interval, t_dyn, MimicConfig.SubSteps,
                                                 MimicConfig.MaxDynamicSubsteps);
  } else {
    ctx->num_substeps = (MimicConfig.SubSteps > 0) ? MimicConfig.SubSteps : 1;
  }

  /* Initialize substep information (updated in substep loop) */
  ctx->substep_number = 0;
  ctx->substep_time = ctx->time;
  ctx->substep_dt = (ctx->num_substeps > 0) ? (ctx->time_interval / ctx->num_substeps) : 0.0;
}

/**
 * @brief   Evolve one FoF workspace through the physics-execution engine
 *
 * @param   view       Input view over this unit's raw halos
 * @param   workspace  FoF workspace holding this group's galaxies
 * @param   halonr     Index of the FOF-background subhalo (main halo)
 * @param   ngal       Total number of halos to process
 *
 * Driver adapter for physics execution: selects the FOF Type 0 central,
 * propagates the stable central unique ID, builds the module context, and hands
 * the workspace to the format-neutral physics-execution engine. Output
 * marshalling is a separate, driver-owned step performed by the caller through
 * the shared output-buffer marshaller.
 *
 * Shared by the tree and snapshot drivers, which pass their own workspaces.
 *
 * Phase assignments and loop modes are configured in the input YAML file.
 * TimestepScheme and SubSteps together determine the active substep count.
 */
void process_halo_evolution(struct HaloInputView view, struct Halo *workspace, int halonr,
                            int ngal) {
  int centralgal, i;
  struct ModuleContext ctx;

  /* Identify the FOF Type 0 central used for global module context. */
  centralgal = -1;
  for (i = 0; i < ngal; i++) {
    if (workspace[i].Type == 0) {
      centralgal = i;
      break;
    }
  }

  if (centralgal == -1) {
    FATAL_ERROR("No Type 0 central found for FOF halo %d (ngal=%d)", halonr, ngal);
  }

  if (workspace[centralgal].HaloNr != halonr) {
    FATAL_ERROR("Central galaxy HaloNr=%d does not match FOF halo %d", workspace[centralgal].HaloNr,
                halonr);
  }

  /* Set FOF-host central unique ID for all members (stable output contract). */
  for (i = 0; i < ngal; i++) {
    workspace[i].UniqueCentralGalaxyID = workspace[centralgal].UniqueGalaxyID;
  }

  /* Setup module execution context */
  setup_module_context(&ctx, view, workspace, halonr, centralgal);

  /* Run the configured module lifecycle over this FoF workspace */
  execute_module_pipeline(&ctx, workspace, ngal);
}
