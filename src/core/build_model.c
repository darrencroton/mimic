/**
 * @file    build_model.c
 *
 * This file contains the core algorithms for tracking halos from
 * merger trees and managing halo evolution through the simulation.
 *
 * Key functions:
 * - build_halo_tree(): Recursive function to build halo tracking structures
 * - join_progenitor_halos(): Tree-driver gather step that prepares inheritance
 *   payloads and calls the shared inheritance service
 * - process_halo_evolution(): Tree-driver adapter that evolves a FoF workspace
 *   through the shared physics-execution engine
 * - marshal_workspace_to_output_buffer(): Shared output-buffer marshalling
 *
 * This file owns tree traversal, tree-indexed progenitor lookup, and tree-owned
 * buffer management. Format-neutral inheritance science lives in inheritance.c.
 *
 * References:
 * - Croton et al. (2006) - Original semi-analytic model framework
 */

#include <assert.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "globals.h"
#include "inheritance.h"
#include "module_registry.h"
#include "numeric.h"
#include "output_buffer.h"
#include "proto.h"
#include "types.h"
#include "generated/tree_property_accessors.h"

static int count_fof_subhalos(int first_fof_halo);
static struct OutputBufferSegment *ensure_output_segment_scratch(int required);

/**
 * @brief   Recursively constructs halos by traversing the merger tree
 *
 * @param   halonr               Index of the current halo in the Halo array
 * @param   unit                 Index of the current unit (merger tree)
 * @param   partition_output_id  Output id of the partition (filenr-equivalent)
 * @param   depth                Current recursion depth (starts at 0)
 *
 * This function traverses the merger tree in a depth-first manner to ensure
 * that halos are constructed from their progenitors before being evolved.
 * It follows these steps:
 *
 * 1. First processes all progenitors of the current halo
 * 2. Then processes all halos in the same FOF group
 * 3. Finally, gathers progenitor galaxies, applies shared inheritance, and
 *    evolves them forward in time
 *
 * The recursive approach ensures that halos are built in the correct
 * chronological order, preserving the flow of mass and properties from
 * high redshift to low redshift.
 */
