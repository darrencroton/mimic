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
void build_halo_tree(int halonr, int tree) {
  int prog, fofhalo, ngal;

  HaloAux[halonr].DoneFlag = 1;

  prog = InputTreeHalos[halonr].FirstProgenitor;
  while (prog >= 0) {
    if (HaloAux[prog].DoneFlag == 0)
      build_halo_tree(prog, tree);
    prog = InputTreeHalos[prog].NextProgenitor;
  }

  fofhalo = InputTreeHalos[halonr].FirstHaloInFOFgroup;
  if (HaloAux[fofhalo].HaloFlag == 0) {
    HaloAux[fofhalo].HaloFlag = 1;
    while (fofhalo >= 0) {
      prog = InputTreeHalos[fofhalo].FirstProgenitor;
      while (prog >= 0) {
        if (HaloAux[prog].DoneFlag == 0)
          build_halo_tree(prog, tree);
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
      ngal = join_progenitor_halos(fofhalo, ngal);
      fofhalo = InputTreeHalos[fofhalo].NextHaloInFOFgroup;
    }

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
int copy_progenitor_halos(int halonr, int ngalstart, int first_occupied) {
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
      }

      FoFWorkspace[ngal].HaloNr = halonr;

      // Calculate time step from progenitor snapshot to current snapshot
      // FoFWorkspace[ngal] contains progenitor data copied from ProcessedHalos
      // Note: Age[] is lookback time, so it decreases with snapshot number
      // Therefore dT = Age[progenitor] - Age[current] gives positive timestep
      int current_snap = InputTreeHalos[halonr].SnapNum;
      int progenitor_snap = FoFWorkspace[ngal].SnapNum;  // From copied progenitor
      FoFWorkspace[ngal].dT = Age[progenitor_snap] - Age[current_snap];

      // this deals with the central halos of (sub)halos
      if (FoFWorkspace[ngal].Type == 0 || FoFWorkspace[ngal].Type == 1) {
        // this halo shouldn't hold an object that has already merged; remove it
        // from future processing
        if (FoFWorkspace[ngal].MergeStatus != 0) {
          // Free galaxy data to prevent memory leak (allocated above but not needed)
          if (FoFWorkspace[ngal].galaxy != NULL) {
            myfree(FoFWorkspace[ngal].galaxy);
            FoFWorkspace[ngal].galaxy = NULL;
          }
          FoFWorkspace[ngal].Type = 3;
          continue;
        }

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

          FoFWorkspace[ngal].deltaMvir =
              get_virial_mass(halonr) - FoFWorkspace[ngal].Mvir;

          if (is_greater(get_virial_mass(halonr), FoFWorkspace[ngal].Mvir)) {
            FoFWorkspace[ngal].Rvir =
                get_virial_radius(halonr); // use the maximum Rvir in model
            FoFWorkspace[ngal].Vvir =
                get_virial_velocity(halonr); // use the maximum Vvir in model
          }
          FoFWorkspace[ngal].Mvir = get_virial_mass(halonr);

          if (halonr == InputTreeHalos[halonr].FirstHaloInFOFgroup) {
            // a central
            FoFWorkspace[ngal].MergeStatus = 0;
            FoFWorkspace[ngal].mergeIntoID = -1;
            FoFWorkspace[ngal].MergTime = 999.9;

            FoFWorkspace[ngal].Type = 0;
          } else {
            // a satellite with subhalo
            FoFWorkspace[ngal].MergeStatus = 0;
            FoFWorkspace[ngal].mergeIntoID = -1;

            if (FoFWorkspace[ngal].Type ==
                0) // remember the infall properties before becoming a subhalo
            {
              FoFWorkspace[ngal].infallMvir = previousMvir;
              FoFWorkspace[ngal].infallVvir = previousVvir;
              FoFWorkspace[ngal].infallVmax = previousVmax;
            }

            if (FoFWorkspace[ngal].Type == 0 ||
                is_greater(FoFWorkspace[ngal].MergTime, 999.0))
              // here the halo has gone from type 1 to type 2 or otherwise
              // doesn't have a merging time.
              FoFWorkspace[ngal].MergTime =
                  999.9; /* No merging without physics */

            FoFWorkspace[ngal].Type = 1;
          }
        } else {
          // an orphan satellite - these will merge or disrupt within the
          // current timestep
          FoFWorkspace[ngal].deltaMvir = -1.0 * FoFWorkspace[ngal].Mvir;
          FoFWorkspace[ngal].Mvir = 0.0;

          if (is_greater(FoFWorkspace[ngal].MergTime, 999.0) ||
              FoFWorkspace[ngal].Type == 0) {
            // here the halo has gone from type 0 to type 2 - merge it!
            FoFWorkspace[ngal].MergTime = 0.0;

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
      init_halo(ngal, halonr);
      ngal++;
    }
    // If not the main subhalo, we don't create an object
  }

  return ngal;
}

/**
 * @brief   Sets the central object reference for all halos in a halo
 *
 * @param   ngalstart    Starting index of halos for this halo
 * @param   ngal         Ending index (exclusive) of halos for this halo
 *
 * This function identifies the central object (Type 0 or 1) for a halo
 * and sets all object in the halo to reference this central object.
 * Each halo can have only one Type 0 or Type 1 object, with all others
 * being Type 2 (orphan) halos.
 */
void set_halo_centrals(int ngalstart, int ngal) {
  int i, centralgal;

  /* Per Halo there can be only one Type 0 or 1 object, all others are Type 2
   * (orphan) Find the central object for this halo */
  for (i = ngalstart, centralgal = -1; i < ngal; i++) {
    if (FoFWorkspace[i].Type == 0 || FoFWorkspace[i].Type == 1) {
      assert(centralgal == -1); /* Ensure only one central object per halo */
      centralgal = i;
    }
  }

  /* Set all halos to point to the central object */
  for (i = ngalstart; i < ngal; i++)
    FoFWorkspace[i].CentralHalo = centralgal;
}

/**
 * @brief   Main function to join halos from progenitor halos
 *
 * @param   halonr       Index of the current halo in the Halo array
 * @param   ngalstart    Starting index for halos in the Gal array
 * @return  Updated number of halos after joining
 *
 * This function coordinates the process of integrating halos from
 * progenitor halos into the current halo. It performs three main steps:
 *
 * 1. Identifies the most massive progenitor with halos
 * 2. Copies and updates halos from all progenitors
 * 3. Establishes relationships between halos (central/satellite)
 *
 * The function ensures proper inheritance of object properties while
 * maintaining the hierarchy of central and satellite halos.
 */
int join_progenitor_halos(int halonr, int ngalstart) {
  int ngal, first_occupied;

  /* Find the most massive progenitor with halos */
  first_occupied = find_most_massive_progenitor(halonr);

  /* Copy halos from progenitors to the current snapshot */
  ngal = copy_progenitor_halos(halonr, ngalstart, first_occupied);

  /* Set up central object relationships */
  set_halo_centrals(ngalstart, ngal);

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
  int p, i, currenthalo, offset;

  /* Attach final list to halos */
  offset = 0;
  for (p = 0, currenthalo = -1; p < ngal; p++) {
    /* When processing a new halo, update its pointers */
    if (FoFWorkspace[p].HaloNr != currenthalo) {
      currenthalo = FoFWorkspace[p].HaloNr;
      HaloAux[currenthalo].FirstHalo =
          NumProcessedHalos;           /* Index of first one in this halo */
      HaloAux[currenthalo].NHalos = 0; /* Reset counter */
    }

    /* Calculate offset for merger target IDs due to halos that won't be
     * output */
    offset = 0;
    i = p - 1;
    while (i >= 0) {
      if (FoFWorkspace[i].MergeStatus > 0)
        if (FoFWorkspace[p].mergeIntoID > FoFWorkspace[i].mergeIntoID)
          offset++; /* These halos won't be kept, so offset mergeIntoID */
      i--;
    }

    /* Handle merged halos - update their merger info in the previous
     * snapshot */
    i = -1;
    if (FoFWorkspace[p].MergeStatus > 0) {
      /* Find this object in the previous snapshot's array */
      i = HaloAux[currenthalo].FirstHalo - 1;
      while (i >= 0) {
        if (ProcessedHalos[i].UniqueHaloID == FoFWorkspace[p].UniqueHaloID)
          break;
        else
          i--;
      }

      assert(i >= 0); /* Should always be found */

      /* Update merger information in the previous snapshot's entry */
      ProcessedHalos[i].MergeStatus = FoFWorkspace[p].MergeStatus;
      ProcessedHalos[i].mergeIntoID = FoFWorkspace[p].mergeIntoID - offset;
      ProcessedHalos[i].mergeIntoSnapNum = InputTreeHalos[currenthalo].SnapNum;
    }

    /* Copy non-merged halos to the permanent array */
    if (FoFWorkspace[p].MergeStatus == 0) {
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
 * @brief   Updates halo properties for output
 *
 * @param   halonr    Index of the FOF-background subhalo (main halo)
 * @param   ngal      Total number of halos to process
 *
 * This function updates halo properties and prepares them for output.
 * All physics integration has been removed. Simply updates halo properties
 * and attaches to output structures.
 */
void process_halo_evolution(int halonr, int ngal)
                            /* Note: halonr is here the FOF-background
                                         subhalo (i.e. main halo) */
{
  int centralgal;

  /* Identify the central object for this halo */
  centralgal = FoFWorkspace[0].CentralHalo;
  assert(FoFWorkspace[centralgal].Type == 0 &&
         FoFWorkspace[centralgal].HaloNr == halonr);

  /* Execute galaxy physics modules (if any registered) */
  module_execute_pipeline(halonr, FoFWorkspace, ngal);

  /* Update final object properties and attach them to halos */
  update_halo_properties(ngal);
}
