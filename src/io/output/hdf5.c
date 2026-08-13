/**
 * @file    output/hdf5.c
 * @brief   Functions for saving halo data to HDF5 output files
 *
 * Owns the HDF5 output lifecycle: property table layout, file/group/dataset
 * creation, per-chunk galaxy writes, run metadata attributes, and master-file
 * linking. Field schema is driven by generated hdf5_field_definitions.inc.
 *
 * Key functions:
 * - calc_hdf5_props(): field table setup from generated metadata
 * - prep_hdf5_file(): per-filenr file/group/table creation
 * - save_halos_hdf5() / flush_hdf5_buffers(): cross-tree write buffer
 * - write_hdf5_attrs(): per-snapshot count attributes and per-tree dataset
 * - write_master_file(): run-level master file with external links
 */

#include <hdf5.h>
#include <hdf5_hl.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"
#include "output/hdf5.h"
#include "output/util.h"
#include "error.h"
#include "module_system/output_helpers.h"
#include "module_registry.h" /* For PhaseModuleConfig */
#include "tree/reader.h"     /* enum InputProcessingOrder (MimicConfig.ProcessingOrder) */

#include "hdf5_internal.h"

/**
 * @brief   Set up the HDF5 table structure (field names, types, offsets) from generated metadata.
 *
 * Driven by hdf5_field_count.inc (sets HDF5_n_props) and hdf5_field_definitions.inc
 * (populates HDF5_dst_offsets/sizes/field_names/field_types). Called once per run
 * before any file is opened.
 */
void calc_hdf5_props(void) {
  struct HaloOutput galout;

  HDF5_dst_size = sizeof(struct HaloOutput);

  /* Shared by generated vector fields; closed during HDF5 output cleanup. */
  hid_t array3f_tid = H5Tarray_create(H5T_NATIVE_FLOAT, 1, (hsize_t[]){3});

/* AUTO-GENERATED: Set property count and allocate arrays */
#include "../../include/generated/hdf5_field_count.inc"

  HDF5_dst_offsets = mymalloc_cat(sizeof(size_t) * HDF5_n_props, MEM_IO);
  HDF5_dst_sizes = mymalloc_cat(sizeof(size_t) * HDF5_n_props, MEM_IO);
  HDF5_field_names = mymalloc_cat(sizeof(const char *) * HDF5_n_props, MEM_IO);
  HDF5_field_types = mymalloc_cat(sizeof(hid_t) * HDF5_n_props, MEM_IO);

#include "../../include/generated/hdf5_field_definitions.inc"

  if (i != HDF5_n_props) {
    FATAL_ERROR("HDF5 property count mismatch. Expected %d properties but "
                "processed %d properties",
                HDF5_n_props, i);
  }
}

/**
 * @brief   Create a new HDF5 output file with one table per selected snapshot.
 * @param   fname       Path to the output file.
 * @param   selection   Requested output snapshots this file carries.
 *
 * Creates the file, one Snap{NNN} group per selection entry, and an empty
 * HDF5 table ("Galaxies") in each group. chunk_size=1000 (≈140 KB per chunk for
 * HaloOutput) is a tuning constant; compression is controlled at runtime via
 * MimicConfig.HDF5CompressionLevel (0 = off).
 */