void build_halo_tree(int halonr, int unit, int partition_output_id, int depth) {
  int prog, fofhalo, ngal;

  /* Check recursion depth */
  if (depth > MimicConfig.MaxTreeDepth) {
    FATAL_ERROR("Tree recursion depth (%d) exceeds MaxTreeDepth (%d) for halo "
                "%d in tree %d",
                depth, MimicConfig.MaxTreeDepth, halonr, unit);
  }

  HaloAux[halonr].DoneFlag = 1;

  prog = mimic_tree_get_FirstProgenitor(halonr);
  while (prog >= 0) {
    if (HaloAux[prog].DoneFlag == 0)
      build_halo_tree(prog, unit, partition_output_id, depth + 1);
    prog = mimic_tree_get_NextProgenitor(prog);
  }

  fofhalo = mimic_tree_get_FirstHaloInFOFgroup(halonr);
  if (HaloAux[fofhalo].HaloFlag == 0) {
    HaloAux[fofhalo].HaloFlag = 1;
    while (fofhalo >= 0) {
      prog = mimic_tree_get_FirstProgenitor(fofhalo);
      while (prog >= 0) {
        if (HaloAux[prog].DoneFlag == 0)
          build_halo_tree(prog, unit, partition_output_id, depth + 1);
        prog = mimic_tree_get_NextProgenitor(prog);
      }

      fofhalo = mimic_tree_get_NextHaloInFOFgroup(fofhalo);
    }
  }

  // At this point, the halos for all progenitors of this halo have been
  // properly constructed. Also, the halos of the progenitors of all other
  // halos in the same FOF group have been constructed as well. We can hence go
  // ahead and construct all halos for the subhalos in this FOF halo, and
  // evolve them in time.

  fofhalo = mimic_tree_get_FirstHaloInFOFgroup(halonr);
  if (HaloAux[fofhalo].HaloFlag == 1) {
    ngal = 0;
    HaloAux[fofhalo].HaloFlag = 2;

    int nsegments = count_fof_subhalos(fofhalo);
    struct OutputBufferSegment *segments = ensure_output_segment_scratch(nsegments);
    int segment_index = 0;

    while (fofhalo >= 0) {
      int workspace_start = ngal;
      int source_halo = fofhalo;
      ngal = join_progenitor_halos(fofhalo, ngal, unit, partition_output_id);

      /*
       * Stamp the FoF-central catalog virial mass onto every member of this
       * subhalo slice now, before physics runs, so CentralMvir is physically
       * correct whenever a module could observe it on the workspace - not only
       * at output time. CentralMvir is a structural per-FoF-group constant (the
       * input-catalog Mvir of the FOF central); physics never writes it, so the
       * value still reaches output unchanged and the shared marshaller no
       * longer needs to know about this field.
       */
      float central_mvir = (float)get_virial_mass(mimic_tree_get_FirstHaloInFOFgroup(source_halo));
      for (int p = workspace_start; p < ngal; p++) {
        FoFWorkspace[p].CentralMvir = central_mvir;
      }

      segments[segment_index].source_id = source_halo;
      segments[segment_index].snapshot_number = mimic_tree_get_SnapNum(source_halo);
      segments[segment_index].workspace_start = workspace_start;
      segments[segment_index].workspace_count = ngal - workspace_start;
      segments[segment_index].output_first = -1;
      segments[segment_index].output_count = 0;
      segment_index++;

      fofhalo = mimic_tree_get_NextHaloInFOFgroup(fofhalo);
    }

    /* Tree driver: run physics, then marshal the workspace to output. */
    process_halo_evolution(mimic_tree_get_FirstHaloInFOFgroup(halonr), ngal);

    struct OutputBuffer output_buffer = {ProcessedHalos, NumProcessedHalos, MaxProcessedHalos};
    marshal_workspace_to_output_buffer(FoFWorkspace, &output_buffer, segments, segment_index);
    NumProcessedHalos = output_buffer.count;
    ProcessedHalos = output_buffer.halos;
    MaxProcessedHalos = output_buffer.capacity;

    for (int i = 0; i < segment_index; i++) {
      HaloAux[segments[i].source_id].FirstHalo = segments[i].output_first;
      HaloAux[segments[i].source_id].NHalos = segments[i].output_count;
    }
  }
}

/**
 * @brief   Finds the most massive progenitor halo that contains an object
 *
 * @param   halonr    Index of the current halo in the Halo array
 * @return  Index of the most massive progenitor with an object
 *
 * This function scans all progenitors of a halo to find the most massive one
 * that actually contains an object. This is important because not all dark
 * matter halos necessarily host objects, and we need to identify the main
 * branch for inheriting object properties.
 *
 * Two criteria are tracked:
 * 1. The most massive progenitor overall (by particle count)
 * 2. The most massive progenitor that contains an object
 *
 * The function returns the index of the most massive progenitor containing an
 * object, which is used to determine which object should become the central
 * of the descendant halo.
 */
int find_most_massive_progenitor(int halonr) {
  int prog, first_occupied, lenoccmax;

  lenoccmax = 0;
  first_occupied = mimic_tree_get_FirstProgenitor(halonr);
  prog = mimic_tree_get_FirstProgenitor(halonr);

  if (prog >= 0)
    if (HaloAux[prog].NHalos > 0)
      lenoccmax = -1;

  // Find most massive progenitor that contains an actual object
  // Maybe FirstProgenitor never was FirstHaloInFOFGroup and thus has no object
  while (prog >= 0) {
    if (lenoccmax != -1 && mimic_tree_get_Len(prog) > lenoccmax && HaloAux[prog].NHalos > 0) {
      lenoccmax = mimic_tree_get_Len(prog);
      first_occupied = prog;
    }
    prog = mimic_tree_get_NextProgenitor(prog);
  }

  return first_occupied;
}

