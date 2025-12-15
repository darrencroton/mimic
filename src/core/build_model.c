/**
 * @file    core_build_model.c
 *
 * This file contains the core algorithms for tracking halos from
 * merger trees and managing halo evolution through the simulation.
 *
 * Key functions:
 * - build_halo_tree(): Recursive function to build halo tracking structures
 * - join_progenitor_halos(): Integrates halos from progenitor structures
 * - process_halo_evolution(): Updates halo properties through time
 *
 * This file implements the core halo tracking infrastructure that forms the
 * foundation for the physics-agnostic framework.
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
#include "proto.h"
#include "module_registry.h"
#include "globals.h"
#include "types.h"
#include "numeric.h"

/**
 * @brief   Recursively constructs halos by traversing the merger tree
 *
 * @param   halonr    Index of the current halo in the Halo array
 * @param   tree      Index of the current merger tree
 * @param   depth     Current recursion depth (starts at 0)
 *
 * This function traverses the merger tree in a depth-first manner to ensure
 * that halos are constructed from their progenitors before being evolved.
 * It follows these steps:
 *
 * 1. First processes all progenitors of the current halo
 * 2. Then processes all halos in the same FOF group
 * 3. Finally, joins progenitor halos and evolves them forward in time
 *
 * The recursive approach ensures that halos are built in the correct
 * chronological order, preserving the flow of mass and properties from
 * high redshift to low redshift.
 */