void prep_hdf5_file(char *fname, struct OutputSnapshotSelection selection) {
  /* 1000 records ≈ 140 KB per chunk: sweet spot for sequential write throughput vs. overhead. */
  hsize_t chunk_size = 1000;
  int *fill_data = NULL;
  hid_t file_id, snap_group_id;
  char target_group[100];
  hid_t status;
  int i_snap;

  DEBUG_LOG("Creating new HDF5 file '%s'", fname);
  file_id = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (file_id < 0) {
    FATAL_ERROR("Failed to create HDF5 file '%s'", fname);
  }

  for (int idx = 0; idx < selection.count; idx++) {
    i_snap = selection.indices[idx];
    sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[i_snap]);
    snap_group_id = H5Gcreate(file_id, target_group, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (snap_group_id < 0) {
      FATAL_ERROR("Failed to create group '%s' in HDF5 file '%s'", target_group, fname);
    }

    // Make the table. Compression (gzip) is off by default and enabled at
    // runtime via --compress; it trades CPU for disk and changes only the
    // on-disk byte layout, not the stored values.
    status = H5TBmake_table("halo Table", snap_group_id, "Galaxies", HDF5_n_props, 0, HDF5_dst_size,
                            HDF5_field_names, HDF5_dst_offsets, HDF5_field_types, chunk_size,
                            fill_data, MimicConfig.HDF5CompressionLevel, NULL);
    if (status < 0) {
      FATAL_ERROR("Failed to create HDF5 table for snapshot %d in file '%s'",
                  MimicConfig.ListOutputSnaps[i_snap], fname);
    }

    H5Gclose(snap_group_id);
  }

  status = H5Fclose(file_id);
  if (status < 0) {
    FATAL_ERROR("Failed to close HDF5 file '%s'", fname);
  }
}

/**
 * @brief   Create this filenr's HDF5 output file and leave it open for writing
 *
 * Creates the per-filenr file with one table per selected snapshot
 * (prep_hdf5_file), then reopens it, stores the handle in
 * HDF5_current_file_id so subsequent batch writes are cheap, and writes the
 * per-file RunProperties metadata immediately: this is the only site that
 * assigns HDF5_current_file_id, so "per-file metadata" is a property of
 * opening a file rather than of a snapshot index. The handle is closed by the
 * driver after the filenr is finalized.
 */
void open_hdf5_output_file(int filenr, struct OutputSnapshotSelection selection) {
  char buf[3 * MAX_STRING_LEN + 40];

  output_path_hdf5(buf, sizeof(buf), filenr);
  prep_hdf5_file(buf, selection);

  HDF5_current_file_id = H5Fopen(buf, H5F_ACC_RDWR, H5P_DEFAULT);
  if (HDF5_current_file_id < 0) {
    FATAL_ERROR("Failed to open HDF5 file '%s' for writing", buf);
  }
  DEBUG_LOG("HDF5 file '%s' opened with ID %lld", buf, (long long)HDF5_current_file_id);

  write_perfile_metadata(HDF5_current_file_id);
}

/**
 * @brief   Append a batch of halos to the open HDF5 file.
 * @param   halo_batch   Prepared HaloOutput records to append.
 * @param   num_halos    Number of records in the batch.
 * @param   n            Snapshot index into ListOutputSnaps.
 * @param   filenr       File number (for error messages).
 */
void write_hdf5_halo_batch(struct HaloOutput *halo_batch, int num_halos, int n, int filenr) {

  herr_t status;
  hid_t group_id;
  char target_group[100];

  if (HDF5_current_file_id < 0) {
    FATAL_ERROR("HDF5 file not open for writing (file_id = %lld)", (long long)HDF5_current_file_id);
  }

  if (num_halos <= 0)
    return;

  sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[n]);
  group_id = H5Gopen(HDF5_current_file_id, target_group, H5P_DEFAULT);
  if (group_id < 0) {
    FATAL_ERROR("Failed to open HDF5 group '%s' for snapshot %d (filenr %d)", target_group,
                MimicConfig.ListOutputSnaps[n], filenr);
  }

  status = H5TBappend_records(group_id, "Galaxies", num_halos, HDF5_dst_size, HDF5_dst_offsets,
                              HDF5_dst_sizes, halo_batch);
  if (status < 0) {
    FATAL_ERROR("Failed to append %d halo records to HDF5 file for snapshot %d "
                "(filenr %d)",
                num_halos, MimicConfig.ListOutputSnaps[n], filenr);
  }

  H5Gclose(group_id); /* file handle stays open; closed by the driver after filenr is finalized */
}

/**
 * @brief   Write per-snapshot count attributes and the per-tree halo count dataset.
 * @param   n        Snapshot index into ListOutputSnaps.
 * @param   filenr   File number (for error messages).
 *
 * Writes TotHalosPerSnap[n] as a scalar attribute on Snap{NNN}/Galaxies.
 * Tree-ordered runs additionally write Ntrees and the
 * TreeHalosPerSnap[0..Ntrees-1] dataset; snapshot-ordered runs have no tree
 * structure, so both are omitted entirely (absent, not zero/empty) rather
 * than reading the tree-only InputHalosPerSnap. Per-file RunProperties
 * metadata is written once, at file open (open_hdf5_output_file()), not here.
 */
