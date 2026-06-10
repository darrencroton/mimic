/**
 * @file    output/binary.c
 * @brief   Functions for saving halo data to binary output files
 *
 * This file contains the functionality for writing tracked halos to binary
 * output files. It manages file I/O operations for writing halo data across
 * requested snapshots.
 *
 * Key functions:
 * - save_halos(): Writes halos to binary output files for all requested
 * snapshots
 * - finalize_halo_file(): Completes file writing by updating headers
 *
 * The output files include headers with tree counts and halo counts per tree,
 * followed by the halo data for the corresponding snapshot.
 *
 * Format-agnostic utilities (shared with HDF5) are in io/output/util.c.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "globals.h"
#include "types.h"
#include "error.h"
#include "output/binary.h"
#include "output/util.h"
#include "proto.h"
#include "module_system/output_helpers.h"
#include "module_system/physical_constants.h" /* For SEC_PER_MEGAYEAR */

// keep a static file handle to remove the need to do constant seeking.
FILE *save_fd[ABSOLUTEMAXSNAPS] = {0};

#ifndef MAX_BUF_SIZE
#define MAX_BUF_SIZE (3 * MAX_STRING_LEN + 40)
#endif
#define MAX_OUTFILE_SIZE (MAX_STRING_LEN + 40)

/**
 * @brief   Create one empty output file per requested snapshot for this filenr
 *
 * Called once per filenr before tree processing so output files exist (and
 * mark the filenr as claimed) even before the first halo is written.
 */
void create_binary_output_files(int filenr) {
  char buf[MAX_BUF_SIZE + 1];
  FILE *fd;

  for (int n = 0; n < MimicConfig.NOUT; n++) {
    output_path_binary(buf, MAX_BUF_SIZE, filenr, n);

    if (!(fd = fopen(buf, "w"))) {
      FATAL_ERROR("Failed to create output halo file '%s' for snapshot %d (filenr %d)", buf,
                  MimicConfig.ListOutputSnaps[n], filenr);
    }
    fclose(fd);
  }
}

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
 */
void save_halos(int filenr, int tree) {
  char buf[MAX_BUF_SIZE + 1];
  int i, n;
  /* No need for static output structure anymore since we use dynamic allocation
   */

  int nwritten;

  // now prepare and write
  for (n = 0; n < MimicConfig.NOUT; n++) {
    // only open the file if it is not already open.
    if (!save_fd[n]) {
      output_path_binary(buf, MAX_BUF_SIZE, filenr, n);

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
          (Ntrees + 2) * sizeof(int); /* Extra two integers are for saving the total number of
                                         trees and total number of objects in this file */
      int *tmp_buf = (int *)mymalloc_cat(size, MEM_IO);

      memset(tmp_buf, 0, size);

      /* Write a zeroed placeholder header; finalize_halo_file() rewrites it
       * with the real counts once all halos are written */
      nwritten = fwrite(tmp_buf, sizeof(int), Ntrees + 2, save_fd[n]);
      if (nwritten != Ntrees + 2) {
        FATAL_ERROR("Failed to write placeholder header to output file %d (expected %d "
                    "elements, wrote %d)",
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
        prepare_halo_for_output(&ProcessedHalos[i], &halo_output);

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
    if (save_fd[n] == NULL) {
      FATAL_ERROR("Output file for snapshot index %d (filenr %d) was never opened", n, filenr);
    }

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
      FATAL_ERROR("Failed to write number of trees to header of file %d (filenr %d)", n, filenr);
    }

    // Write the total number of objects (second header field)
    nwritten = fwrite(&TotHalosPerSnap[n], sizeof(int), 1, save_fd[n]);
    if (nwritten != 1) {
      FATAL_ERROR("Failed to write total halo count to header of file %d (filenr %d)", n, filenr);
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
