/**
 * @file    io_save_binary.c
 * @brief   Functions for saving halo data to output files
 *
 * This file contains the functionality for writing tracked halos to output
 * files. It handles the conversion of internal halo tracking structures to the
 * output format, manages file I/O operations, and ensures consistent halo
 * indexing across files. The code supports writing halo data for multiple
 * snapshots and maintains proper cross-references between halos.
 *
 * Key functions:
 * - save_halos(): Writes halos to output files for all requested
 * snapshots
 * - prepare_halo_for_output(): Converts internal halo format to output
 * format
 * - finalize_halo_file(): Completes file writing by updating headers
 *
 * The output files include headers with tree counts and halo counts per tree,
 * followed by the halo data for the corresponding snapshot.
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "allvars.h"
#include "error.h"
#include "output/binary.h"
#include "output/util.h"
#include "proto.h"
#include "util.h"

#define TREE_MUL_FAC (1000000000LL)
#define FILENR_MUL_FAC (1000000000000000LL)

// keep a static file handle to remove the need to do constant seeking.
FILE *save_fd[ABSOLUTEMAXSNAPS] = {0};

#ifndef MAX_BUF_SIZE
#define MAX_BUF_SIZE (3 * MAX_STRING_LEN + 40)
#endif
#define MAX_OUTFILE_SIZE (MAX_STRING_LEN + 40)

/**
 * @brief   Saves output files for all requested snapshots
 *
 * @param   filenr    Current file number being processed
 * @param   tree      Current tree number being processed
 *
 * This function writes all objects for the current tree to their respective
 * output files. For each output snapshot, it:
 *
 * 1. Opens the output file if not already open
 * 2. Writes placeholder headers to be filled later
 * 3. Processes onjects belonging to that snapshot
 * 4. Converts internal halo structures to output format
 * 5. Writes objects to the file
 * 6. Updates halo counts for the file and tree
 *
 * The function also handles the indexing system that allows cross-referencing
 * between objects (e.g., for tracking merger destinations) across different
 * trees and files.
 */
void save_halos(int filenr, int tree) {
  char buf[MAX_BUF_SIZE + 1];
  int i, n;
  /* No need for static output structure anymore since we use dynamic allocation
   */

  int OutputGalCount[MAXSNAPS], *OutputGalOrder, nwritten;

  /* Prepare output ordering and update merger pointers using shared utility */
  OutputGalOrder = prepare_output_for_tree(OutputGalCount);

  // now prepare and write
  for (n = 0; n < MimicConfig.NOUT; n++) {
    // only open the file if it is not already open.
    if (!save_fd[n]) {
      snprintf(buf, MAX_BUF_SIZE, "%s/%s_z%1.3f_%d", MimicConfig.OutputDir,
               MimicConfig.OutputFileBaseName,
               MimicConfig.ZZ[MimicConfig.ListOutputSnaps[n]], filenr);

      /* Open in binary mode with update permissions */
      save_fd[n] = fopen(buf, "wb+");
      if (save_fd[n] == NULL) {
        FATAL_ERROR("Failed to open output halo file '%s' for snapshot %d "
                    "(filenr %d)",
                    buf, MimicConfig.ListOutputSnaps[n], filenr);
      }

      /* Enable full buffering for better write performance (64KB buffer) */
      setvbuf(save_fd[n], NULL, _IOFBF, 65536);

      /* Write out placeholders for the header data */
      size_t size =
          (Ntrees + 2) *
          sizeof(int); /* Extra two integers are for saving the total number of
                          trees and total number of objects in this file */
      int *tmp_buf = (int *)mymalloc_cat(size, MEM_IO);

      memset(tmp_buf, 0, size);

      /* Write header data (will be flushed on close) */
      nwritten = fwrite(tmp_buf, sizeof(int), Ntrees + 2, save_fd[n]);
      if (nwritten != Ntrees + 2) {
        ERROR_LOG(
            "Failed to write header information to output file %d. Expected %d "
            "elements, wrote %d elements. Will retry after output is complete",
            n, Ntrees + 2, nwritten);
      }

      /* Make sure data is actually written to disk */
      fflush(save_fd[n]);

      myfree(tmp_buf);
    }

    for (i = 0; i < NumProcessedHalos; i++) {
      if (ProcessedHalos[i].SnapNum == MimicConfig.ListOutputSnaps[n]) {
        /* Use stack allocation instead of dynamic allocation */
        struct HaloOutput halo_output = {0}; /* Zero-initialize */

        /* Convert internal halo to output format */
        prepare_halo_for_output(filenr, tree, &ProcessedHalos[i], &halo_output);

        /* Write halo data to output file */
        size_t halo_size = sizeof(struct HaloOutput);
        nwritten = fwrite(&halo_output, halo_size, 1, save_fd[n]);

        if (nwritten != 1) {
          FATAL_ERROR("Failed to write halo data for halo %d (tree %d, "
                      "filenr %d, snapshot %d)",
                      i, tree, filenr, MimicConfig.ListOutputSnaps[n]);
        }

        /* Increment halo counters right after successful write */
        TotHalosPerSnap[n]++;
        InputHalosPerSnap[n][tree]++;
      }
    }
  }

  /* Free the workspace using tracked memory deallocation */
  myfree(OutputGalOrder);
}

