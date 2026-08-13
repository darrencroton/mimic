/**
 * @file    output/util.c
 * @brief   Shared utilities for output file writing (binary and HDF5)
 *
 * Path formatters, counter helpers, and halo conversion shared by the
 * binary and HDF5 output writers.
 */

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "globals.h"
#include "proto.h"
#include "output/util.h"
#include "output/binary.h"
#ifdef HDF5
#include "output/hdf5.h"
#endif
#include "module_system/output_helpers.h"
#include "module_system/physical_constants.h" /* For SEC_PER_MEGAYEAR */
#include "tree/reader.h" /* enum InputProcessingOrder only (the ProcessingOrder field), never the active reader pointer */

void output_path_binary(char *buf, size_t size, int filenr, int snap_index) {
  int written =
      snprintf(buf, size, "%s/%s_z%1.3f_%d", MimicConfig.OutputDir, MimicConfig.OutputFileBaseName,
               MimicConfig.ZZ[MimicConfig.ListOutputSnaps[snap_index]], filenr);
  if (written < 0 || (size_t)written >= size) {
    FATAL_ERROR("Binary output path too long (filenr %d, snapshot index %d)", filenr, snap_index);
  }
}

void output_path_hdf5(char *buf, size_t size, int filenr) {
  int written = snprintf(buf, size, "%s/%s_%03d.hdf5", MimicConfig.OutputDir,
                         MimicConfig.OutputFileBaseName, filenr);
  if (written < 0 || (size_t)written >= size) {
    FATAL_ERROR("HDF5 output path too long (filenr %d)", filenr);
  }
}

void prepare_output_files(int filenr, struct OutputSnapshotSelection selection) {
#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    open_hdf5_output_file(filenr, selection);
    return;
  }
#endif
  /* Binary output is tree-ordered-only (rejected at configuration time for
   * snapshot runs), so it always carries every requested snapshot; the
   * selection is not consulted here. */
  (void)selection;
  create_binary_output_files(filenr);
}

void output_increment_halo_counters_checked(int filenr, int snap_index, int snap_num, int tree) {
  const int snapshot_run =
      (enum InputProcessingOrder)MimicConfig.ProcessingOrder == INPUT_PROCESSING_ORDER_SNAPSHOT;

  if (snapshot_run) {
    /* Snapshot-ordered runs never allocate InputHalosPerSnap (only the tree
     * reader does, src/io/tree/interface.c), so only the snapshot total is
     * tracked here; touching InputHalosPerSnap would dereference NULL. */
    if (TotHalosPerSnap[snap_index] == INT_MAX) {
      FATAL_ERROR("Halo counter overflow for output chunk %d at snapshot %d (tree %d)", filenr,
                  snap_num, tree);
    }
    TotHalosPerSnap[snap_index]++;
    return;
  }

  if (TotHalosPerSnap[snap_index] == INT_MAX || InputHalosPerSnap[snap_index][tree] == INT_MAX) {
    FATAL_ERROR("Halo counter overflow for output chunk %d at snapshot %d (tree %d)", filenr,
                snap_num, tree);
  }

  TotHalosPerSnap[snap_index]++;
  InputHalosPerSnap[snap_index][tree]++;
}

/**
 * @brief   Copy struct Halo fields into struct HaloOutput via generated copy_to_output.inc.
 * @param   view  Input view over the raw halos this record was built from.
 * @param   g     Source halo (internal tracking structure).
 * @param   o     Destination halo output record (zeroed by caller).
 */
void prepare_halo_for_output(struct HaloInputView view, const struct Halo *g,
                             struct HaloOutput *o) {
/* AUTO-GENERATED: Copy all properties from struct Halo to struct HaloOutput */
/* Includes automatic unit conversion for dT (seconds → Myr) with sentinel preservation */
#include "../../include/generated/copy_to_output.inc"
}