void write_hdf5_attrs(int n, int filenr) {

  herr_t status;
  hid_t dataset_id, attribute_id, dataspace_id, group_id;
  hsize_t dims;
  char target_group[100];
  const int snapshot_run =
      (enum InputProcessingOrder)MimicConfig.ProcessingOrder == INPUT_PROCESSING_ORDER_SNAPSHOT;

  if (HDF5_current_file_id < 0) {
    FATAL_ERROR("HDF5 file not open for writing attributes (file_id = %lld)",
                (long long)HDF5_current_file_id);
  }

  sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[n]);
  group_id = H5Gopen(HDF5_current_file_id, target_group, H5P_DEFAULT);
  if (group_id < 0) {
    FATAL_ERROR("Failed to open HDF5 group '%s' for writing attributes (filenr %d)", target_group,
                filenr);
  }
  dataset_id = H5Dopen(group_id, "Galaxies", H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to open Galaxies dataset for writing attributes (filenr %d)", filenr);
  }

  dims = 1;
  dataspace_id = H5Screate_simple(1, &dims, NULL);
  if (dataspace_id < 0) {
    FATAL_ERROR("Failed to create dataspace for attributes in HDF5 file (filenr %d)", filenr);
  }

  if (!snapshot_run) {
    attribute_id =
        H5Acreate(dataset_id, "Ntrees", H5T_NATIVE_INT, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
    if (attribute_id < 0) {
      FATAL_ERROR("Failed to create Ntrees attribute in HDF5 file (filenr %d)", filenr);
    }
    status = H5Awrite(attribute_id, H5T_NATIVE_INT, &Ntrees);
    if (status < 0) {
      FATAL_ERROR("Failed to write Ntrees attribute to HDF5 file (filenr %d)", filenr);
    }
    H5Aclose(attribute_id);
  }

  attribute_id = H5Acreate(dataset_id, "TotHalosPerSnap", H5T_NATIVE_INT64, dataspace_id,
                           H5P_DEFAULT, H5P_DEFAULT);
  if (attribute_id < 0) {
    FATAL_ERROR("Failed to create TotHalosPerSnap attribute in HDF5 file (filenr %d)", filenr);
  }
  status = H5Awrite(attribute_id, H5T_NATIVE_INT64, &TotHalosPerSnap[n]);
  if (status < 0) {
    FATAL_ERROR("Failed to write TotHalosPerSnap attribute to HDF5 file (filenr %d)", filenr);
  }
  H5Aclose(attribute_id);

  H5Sclose(dataspace_id);
  H5Dclose(dataset_id);

  /* FieldMetadata is identical for every snapshot, so it is written once per
   * file under RunProperties (see write_perfile_metadata) rather than being
   * duplicated in each snapshot group. */

  if (!snapshot_run) {
    // Create an array dataset to hold the number of objects per tree and write
    // it.
    dims = Ntrees;
    if (dims <= 0) {
      FATAL_ERROR("Invalid number of trees (Ntrees=%d) in write_hdf5_attrs for "
                  "snapshot %d (filenr %d)",
                  (int)dims, MimicConfig.ListOutputSnaps[n], filenr);
    }
    dataspace_id = H5Screate_simple(1, &dims, NULL);
    if (dataspace_id < 0) {
      FATAL_ERROR("Failed to create dataspace for TreeHalosPerSnap dataset for snapshot %d "
                  "(filenr %d)",
                  MimicConfig.ListOutputSnaps[n], filenr);
    }
    dataset_id = H5Dcreate(group_id, "TreeHalosPerSnap", H5T_NATIVE_INT, dataspace_id, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT);
    if (dataset_id < 0) {
      FATAL_ERROR("Failed to create TreeHalosPerSnap dataset for snapshot %d (filenr %d)",
                  MimicConfig.ListOutputSnaps[n], filenr);
    }

    write_description_attr(dataset_id, "Number of halos per merger tree at this snapshot");

    status =
        H5Dwrite(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, InputHalosPerSnap[n]);
    if (status < 0) {
      FATAL_ERROR("Failed to write TreeHalosPerSnap dataset for snapshot %d "
                  "(filenr %d, status=%d)",
                  MimicConfig.ListOutputSnaps[n], filenr, (int)status);
    }

    H5Sclose(dataspace_id);
    H5Dclose(dataset_id);
  }

  H5Gclose(group_id); /* file handle stays open; closed by the driver after filenr is finalized */
}