void build_halo_tree(int halonr, int tree, int filenr, int depth) {
  int prog, fofhalo, ngal;

  /* Check recursion depth */
  if (depth > MimicConfig.MaxTreeDepth) {
    FATAL_ERROR("Tree recursion depth (%d) exceeds MaxTreeDepth (%d) for halo "
                "%d in tree %d",
                depth, MimicConfig.MaxTreeDepth, halonr, tree);
  }

  HaloAux[halonr].DoneFlag = 1;

  prog = InputTreeHalos[halonr].FirstProgenitor;
  while (prog >= 0) {
    if (HaloAux[prog].DoneFlag == 0)
      build_halo_tree(prog, tree, filenr, depth + 1);
    prog = InputTreeHalos[prog].NextProgenitor;
  }

  fofhalo = InputTreeHalos[halonr].FirstHaloInFOFgroup;
  if (HaloAux[fofhalo].HaloFlag == 0) {
    HaloAux[fofhalo].HaloFlag = 1;
    while (fofhalo >= 0) {
      prog = InputTreeHalos[fofhalo].FirstProgenitor;
      while (prog >= 0) {
        if (HaloAux[prog].DoneFlag == 0)
          build_halo_tree(prog, tree, filenr, depth + 1);
        prog = InputTreeHalos[prog].NextProgenitor;
      }

      fofhalo = InputTreeHalos[fofhalo].NextHaloInFOFgroup;
    }
  }

  // At this point, the halos for all progenitors of this halo have been
  // properly constructed. Also, the halos of the progenitors of all other
  // halos in the same FOF group have been constructed as well. We can hence go
  // ahead and construct all halos for the subhalos in this FOF halo, and
  // evolve them in time.

  fofhalo = InputTreeHalos[halonr].FirstHaloInFOFgroup;
  if (HaloAux[fofhalo].HaloFlag == 1) {
    ngal = 0;
    HaloAux[fofhalo].HaloFlag = 2;

    while (fofhalo >= 0) {
      ngal = join_progenitor_halos(fofhalo, ngal, tree, filenr);
      fofhalo = InputTreeHalos[fofhalo].NextHaloInFOFgroup;
    }

    /* Set central references for entire FOF group
     * Called here (not per-subhalo) to ensure all galaxies point to the
     * FOF group's Type 0 central, including satellites and orphans from
     * satellite subhalos */
    set_halo_centrals(0, ngal);

    process_halo_evolution(InputTreeHalos[halonr].FirstHaloInFOFgroup, ngal);
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
  int prog, first_occupied, lenmax, lenoccmax;

  lenmax = 0;
  lenoccmax = 0;
  first_occupied = InputTreeHalos[halonr].FirstProgenitor;
  prog = InputTreeHalos[halonr].FirstProgenitor;

  if (prog >= 0)
    if (HaloAux[prog].NHalos > 0)
      lenoccmax = -1;

  // Find most massive progenitor that contains an actual object
  // Maybe FirstProgenitor never was FirstHaloInFOFGroup and thus has no object
  while (prog >= 0) {
    if (InputTreeHalos[prog].Len > lenmax) {
      lenmax = InputTreeHalos[prog].Len;
      /* mother_halo = prog; */
    }
    if (lenoccmax != -1 && InputTreeHalos[prog].Len > lenoccmax &&
        HaloAux[prog].NHalos > 0) {
      lenoccmax = InputTreeHalos[prog].Len;
      first_occupied = prog;
    }
    prog = InputTreeHalos[prog].NextProgenitor;
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
int copy_progenitor_halos(int halonr, int ngalstart, int first_occupied, int tree, int filenr) {
  int ngal, prog, i, j;
  double previousMvir, previousVvir, previousVmax;

  ngal = ngalstart;
  prog = InputTreeHalos[halonr].FirstProgenitor;

  while (prog >= 0) {
    for (i = 0; i < HaloAux[prog].NHalos; i++) {
      if (ngal == (MaxFoFWorkspace - 1)) {
        /* Calculate new size using growth factor */
        int new_size = (int)(MaxFoFWorkspace * HALO_ARRAY_GROWTH_FACTOR);

        /* Ensure minimum growth to prevent too-frequent reallocations */
        if (new_size - MaxFoFWorkspace < MIN_HALO_ARRAY_GROWTH)
          new_size = MaxFoFWorkspace + MIN_HALO_ARRAY_GROWTH;

        /* Cap maximum size to prevent excessive memory usage */
        if (new_size > MAX_HALO_ARRAY_SIZE)
          new_size = MAX_HALO_ARRAY_SIZE;

        INFO_LOG("Growing halo array from %d to %d elements", MaxFoFWorkspace,
                 new_size);

        int old_size = MaxFoFWorkspace;

        /* Reallocate with new size */
        MaxFoFWorkspace = new_size;
        FoFWorkspace =
            myrealloc(FoFWorkspace, MaxFoFWorkspace * sizeof(struct Halo));

        /* Zero the newly allocated entries to ensure galaxy pointers are NULL */
        memset(&FoFWorkspace[old_size], 0, (new_size - old_size) * sizeof(struct Halo));
      }
      assert(ngal < MaxFoFWorkspace);

      // This is the crucial line in which the properties of the progenitor
      // halos are copied over (as a whole) to the (temporary) halos
      // FoFWorkspace[xxx] in the current snapshot After updating their
      // properties and evolving them they are copied to the end of the list of
      // permanent halos ProcessedHalos[xxx]
      FoFWorkspace[ngal] = ProcessedHalos[HaloAux[prog].FirstHalo + i];

      // Deep copy galaxy data to prevent shared memory corruption across snapshots
      // Without this, multiple halos would share the same galaxy pointer, causing
      // module updates to corrupt previous snapshots' data
      if (ProcessedHalos[HaloAux[prog].FirstHalo + i].galaxy != NULL) {
        FoFWorkspace[ngal].galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_HALOS);
        memcpy(FoFWorkspace[ngal].galaxy,
               ProcessedHalos[HaloAux[prog].FirstHalo + i].galaxy,
               sizeof(struct GalaxyData));

        // Reset snapshot-scoped accumulator properties (auto-generated from metadata)
        // These properties track values during a single snapshot and must start fresh
        // This happens for ALL galaxies (including orphans) after deep copy
        #include "../include/generated/reset_galaxy_properties.inc"
      }

      FoFWorkspace[ngal].HaloNr = halonr;

      // Calculate time step from progenitor snapshot to current snapshot
      // FoFWorkspace[ngal] contains progenitor data copied from ProcessedHalos
      // Note: Age[] is lookback time, so it decreases with snapshot number
      // Therefore dT = Age[progenitor] - Age[current] gives positive timestep
      int current_snap = InputTreeHalos[halonr].SnapNum;
      int progenitor_snap = FoFWorkspace[ngal].SnapNum;  // From copied progenitor
      FoFWorkspace[ngal].dT = Age[progenitor_snap] - Age[current_snap];

      // Skip halos that have already merged (marked in previous snapshot)
      if (FoFWorkspace[ngal].Type == 3) {
        // Free galaxy data to prevent memory leak (allocated above but not needed)
        if (FoFWorkspace[ngal].galaxy != NULL) {
          myfree(FoFWorkspace[ngal].galaxy);
          FoFWorkspace[ngal].galaxy = NULL;
        }
        continue;
      }

      // this deals with the central halos of (sub)halos
      if (FoFWorkspace[ngal].Type == 0 || FoFWorkspace[ngal].Type == 1) {
        // remember properties from the last snapshot
        previousMvir = FoFWorkspace[ngal].Mvir;
        previousVvir = FoFWorkspace[ngal].Vvir;
        previousVmax = FoFWorkspace[ngal].Vmax;

        if (prog == first_occupied) {
          // update properties of this object with physical properties of halo
          FoFWorkspace[ngal].MostBoundID = InputTreeHalos[halonr].MostBoundID;

          for (j = 0; j < 3; j++) {
            FoFWorkspace[ngal].Pos[j] = InputTreeHalos[halonr].Pos[j];
            FoFWorkspace[ngal].Vel[j] = InputTreeHalos[halonr].Vel[j];
          }

          FoFWorkspace[ngal].Len = InputTreeHalos[halonr].Len;
          FoFWorkspace[ngal].Vmax = InputTreeHalos[halonr].Vmax;
          FoFWorkspace[ngal].VelDisp = InputTreeHalos[halonr].VelDisp;

          FoFWorkspace[ngal].deltaMvir =
              get_virial_mass(halonr) - FoFWorkspace[ngal].Mvir;

          if (get_virial_mass(halonr) > FoFWorkspace[ngal].Mvir) {
            FoFWorkspace[ngal].Rvir =
                get_virial_radius(halonr); // use the maximum Rvir in model
            FoFWorkspace[ngal].Vvir =
                get_virial_velocity(halonr); // use the maximum Vvir in model
          }
          FoFWorkspace[ngal].Mvir = get_virial_mass(halonr);

          if (halonr == InputTreeHalos[halonr].FirstHaloInFOFgroup) {
            // a central
            FoFWorkspace[ngal].MergTime = 999.9;

            FoFWorkspace[ngal].Type = 0;
          } else {
            // a satellite with subhalo

            if (FoFWorkspace[ngal].Type ==
                0) // remember the infall properties before becoming a subhalo
            {
              FoFWorkspace[ngal].infallMvir = previousMvir;
              FoFWorkspace[ngal].infallVvir = previousVvir;
              FoFWorkspace[ngal].infallVmax = previousVmax;
            }

            if (FoFWorkspace[ngal].Type == 0 ||
                FoFWorkspace[ngal].MergTime > 999.0)
              // here the halo has gone from type 1 to type 2 or otherwise
              // doesn't have a merging time.
              FoFWorkspace[ngal].MergTime =
                  999.9; /* No merging without physics */

            FoFWorkspace[ngal].Type = 1;
          }
        } else {
          // an orphan satellite
          FoFWorkspace[ngal].Len = 0;
          FoFWorkspace[ngal].Mvir = 0.0;
          FoFWorkspace[ngal].deltaMvir = 0.0;

          if (FoFWorkspace[ngal].Type == 0) {
            // here the halo has gone from type 0 to type 2
            FoFWorkspace[ngal].infallMvir = previousMvir;
            FoFWorkspace[ngal].infallVvir = previousVvir;
            FoFWorkspace[ngal].infallVmax = previousVmax;
          }

          FoFWorkspace[ngal].Type = 2;
        }
      }

      ngal++;
    }

    prog = InputTreeHalos[prog].NextProgenitor;
  }

  if (ngal == ngalstart) {
    // We have no progenitors with halos. This means we create a new object.
    // init_halo requires halonr to be the main subhalo
    if (halonr == InputTreeHalos[halonr].FirstHaloInFOFgroup) {
      init_halo(ngal, halonr, tree, filenr);
      ngal++;
    }
    // If not the main subhalo, we don't create an object
  }

  return ngal;
}

/**
 * @brief   Sets the central object reference for all galaxies in a FOF group
 *
 * @param   ngalstart    Starting index of galaxies for this FOF group
 * @param   ngal         Ending index (exclusive) of galaxies for this FOF group
 *
 * This function identifies the Type 0 central galaxy for a FOF group
 * and sets ALL galaxies (including Type 1 satellites and Type 2 orphans) to
 * reference this central galaxy. This ensures UniqueCentralGalaxyID correctly points
 * to the FOF group's main central, not to individual satellite subhalo centrals.
 *
 * Each FOF group has exactly one Type 0 central, possibly multiple Type 1
 * satellites (satellite subhalo centrals), and any number of Type 2 orphans.
 */
void set_halo_centrals(int ngalstart, int ngal) {
  int i, centralgal;

  /* Find the Type 0 central galaxy for this FOF group
   * Note: FOF groups can have multiple Type 1 satellites, but only one Type 0 */
  for (i = ngalstart, centralgal = -1; i < ngal; i++) {
    if (FoFWorkspace[i].Type == 0) {
      if (centralgal != -1) {
        ERROR_LOG("Multiple Type 0 galaxies in FOF group (range %d-%d)", ngalstart, ngal);
        assert(centralgal == -1);
      }
      centralgal = i;
      break; /* Found it, no need to continue */
    }
  }

  /* Set ALL galaxies to point to the FOF group's Type 0 central */
  for (i = ngalstart; i < ngal; i++)
    FoFWorkspace[i].CentralHalo = centralgal;
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
 * Note: Central-satellite relationships are established in build_halo_tree()
 * after all subhalos in the FOF group have been processed, ensuring all
 * galaxies reference the FOF group's Type 0 central.
 *
 * The function ensures proper inheritance of object properties while
 * maintaining the hierarchy of central and satellite halos.
 */
int join_progenitor_halos(int halonr, int ngalstart, int tree, int filenr) {
  int ngal, first_occupied;

  /* Find the most massive progenitor with halos */
  first_occupied = find_most_massive_progenitor(halonr);

  /* Copy halos from progenitors to the current snapshot */
  ngal = copy_progenitor_halos(halonr, ngalstart, first_occupied, tree, filenr);

  /* Central assignment deferred to build_halo_tree() which processes entire
   * FOF group at once (see set_halo_centrals call after subhalo loop) */

  return ngal;
}

/**
 * @brief   Attaches halo tracking structures to halos for output
 *
 * @param   ngal          Total number of halos in this structure
 *
 * This function attaches halo tracking structures to halos for output.
 * Simply copies halo structures to output array (ProcessedHalos).
 */
void update_halo_properties(int ngal) {
  int p, currenthalo;

  /* Attach final list to halos */
  for (p = 0, currenthalo = -1; p < ngal; p++) {
    /* When processing a new halo, update its pointers */
    if (FoFWorkspace[p].HaloNr != currenthalo) {
      currenthalo = FoFWorkspace[p].HaloNr;
      HaloAux[currenthalo].FirstHalo =
          NumProcessedHalos;           /* Index of first one in this halo */
      HaloAux[currenthalo].NHalos = 0; /* Reset counter */
    }

    /* Set UniqueCentralGalaxyID from the central halo's UniqueGalaxyID
     * This must be done for ALL galaxies (not just non-merged) to maintain
     * consistency. Uses FoFWorkspace relationships (set by set_halo_centrals)
     * which are always correct, avoiding fragile tree lookups. */
    int central_idx = FoFWorkspace[p].CentralHalo;
    FoFWorkspace[p].UniqueCentralGalaxyID = FoFWorkspace[central_idx].UniqueGalaxyID;

    /* Copy non-merged halos to the permanent array
     * Type=3 halos are skipped (marked by physics modules as merged) */
    if (FoFWorkspace[p].Type != 3) {
      assert(NumProcessedHalos <
             MaxProcessedHalos); /* Ensure we don't exceed array bounds */

      FoFWorkspace[p].SnapNum =
          InputTreeHalos[currenthalo].SnapNum; /* Update snapshot number */
      ProcessedHalos[NumProcessedHalos++] =
          FoFWorkspace[p]; /* Copy to permanent array and increment counter */
      HaloAux[currenthalo].NHalos++; /* Increment count for this halo */
    }
  }
}

/**
 * @brief   Setup module context for current snapshot and FOF group
 *
 * @param   ctx          Module context to populate
 * @param   halonr       Index of main halo in InputTreeHalos
 * @param   centralgal   Index of central galaxy in FoFWorkspace
 */
static void setup_module_context(struct ModuleContext *ctx, int halonr,
                                 int centralgal) {
  int snap = InputTreeHalos[halonr].SnapNum;

  /* Snapshot information */
  ctx->redshift = ZZ[snap];
  ctx->time = Age[snap];
  ctx->snapshot_number = snap;

  /* Halo information */
  ctx->central_index = centralgal;

  /* Configuration access */
  ctx->params = &MimicConfig;

  /* Determine number of substeps (default to 1 if not specified) */
  ctx->num_substeps = (MimicConfig.SubSteps > 0) ? MimicConfig.SubSteps : 1;

  /* Calculate total time interval for this timestep */
  if (FoFWorkspace[centralgal].SnapNum >= 0) {
    int prev_snap = FoFWorkspace[centralgal].SnapNum; /* Previous snapshot */
    ctx->time_interval = Age[prev_snap] - Age[snap];
  } else {
    ctx->time_interval = 0.0; /* First snapshot has no previous */
  }

  /* Initialize substep information (updated in substep loop) */
  ctx->substep_number = 0;
  ctx->substep_time = ctx->time;
  ctx->substep_dt =
      (ctx->num_substeps > 0) ? (ctx->time_interval / ctx->num_substeps) : 0.0;
}

/**
 * @brief   Update context for specific substep
 *
 * @param   ctx     Module context to update
 * @param   step    Current substep number (0-indexed)
 */
static void update_context_for_substep(struct ModuleContext *ctx, int step) {
  ctx->substep_number = step;
  /* Substep time is at midpoint of substep interval */
  ctx->substep_time = ctx->time - (step + 0.5) * ctx->substep_dt;
}

/**
 * @brief   Multi-phase halo evolution with time sub-stepping
 *
 * @param   halonr    Index of the FOF-background subhalo (main halo)
 * @param   ngal      Total number of halos to process
 *
 * This function implements the multi-phase pipeline with optional time
 * sub-stepping:
 * 1. PRE_TIMESTEP: Setup phase (runs once before substeps)
 * 2. SUBSTEP LOOP: Iterates over time substeps
 *    - PHASE_1: First physics phase (each substep)
 *    - PHASE_2: Second physics phase (each substep)
 * 3. POST_TIMESTEP: Finalization phase (runs once after substeps)
 * 4. Update output structures
 *
 * Phase assignments and loop modes are configured in input YAML file.
 * SubSteps parameter controls time sub-stepping (0 or 1 = no substeps).
 */
void process_halo_evolution(int halonr, int ngal) {
  int centralgal;
  struct ModuleContext ctx;

  /* Identify the central galaxy for this FOF group */
  centralgal = FoFWorkspace[0].CentralHalo;
  assert(FoFWorkspace[centralgal].Type == 0 &&
         FoFWorkspace[centralgal].HaloNr == halonr);

  /* Setup module execution context */
  setup_module_context(&ctx, halonr, centralgal);

  /* PHASE 1: Pre-timestep (runs once before substeps) */
  execute_phase(MimicConfig.pre_timestep, MimicConfig.num_pre_timestep, &ctx,
                FoFWorkspace, ngal);

  /* PHASE 2-3: Substep loop (phase_1 and phase_2 run each substep) */
  for (int step = 0; step < ctx.num_substeps; step++) {
    update_context_for_substep(&ctx, step);

    /* PHASE 2A: Phase 1 within substep loop */
    execute_phase(MimicConfig.phase_1, MimicConfig.num_phase_1, &ctx,
                  FoFWorkspace, ngal);

    /* PHASE 2B: Phase 2 within substep loop */
    execute_phase(MimicConfig.phase_2, MimicConfig.num_phase_2, &ctx,
                  FoFWorkspace, ngal);
  }

  /* PHASE 4: Post-timestep (runs once after substeps) */
  execute_phase(MimicConfig.post_timestep, MimicConfig.num_post_timestep, &ctx,
                FoFWorkspace, ngal);

  /* Update final halo properties and attach them to output structures */
  update_halo_properties(ngal);
}
