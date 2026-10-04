/**
 * @file    halo_evolution.c
 *
 * Driver-neutral FoF evolution adapters shared by the vertical and horizontal
 * drivers. Both drivers assemble their struct FoFWorkspace their own way (tree
 * traversal per unit; snapshot sweep over a per-run descriptor) and then hand
 * it to the functions here, which own the module-context setup, the
 * physics-execution dispatch, the FoF chain count, the halo-init payload, the
 * per-slice CentralMvir stamp and output segment, and the created-record
 * identity-space verdict.
 *
 * These live in their own file so build_model.c stays vertical-driver-specific and
 * so the unit-test harness can link the shared adapters without pulling in the
 * tree traversal it deliberately stubs (tests/unit/test_stubs.c).
 *
 * The cross-format identity gate rests on both drivers passing through these
 * same bodies; a driver-specific variant of any of them would reopen the
 * divergence surface this file exists to close.
 */

#include <inttypes.h>

#include "config.h"
#include "fof_workspace.h"
#include "galaxy_id.h"
#include "globals.h"
#include "inheritance.h"
#include "module_registry.h"
#include "numeric.h"
#include "output_buffer.h"
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
 * Shared by the vertical and horizontal drivers; this is the only instantiation of
 * the generated populator.
 */
struct HaloInitPayload make_halo_init_payload(struct HaloInputView view, int64_t halonr) {
  struct HaloInitPayload payload;

#include "../include/generated/populate_halo_payload.inc"

  return payload;
}

/* Shared by the vertical and horizontal drivers. */
int64_t count_fof_subhalos(struct HaloInputView view, int64_t first_fof_halo) {
  int64_t count = 0;
  int64_t fofhalo = first_fof_halo;

  int64_t steps = 0;

  while (fofhalo >= 0) {
    /* The chain stays inside this view, so it can visit each halo at most once;
     * more steps than that means the input's FoF links form a cycle, which
     * would otherwise loop forever. count only increments after the guard, so
     * it never exceeds view.count. */
    if (++steps > view.count) {
      FATAL_ERROR("FoF chain from halo %" PRId64 " visits more than the %" PRId64
                  " halos of its input view; the NextHaloInFOFgroup links contain a cycle",
                  first_fof_halo, view.count);
    }
    count++;
    fofhalo = mimic_tree_get_NextHaloInFOFgroup(view, fofhalo);
  }

  return count;
}

/*
 * Shared by the vertical and horizontal drivers, after each subhalo slice is
 * joined into the workspace.
 *
 * Stamps the FoF-central catalog virial mass onto every member of the slice
 * before physics runs, so CentralMvir is physically correct whenever a module
 * could observe it on the workspace - not only at output time. CentralMvir is a
 * structural per-FoF-group constant (the input-catalog Mvir of the FOF central);
 * physics never writes it, so the value reaches output unchanged and the shared
 * marshaller does not need to know about this field. The segment then records
 * the slice for the marshaller, whose merge relies on slices being contiguous
 * and recorded in workspace order.
 */
void record_subhalo_slice(struct HaloInputView view, struct FoFWorkspace *ws,
                          int64_t workspace_start, int64_t source_halo,
                          struct OutputBufferSegment *segment) {
  const double central_mvir =
      get_virial_mass(view, mimic_tree_get_FirstHaloInFOFgroup(view, source_halo));
  for (int64_t p = workspace_start; p < ws->count; p++) {
    ws->halos[p].CentralMvir = central_mvir;
  }

  segment->source_id = source_halo;
  segment->snapshot_number = mimic_tree_get_SnapNum(view, source_halo);
  segment->workspace_start = workspace_start;
  segment->workspace_count = ws->count - workspace_start;
  segment->output_first = -1;
  segment->output_count = 0;
}

/*
 * Shared by the vertical and horizontal drivers at startup. The INFO line's
 * format is relied on by tests/integration/test_record_creation.py, so both
 * drivers log it through here.
 */