/**
 * @brief   Copies and updates halos from progenitor halos to the current
 * snapshot
 *
 * @param   halonr          Index of the current halo in the Halo array
 * @param   ngalstart       Starting index for halos in the Gal array
 * @param   first_occupied  Index of the most massive progenitor with halos
 * @param   tree            Index of the current merger tree
 * @param   filenr          File number in multi-file run
 * @return  Updated number of halos after copying
 *
 * This function transfers halos from progenitor halos to the current
 * snapshot, updating their properties based on the new halo structure. It
 * handles:
 *
 * 1. Copying halos from all progenitors to the temporary Gal array
 * 2. Updating object properties based on their new host halo
 * 3. Handling type transitions (central → satellite → orphan)
 * 4. Setting appropriate merger times for satellites
 * 5. Creating new halos when a halo has no progenitor halos
 *
 * The function maintains the continuity of object evolution by preserving
 * their properties while updating their status based on the evolving
 * dark matter structures.
 */
static int count_progenitor_galaxies(int halonr) {
  int count = 0;
  int prog = mimic_tree_get_FirstProgenitor(halonr);

  while (prog >= 0) {
    count += HaloAux[prog].NHalos;
    prog = mimic_tree_get_NextProgenitor(prog);
  }

  return count;
}

static void ensure_fof_workspace_capacity(int required) {
  while (required > MaxFoFWorkspace) {
    int old_size = MaxFoFWorkspace;
    int new_size = (int)(MaxFoFWorkspace * HALO_ARRAY_GROWTH_FACTOR);

    if (new_size - MaxFoFWorkspace < MIN_HALO_ARRAY_GROWTH)
      new_size = MaxFoFWorkspace + MIN_HALO_ARRAY_GROWTH;

    if (new_size > MAX_HALO_ARRAY_SIZE)
      new_size = MAX_HALO_ARRAY_SIZE;

    if (new_size <= MaxFoFWorkspace) {
      FATAL_ERROR("FoF workspace requires %d halos but maximum allowed size is %d", required,
                  MAX_HALO_ARRAY_SIZE);
    }

    INFO_LOG("Growing halo array from %d to %d elements", MaxFoFWorkspace, new_size);

    MaxFoFWorkspace = new_size;
    FoFWorkspace = myrealloc_cat(FoFWorkspace, MaxFoFWorkspace * sizeof(struct Halo), MEM_HALOS);
    memset(&FoFWorkspace[old_size], 0, (new_size - old_size) * sizeof(struct Halo));
  }
}

static long long make_unique_galaxy_id(int halonr, int unit, int partition_output_id) {
  long long file_mul_fac = (MimicConfig.LastFile >= 10000) ? (FILENR_MUL_FAC / 10) : FILENR_MUL_FAC;
  long long unit_mul = TREE_MUL_FAC * unit;
  long long partition_mul = file_mul_fac * partition_output_id;

  return (long long)halonr + unit_mul + partition_mul;
}

/*
 * Build the driver-neutral descendant payload from the tree input. The field
 * population is generated from property metadata (see the included file), so it
 * cannot silently desync from struct HaloInitPayload when halo properties are
 * added. This is the only place tree-index coupling touches halo init; the
 * consumer (init_halo_from_payload) is format-neutral.
 */
static struct HaloInitPayload make_halo_init_payload(int halonr) {
  struct HaloInitPayload payload;

#include "../include/generated/populate_halo_payload_from_tree.inc"

  return payload;
}

/*
 * Reusable scratch for the gather step: the progenitor-galaxy list for one
 * descendant subhalo. Grown monotonically and kept for the whole run so the
 * depth-first tree hot path does not allocate per subhalo. Freed at shutdown by
 * free_tree_driver_scratch(). The non-LIFO allocator (see memory.c) makes
 * whole-run persistence safe alongside the per-tree FoFWorkspace.
 */
static struct InheritanceProgenitorGalaxy *ProgenitorScratch = NULL;
static int ProgenitorScratchCapacity = 0;
static struct OutputBufferSegment *OutputSegmentScratch = NULL;
static int OutputSegmentScratchCapacity = 0;

static struct InheritanceProgenitorGalaxy *ensure_progenitor_scratch(int required) {
  if (required > ProgenitorScratchCapacity) {
    ProgenitorScratch = myrealloc_cat(
        ProgenitorScratch, required * sizeof(struct InheritanceProgenitorGalaxy), MEM_HALOS);
    ProgenitorScratchCapacity = required;
  }
  return ProgenitorScratch;
}

