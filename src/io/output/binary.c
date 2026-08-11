/**
 * @file    output/binary.c
 * @brief   Functions for saving halo data to binary output files
 *
 * Writes tracked halos to per-snapshot, per-filenr binary output files.
 * Each file begins with a placeholder header (Ntrees, TotHalosPerSnap,
 * InputHalosPerSnap[]) that finalize_halo_file() overwrites once all halos
 * are written. Format-agnostic utilities (shared with HDF5) are in util.c.
 */

#include <inttypes.h>
#include <math.h>
#include <limits.h>
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

/* Per-snapshot file handles; kept open across trees to avoid repeated seeking. */
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
 * @brief   Write ProcessedHalos for the current tree to per-snapshot binary files.
 * @param   filenr   Output chunk identifier (maps to one output file per snapshot).
 * @param   tree     Tree index within the current filenr (for counter updates).
 * @param   view     Input view over the raw halos this tree was built from.
 */
void save_halos(int filenr, int tree, struct HaloInputView view) {
  char buf[MAX_BUF_SIZE + 1];
  int64_t i;
  int n;
  int nwritten;

  for (n = 0; n < MimicConfig.NOUT; n++) {
    if (!save_fd[n]) {
      output_path_binary(buf, MAX_BUF_SIZE, filenr, n);

      save_fd[n] = fopen(buf, "wb+");
      if (save_fd[n] == NULL) {
        FATAL_ERROR("Failed to open output halo file '%s' for snapshot %d "
                    "(filenr %d)",
                    buf, MimicConfig.ListOutputSnaps[n], filenr);
      }

      setvbuf(save_fd[n], NULL, _IOFBF, 65536); /* 64 KB buffer: ~2× faster sequential writes */

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

      fflush(save_fd[n]);
      myfree(tmp_buf);
    }

    for (i = 0; i < NumProcessedHalos; i++) {
      if (ProcessedHalos[i].SnapNum == MimicConfig.ListOutputSnaps[n]) {
        struct HaloOutput halo_output = {0};

        prepare_halo_for_output(view, &ProcessedHalos[i], &halo_output);

        size_t halo_size = sizeof(struct HaloOutput);
        nwritten = fwrite(&halo_output, halo_size, 1, save_fd[n]);

        if (nwritten != 1) {
          FATAL_ERROR("Failed to write halo data for halo %" PRId64 " (tree %d, "
                      "filenr %d, snapshot %d)",
                      i, tree, filenr, MimicConfig.ListOutputSnaps[n]);
        }

        output_increment_halo_counters_checked(filenr, n, MimicConfig.ListOutputSnaps[n], tree);
      }
    }
  }
}

/**
 * @brief   Write binary file headers and close output files for this filenr.
 * @param   filenr   File number being finalized.
 *
 * Seeks to the start of each per-snapshot file and overwrites the placeholder
 * header (written by save_halos on first open): Ntrees, TotHalosPerSnap[n],
 * InputHalosPerSnap[n][0..Ntrees-1]. Then flushes and closes every open handle.
 */
void finalize_halo_file(int filenr) {
  int n, nwritten;

  for (n = 0; n < MimicConfig.NOUT; n++) {
    if (save_fd[n] == NULL) {
      FATAL_ERROR("Output file for snapshot index %d (filenr %d) was never opened", n, filenr);
    }

    fflush(save_fd[n]);

    if (fseek(save_fd[n], 0, SEEK_SET) != 0) {
      FATAL_ERROR("Failed to seek to beginning of file for writing header");
    }

    nwritten = fwrite(&Ntrees, sizeof(int), 1, save_fd[n]);
    if (nwritten != 1) {
      FATAL_ERROR("Failed to write number of trees to header of file %d (filenr %d)", n, filenr);
    }

    const int tot_halos_header =
        narrow_int64_to_int_checked(TotHalosPerSnap[n], "binary header TotHalosPerSnap");
    nwritten = fwrite(&tot_halos_header, sizeof(int), 1, save_fd[n]);
    if (nwritten != 1) {
      FATAL_ERROR("Failed to write total halo count to header of file %d (filenr %d)", n, filenr);
    }

    nwritten = fwrite(InputHalosPerSnap[n], sizeof(int), Ntrees, save_fd[n]);
    if (nwritten != Ntrees) {
      FATAL_ERROR("Failed to write halo counts per tree to header of file %d "
                  "(filenr %d)",
                  n, filenr);
    }

    fflush(save_fd[n]);
    fclose(save_fd[n]);
    save_fd[n] = NULL;
  }
}