/**
 * @brief   Converts internal halo structure to output format
 *
 * @param   filenr    Current file number being processed
 * @param   tree      Current tree number being processed
 * @param   g         Pointer to the internal halo tracking structure
 * @param   o         Pointer to the output halo structure to be filled
 *
 * This function transforms the internal halo representation (halo struct)
 * to the output format (halo_OUTPUT struct). It:
 *
 * 1. Copies basic halo properties (type, position, velocities, masses)
 * 2. Creates a unique halo index that encodes file, tree, and halo number
 * 3. Converts units from internal simulation units to physical units
 *
 * Note: Only halo properties from merger trees are output (no physics).
 */
void prepare_halo_for_output(int filenr, int tree, const struct Halo *g,
                             struct HaloOutput *o) {
  /* CUSTOM: Calculate unique halo index encoding file, tree, and halo number.
   * For large file counts (>=10000), use smaller file multiplier to fit in long
   * long. */
  long long file_mul_fac =
      (MimicConfig.LastFile >= 10000) ? (FILENR_MUL_FAC / 10) : FILENR_MUL_FAC;
  long long tree_mul = TREE_MUL_FAC * tree;
  long long file_mul = file_mul_fac * filenr;
  int central_halo_nr =
      ProcessedHalos[HaloAux[InputTreeHalos[g->HaloNr].FirstHaloInFOFgroup]
                         .FirstHalo]
          .HaloNr;

  /* Verify tree size assumptions */
  assert(g->HaloNr < TREE_MUL_FAC);
  assert(tree < file_mul_fac / TREE_MUL_FAC);

  /* Compute indices */
  o->HaloIndex = g->HaloNr + tree_mul + file_mul;
  o->CentralHaloIndex = central_halo_nr + tree_mul + file_mul;

  /* Verify index encoding/decoding correctness */
  assert((o->HaloIndex - g->HaloNr - tree_mul) / file_mul_fac == filenr);
  assert((o->HaloIndex - g->HaloNr - file_mul) / TREE_MUL_FAC == tree);
  assert(o->HaloIndex - tree_mul - file_mul == g->HaloNr);

  /* CUSTOM: Mimic-specific indices */
  o->MimicHaloIndex = g->HaloNr;
  o->MimicTreeIndex = tree;
  o->SimulationHaloIndex = InputTreeHalos[g->HaloNr].MostBoundID;

/* AUTO-GENERATED: Copy all properties from struct Halo to struct HaloOutput */
#include "../../include/generated/copy_to_output.inc"

  /* CUSTOM: dT unit conversion (internal uses seconds, output uses Myr) */
  /* Don't convert sentinel value (-1.0 indicates no progenitor/invalid) */
  if (g->dT == -1.0) {
    o->dT = -1.0;
  } else {
    o->dT = g->dT * UnitTime_in_s / SEC_PER_MEGAYEAR;
  }
}

/**
 * @brief   Finalizes halo output files by writing header information
 *
 * @param   filenr    Current file number being processed
 *
 * This function completes the halo output files after all objects have
 * been written. For each output snapshot, it:
 *
 * 1. Seeks to the beginning of the file
 * 2. Writes the total number of trees
 * 3. Writes the total number of objects in the file
 * 4. Writes the number of objects for each tree
 * 5. Closes the file
 *
 * This header information is essential for readers to navigate the file
 * structure and access specific trees or objects efficiently.
 */
void finalize_halo_file(int filenr) {
  int n, nwritten;

  for (n = 0; n < MimicConfig.NOUT; n++) {
    // file must already be open.
    assert(save_fd[n]);

    // Finalize the file for this snapshot

    // Force a final flush to ensure all data is written
    fflush(save_fd[n]);

    // Seek to the beginning of the file for header writing
    if (fseek(save_fd[n], 0, SEEK_SET) != 0) {
      FATAL_ERROR("Failed to seek to beginning of file for writing header");
    }

    // Write the total number of trees (first header field)
    nwritten = fwrite(&Ntrees, sizeof(int), 1, save_fd[n]);
    if (nwritten != 1) {
      FATAL_ERROR(
          "Failed to write number of trees to header of file %d (filenr %d)", n,
          filenr);
    }

    // Write the total number of objects (second header field)
    nwritten = fwrite(&TotHalosPerSnap[n], sizeof(int), 1, save_fd[n]);
    if (nwritten != 1) {
      FATAL_ERROR(
          "Failed to write total halo count to header of file %d (filenr %d)",
          n, filenr);
    }

    // Write objects per tree (array of integers)
    nwritten = fwrite(InputHalosPerSnap[n], sizeof(int), Ntrees, save_fd[n]);
    if (nwritten != Ntrees) {
      FATAL_ERROR("Failed to write halo counts per tree to header of file %d "
                  "(filenr %d)",
                  n, filenr);
    }

    // Final data flush
    fflush(save_fd[n]);

    // Close the file and clear handle
    fclose(save_fd[n]);
    save_fd[n] = NULL;
  }
}

#undef TREE_MUL_FAC
#undef FILENR_MUL_FAC