static int count_fof_subhalos(int first_fof_halo) {
  int count = 0;
  int fofhalo = first_fof_halo;

  while (fofhalo >= 0) {
    count++;
    fofhalo = mimic_tree_get_NextHaloInFOFgroup(fofhalo);
  }

  return count;
}

static struct OutputBufferSegment *ensure_output_segment_scratch(int required) {
  if (required > OutputSegmentScratchCapacity) {
    OutputSegmentScratch = myrealloc_cat(OutputSegmentScratch,
                                         required * sizeof(struct OutputBufferSegment), MEM_HALOS);
    OutputSegmentScratchCapacity = required;
  }
  return OutputSegmentScratch;
}

void free_tree_driver_scratch(void) {
  if (ProgenitorScratch != NULL) {
    myfree(ProgenitorScratch);
    ProgenitorScratch = NULL;
    ProgenitorScratchCapacity = 0;
  }
  if (OutputSegmentScratch != NULL) {
    myfree(OutputSegmentScratch);
    OutputSegmentScratch = NULL;
    OutputSegmentScratchCapacity = 0;
  }
}

static void gather_progenitor_galaxies(int halonr, int first_occupied,
                                       struct InheritanceProgenitorGalaxy *progenitors) {
  int index = 0;
  int prog = mimic_tree_get_FirstProgenitor(halonr);

  while (prog >= 0) {
    for (int i = 0; i < HaloAux[prog].NHalos; i++) {
      const struct Halo *source = &ProcessedHalos[HaloAux[prog].FirstHalo + i];
      progenitors[index].source = source;
      progenitors[index].source_time = Age[source->SnapNum];
      progenitors[index].is_main_branch = (prog == first_occupied);
      index++;
    }

    prog = mimic_tree_get_NextProgenitor(prog);
  }
}

/**
 * @brief   Main function to join halos from progenitor halos
 *
 * @param   halonr       Index of the current halo in the Halo array
 * @param   ngalstart    Starting index for halos in the Gal array
 * @param   tree         Index of the current merger tree
 * @param   filenr       File number in multi-file run
 * @return  Updated number of halos after joining
 *
 * This function coordinates the process of integrating halos from
 * progenitor halos into the current halo. It performs two main steps:
 *
 * 1. Identifies the most massive progenitor with halos
 * 2. Copies and updates halos from all progenitors
 *
 * Note: Central-satellite relationships are established here per subhalo slice.
 * This preserves SAGE parity where Type 2 satellites can reference a Type 1
 * subhalo central instead of always pointing to the FOF Type 0 central.
 *
 * The function ensures proper inheritance of object properties while
 * maintaining the hierarchy of central and satellite halos.
 */
int join_progenitor_halos(int halonr, int ngalstart, int unit, int partition_output_id) {
  int current_snap, first_occupied, ngal, nprogenitors, required;
  struct InheritanceDescendant descendant;
  struct InheritanceProgenitorGalaxy *progenitors = NULL;

  /* Find the most massive progenitor with halos */
  first_occupied = find_most_massive_progenitor(halonr);

  nprogenitors = count_progenitor_galaxies(halonr);
  required = ngalstart + nprogenitors;
  if (nprogenitors == 0 && halonr == mimic_tree_get_FirstHaloInFOFgroup(halonr)) {
    required++;
  }
  ensure_fof_workspace_capacity(required);

  if (nprogenitors > 0) {
    progenitors = ensure_progenitor_scratch(nprogenitors);
    gather_progenitor_galaxies(halonr, first_occupied, progenitors);
  }

  current_snap = mimic_tree_get_SnapNum(halonr);
  descendant.halo_nr = halonr;
  descendant.current_snap = current_snap;
  descendant.current_time = Age[current_snap];
  descendant.new_halo_dt = (current_snap > 0) ? Age[current_snap - 1] - Age[current_snap] : -1.0;
  descendant.virial_mass = get_virial_mass(halonr);
  descendant.virial_radius = get_virial_radius(halonr);
  descendant.virial_velocity = get_virial_velocity(halonr);
  descendant.is_fof_central = (halonr == mimic_tree_get_FirstHaloInFOFgroup(halonr));
  descendant.unique_galaxy_id = make_unique_galaxy_id(halonr, unit, partition_output_id);
  descendant.halo_payload = make_halo_init_payload(halonr);

  ngal = inherit_descendant_halos(FoFWorkspace, ngalstart, MaxFoFWorkspace, &descendant,
                                  progenitors, nprogenitors);

  return ngal;
}