struct RecordIdentitySpace record_identity_space_evaluate(const char *driver,
                                                          const char *reader_name, int64_t units,
                                                          int64_t rows_per_unit,
                                                          const char *rows_per_unit_source) {
  const bool fits = mimic_created_record_space_fits(units, rows_per_unit);

  INFO_LOG("Created-record identity space (%s, reader '%s'): units=%" PRId64
           ", rows_per_unit=%" PRId64 " (%s), radix=%d: %s",
           driver, reader_name, units, rows_per_unit, rows_per_unit_source,
           MAX_CREATED_RECORDS_PER_HOST,
           fits ? "fits int64" : "does not fit int64; this run cannot create records");

  return (struct RecordIdentitySpace){
      .unit = -1,
      .rows_per_unit = rows_per_unit,
      .fits = fits,
      .units = units,
      .driver = driver,
  };
}

/**
 * @brief   Setup module context for current snapshot and FOF group
 *
 * @param   ctx          Module context to populate
 * @param   view         Input view over this unit's raw halos
 * @param   workspace    FoF workspace rows holding this group's galaxies
 * @param   halonr       Index of main halo in the input view
 * @param   centralgal   Index of central galaxy in the workspace
 *
 * Shared by the vertical and horizontal drivers, which own different workspace
 * descriptors (sized per unit and per run respectively).
 */
static void setup_module_context(struct ModuleContext *ctx, struct HaloInputView view,
                                 struct Halo *workspace, int64_t halonr, int centralgal) {
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
 * @param   ws         FoF workspace descriptor; ws->count rows hold this group
 * @param   halonr     Index of the FOF-background subhalo (main halo)
 *
 * Driver adapter for physics execution: selects the FOF Type 0 central,
 * propagates the stable central unique ID, builds the module context, and hands
 * the workspace to the format-neutral physics-execution engine. Output
 * marshalling is a separate, driver-owned step performed by the caller through
 * the shared output-buffer marshaller.
 *
 * Shared by the vertical and horizontal drivers, which pass their own descriptors.
 *
 * Phase assignments and loop modes are configured in the input YAML file.
 * TimestepScheme and SubSteps together determine the active substep count.
 */
void process_halo_evolution(struct HaloInputView view, struct FoFWorkspace *ws, int64_t halonr) {
  int centralgal, i;
  struct ModuleContext ctx;
  struct Halo *workspace = ws->halos;

  /* struct ModuleContext's central_index and the process() ABI count a FoF
   * workspace in int. The workspace is bounded by MAX_HALO_ARRAY_SIZE, so this
   * never fires on a valid run. */
  const int ngal = narrow_int64_to_int_checked(ws->count, "FoF workspace galaxy count");

  /* Identify the FOF Type 0 central used for global module context. */
  centralgal = -1;
  for (i = 0; i < ngal; i++) {
    if (workspace[i].Type == 0) {
      centralgal = i;
      break;
    }
  }

  if (centralgal == -1) {
    FATAL_ERROR("No Type 0 central found for FOF halo %" PRId64 " (ngal=%d)", halonr, ngal);
  }

  if (workspace[centralgal].HaloNr != halonr) {
    FATAL_ERROR("Central galaxy HaloNr=%lld does not match FOF halo %" PRId64,
                workspace[centralgal].HaloNr, halonr);
  }

  /* Set FOF-host central unique ID for all members (stable output contract). */
  for (i = 0; i < ngal; i++) {
    workspace[i].UniqueCentralGalaxyID = workspace[centralgal].UniqueGalaxyID;
  }

  /* Setup module execution context */
  setup_module_context(&ctx, view, workspace, halonr, centralgal);

  /* Every row from here on is a record created by the pipeline; the marshaller
   * places each one after its host's subhalo slice (output_buffer.c). */
  ws->base_count = ws->count;

  /* Run the configured module lifecycle over this FoF workspace */
  execute_module_pipeline(&ctx, ws);
}