/**
 * @brief   Buffer ProcessedHalos into the cross-tree write buffers for the snapshots in the
 *          supplied selection.
 * @param   filenr      File number (for output counters).
 * @param   tree        Tree index within the partition (for output counters).
 * @param   view        Input view over the raw halos this record was built from.
 * @param   selection   Requested output snapshots this partition carries.
 *
 * Halos accumulate in hdf5_wbuf[n] and are flushed to write_hdf5_halo_batch() only
 * when a buffer fills or at end-of-file (flush_hdf5_buffers). See the cross-tree
 * buffer comment below for the performance rationale.
 */
/*
 * Cross-tree write buffer.
 *
 * One fixed-size buffer of prepared HaloOutput records per output snapshot.
 * Records accumulate across trees and flush to HDF5 only when a buffer fills
 * or at end of file (flush_hdf5_buffers). This decouples HDF5 write
 * granularity from tree boundaries (the previous one-append-per-tree pattern)
 * and from total file size, so memory stays bounded regardless of simulation
 * scale. Appends drop from O(Ntrees * NOUT) to O(total_records / BUFFER_RECORDS)
 * per snapshot, which is what makes HDF5 output as cheap as binary.
 */
#define HDF5_WRITE_BUFFER_RECORDS 8192
static struct HaloOutput *hdf5_wbuf[ABSOLUTEMAXSNAPS] = {0};
static int hdf5_wbuf_count[ABSOLUTEMAXSNAPS] = {0};

static void flush_hdf5_buffer(int n, int filenr) {
  if (hdf5_wbuf_count[n] > 0) {
    write_hdf5_halo_batch(hdf5_wbuf[n], hdf5_wbuf_count[n], n, filenr);
    hdf5_wbuf_count[n] = 0;
  }
}

void flush_hdf5_buffers(int filenr, struct OutputSnapshotSelection selection) {
  for (int idx = 0; idx < selection.count; idx++) {
    int n = selection.indices[idx];
    flush_hdf5_buffer(n, filenr);
    if (hdf5_wbuf[n] != NULL) {
      myfree(hdf5_wbuf[n]);
      hdf5_wbuf[n] = NULL;
    }
  }
}

void save_halos_hdf5(int filenr, int tree, struct HaloInputView view,
                     struct OutputSnapshotSelection selection) {
  int64_t i;
  int n;

  for (int idx = 0; idx < selection.count; idx++) {
    n = selection.indices[idx];
    for (i = 0; i < NumProcessedHalos; i++) {
      if (ProcessedHalos[i].SnapNum != MimicConfig.ListOutputSnaps[n])
        continue;

      if (hdf5_wbuf[n] == NULL) {
        hdf5_wbuf[n] = (struct HaloOutput *)mymalloc_cat(
            HDF5_WRITE_BUFFER_RECORDS * sizeof(struct HaloOutput), MEM_IO);
      }

      prepare_halo_for_output(view, &ProcessedHalos[i], &hdf5_wbuf[n][hdf5_wbuf_count[n]]);
      hdf5_wbuf_count[n]++;

      output_increment_halo_counters_checked(filenr, n, MimicConfig.ListOutputSnaps[n], tree);

      if (hdf5_wbuf_count[n] == HDF5_WRITE_BUFFER_RECORDS) {
        flush_hdf5_buffer(n, filenr);
      }
    }
  }
}

void free_hdf5_ids(void) {
  myfree(HDF5_field_types);
  myfree(HDF5_field_names);
  myfree(HDF5_dst_sizes);
  myfree(HDF5_dst_offsets);
}