/**
 * @brief   Setup module context for current snapshot and FOF group
 *
 * @param   ctx          Module context to populate
 * @param   halonr       Index of main halo in InputTreeHalos
 * @param   centralgal   Index of central galaxy in FoFWorkspace
 */
static void setup_module_context(struct ModuleContext *ctx, int halonr, int centralgal) {
  int snap = mimic_tree_get_SnapNum(halonr);

  /* Snapshot information */
  ctx->redshift = MimicConfig.ZZ[snap];
  ctx->time = Age[snap];
  ctx->snapshot_number = snap;

  /* Halo information */
  ctx->central_index = centralgal;
  ctx->central_galaxy = &FoFWorkspace[centralgal];
  ctx->active_event = NULL;

  /* Configuration access */
  ctx->params = &MimicConfig;

  /* Determine number of substeps (default to 1 if not specified) */
  ctx->num_substeps = (MimicConfig.SubSteps > 0) ? MimicConfig.SubSteps : 1;

  /* Calculate total time interval for this timestep.
   *
   * INVARIANT: workspace halos still carry their *progenitor's* SnapNum at
   * this point — inheritance copies it unchanged, and it is only advanced to
   * the current snapshot when the workspace is marshalled to the output
   * buffer. That is what makes Age[SnapNum] here the progenitor age. */
  if (FoFWorkspace[centralgal].SnapNum >= 0) {
    int prev_snap = FoFWorkspace[centralgal].SnapNum;
    ctx->time_interval = Age[prev_snap] - Age[snap];
  } else {
    ctx->time_interval = 0.0; /* First snapshot has no previous */
  }

  /* Initialize substep information (updated in substep loop) */
  ctx->substep_number = 0;
  ctx->substep_time = ctx->time;
  ctx->substep_dt = (ctx->num_substeps > 0) ? (ctx->time_interval / ctx->num_substeps) : 0.0;
}

/**
 * @brief   Evolve one FoF workspace through the physics-execution engine
 *
 * @param   halonr    Index of the FOF-background subhalo (main halo)
 * @param   ngal      Total number of halos to process
 *
 * Tree-driver adapter for physics execution: selects the FOF Type 0 central,
 * propagates the stable central unique ID, builds the (tree-coupled) module
 * context, and hands the workspace to the format-neutral physics-execution
 * engine. Output marshalling is a separate, driver-owned step performed by the
 * caller through the shared output-buffer marshaller.
 *
 * Phase assignments and loop modes are configured in the input YAML file.
 * SubSteps parameter controls time sub-stepping (0 or 1 = no substeps).
 */
void process_halo_evolution(int halonr, int ngal) {
  int centralgal, i;
  struct ModuleContext ctx;

  /* Identify the FOF Type 0 central used for global module context. */
  centralgal = -1;
  for (i = 0; i < ngal; i++) {
    if (FoFWorkspace[i].Type == 0) {
      centralgal = i;
      break;
    }
  }

  if (centralgal == -1) {
    FATAL_ERROR("No Type 0 central found for FOF halo %d (ngal=%d)", halonr, ngal);
  }

  if (FoFWorkspace[centralgal].HaloNr != halonr) {
    FATAL_ERROR("Central galaxy HaloNr=%d does not match FOF halo %d",
                FoFWorkspace[centralgal].HaloNr, halonr);
  }

  /* Set FOF-host central unique ID for all members (stable output contract). */
  for (i = 0; i < ngal; i++) {
    FoFWorkspace[i].UniqueCentralGalaxyID = FoFWorkspace[centralgal].UniqueGalaxyID;
  }

  /* Setup module execution context */
  setup_module_context(&ctx, halonr, centralgal);

  /* Run the configured module lifecycle over this FoF workspace */
  execute_module_pipeline(&ctx, FoFWorkspace, ngal);
}
