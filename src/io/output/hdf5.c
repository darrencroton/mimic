/**
 * @file    output/hdf5.c
 * @brief   Functions for saving halo data to HDF5 output files
 *
 * This file implements functionality for writing halo data to HDF5 format
 * output files. It handles the creation of HDF5 file structures, the definition
 * of halo property tables, and the writing of halo data and metadata.
 *
 * The HDF5 format provides several advantages over plain binary files:
 * - Self-describing data with attributes and metadata
 * - Better portability across different systems
 * - Built-in compression and chunking for efficient storage and access
 * - Support for direct access to specific data elements
 *
 * Key functions:
 * - calc_hdf5_props(): Defines the HDF5 table structure for halo properties
 * - prep_hdf5_file(): Creates and initializes an HDF5 output file
 * - write_hdf5_halo(): Writes a single halo to an HDF5 file
 * - write_hdf5_attrs(): Writes metadata attributes to an HDF5 file
 * - write_master_file(): Creates a master file with links to all output files
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

#include "hdf5_internal.h"

/**
 * @brief   Defines the HDF5 table structure for halo properties
 *
 * This function sets up the HDF5 table structure for storing halo properties
 * in the output files. It:
 * 1. Defines the total number of halo properties to be saved
 * 2. Allocates memory for property metadata arrays
 * 3. Calculates memory offsets for each property in the halo_OUTPUT struct
 * 4. Defines field names and data types for each property
 *
 * The function handles all halo properties, including scalars (masses, rates)
 * and arrays (positions, velocities, spins). It configures the HDF5 table
 * to match the layout of the halo_OUTPUT struct for efficient I/O.
 */
void calc_hdf5_props(void) {

  /*
   * Prepare an HDF5 to receive the output halo data.
   * Here we store the data in an hdf5 table for easily appending new data.
   */

  struct HaloOutput galout;

  /* Size of a single halo entry */
  HDF5_dst_size = sizeof(struct HaloOutput);

  /* Create datatypes for different size arrays */
  hid_t array3f_tid = H5Tarray_create(H5T_NATIVE_FLOAT, 1, (hsize_t[]){3});

/* AUTO-GENERATED: Set property count and allocate arrays */
#include "../../include/generated/hdf5_field_count.inc"

  /* Allocate arrays for field metadata */
  HDF5_dst_offsets = mymalloc_cat(sizeof(size_t) * HDF5_n_props, MEM_IO);
  HDF5_dst_sizes = mymalloc_cat(sizeof(size_t) * HDF5_n_props, MEM_IO);
  HDF5_field_names = mymalloc_cat(sizeof(const char *) * HDF5_n_props, MEM_IO);
  HDF5_field_types = mymalloc_cat(sizeof(hid_t) * HDF5_n_props, MEM_IO);

/* AUTO-GENERATED: Define all HDF5 fields from metadata
 * This replaces ~150 lines of manual field definitions */
#include "../../include/generated/hdf5_field_definitions.inc"

  /* Validate property count */
  if (i != HDF5_n_props) {
    FATAL_ERROR("HDF5 property count mismatch. Expected %d properties but "
                "processed %d properties",
                HDF5_n_props, i);
  }
}

/**
 * @brief   Creates and initializes an HDF5 output file
 *
 * @param   fname   Path to the output file
 *
 * This function creates and initializes a new HDF5 file for halo output.
 * It:
 * 1. Creates the file with default HDF5 properties
 * 2. Creates a group for each output snapshot
 * 3. Creates a table within each group to store halo data
 * 4. Configures table properties like chunking for optimal performance
 *
 * The created file structure allows easy organization of objects by snapshot,
 * and efficient appending of new halo records as they are processed.
 */
void prep_hdf5_file(char *fname) {

  /*
   * HDF5 chunk size for table storage (number of records per chunk).
   *
   * This is the single most important parameter for HDF5 I/O performance
   * tuning. Current value: 1000 records ≈ 140 KB per chunk (for HaloOutput
   * struct).
   *
   * Performance considerations:
   * - Too small (<100): Excessive metadata overhead, poor sequential I/O
   * - Too large (>10000): Wasted memory for partial chunk reads/writes
   * - Recommended range: 10 KB - 1 MB per chunk
   * - System-dependent: Optimal value varies with filesystem (NFS, Lustre,
   * local)
   *
   * For advanced HPC tuning, this could be made configurable via parameter
   * file. Current default (1000) provides good performance for typical use
   * cases.
   */
  hsize_t chunk_size = 1000;
  int *fill_data = NULL;
  hid_t file_id, snap_group_id;
  char target_group[100];
  hid_t status;
  int i_snap;

  DEBUG_LOG("Creating new HDF5 file '%s'", fname);
  file_id = H5Fcreate(fname, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);

  // Create a group for each output snapshot
  for (i_snap = 0; i_snap < MimicConfig.NOUT; i_snap++) {
    sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[i_snap]);
    snap_group_id = H5Gcreate(file_id, target_group, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

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

  // Close the HDF5 file.
  H5Fclose(file_id);
}

/**
 * @brief   Create this filenr's HDF5 output file and leave it open for writing
 *
 * Creates the per-filenr file with one table per requested snapshot
 * (prep_hdf5_file), then reopens it and stores the handle in
 * HDF5_current_file_id so subsequent batch writes are cheap. The handle is
 * closed by the driver after the filenr is finalized.
 */
void open_hdf5_output_file(int filenr) {
  char buf[3 * MAX_STRING_LEN + 40];

  output_path_hdf5(buf, sizeof(buf), filenr);
  prep_hdf5_file(buf);

  HDF5_current_file_id = H5Fopen(buf, H5F_ACC_RDWR, H5P_DEFAULT);
  if (HDF5_current_file_id < 0) {
    FATAL_ERROR("Failed to open HDF5 file '%s' for writing", buf);
  }
  DEBUG_LOG("HDF5 file '%s' opened with ID %lld", buf, (long long)HDF5_current_file_id);
}

/**
 * @brief   Writes a single halo to an HDF5 file
 *
 * @param   halo_output   Pointer to halo data to write
 * @param   n               Snapshot index in ListOutputSnaps
 * @param   filenr          File number to write to
 *
 * This function writes a single halo to the appropriate HDF5 file.
 * It:
 * 1. Opens the target HDF5 file
 * 2. Navigates to the correct snapshot group
 * 3. Appends the halo record to the halo table
 * 4. Properly closes all HDF5 objects
 *
 * The function is designed to be called for each individual halo
 * as it is processed, enabling incremental output without requiring
 * all objects to be held in memory.
 */
/**
 * @brief   Writes a batch of halos to an HDF5 file (OPTIMIZED)
 *
 * @param   halo_batch   Array of halos to write
 * @param   num_halos    Number of halos in the batch
 * @param   n            Snapshot index in ListOutputSnaps
 * @param   filenr       File number
 *
 * This function writes multiple halos to the HDF5 file in a single operation.
 * This is MUCH faster than writing halos one at a time because it:
 * - Opens the group once
 * - Writes all records in one HDF5 operation
 * - Closes the group once
 *
 * This reduces HDF5 overhead from O(N) to O(1) per batch.
 */
void write_hdf5_halo_batch(struct HaloOutput *halo_batch, int num_halos, int n, int filenr) {

  herr_t status;
  hid_t group_id;
  char target_group[100];

  // Verify file is open
  if (HDF5_current_file_id < 0) {
    FATAL_ERROR("HDF5 file not open for writing (file_id = %lld)", (long long)HDF5_current_file_id);
  }

  if (num_halos <= 0)
    return; /* Nothing to write */

  // Open the relevant group
  sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[n]);
  group_id = H5Gopen(HDF5_current_file_id, target_group, H5P_DEFAULT);
  if (group_id < 0) {
    FATAL_ERROR("Failed to open HDF5 group '%s' for snapshot %d (filenr %d)", target_group,
                MimicConfig.ListOutputSnaps[n], filenr);
  }

  // Write entire batch at once
  status = H5TBappend_records(group_id, "Galaxies", num_halos, HDF5_dst_size, HDF5_dst_offsets,
                              HDF5_dst_sizes, halo_batch);
  if (status < 0) {
    FATAL_ERROR("Failed to append %d halo records to HDF5 file for snapshot %d "
                "(filenr %d)",
                num_halos, MimicConfig.ListOutputSnaps[n], filenr);
  }

  // Close only the group (file stays open)
  H5Gclose(group_id);
}

/**
 * @brief   Writes metadata attributes to an HDF5 file
 *
 * @param   n          Snapshot index in ListOutputSnaps
 * @param   filenr     File number to write to
 *
 * This function writes metadata attributes to an HDF5 file after all
 * have been written. It:
 * 1. Opens the target HDF5 file
 * 2. Navigates to the correct snapshot group
 * 3. Adds attributes such as number of trees and number of objects
 * 4. Creates and writes the InputHalosPerSnap dataset (objects per tree)
 *
 * These attributes are essential for readers to understand the file structure
 * and for tools to navigate and process the halo data efficiently.
 */
void write_hdf5_attrs(int n, int filenr) {

  /*
   * Write the HDF5 file attributes.
   * Uses the already-open HDF5_current_file_id for performance.
   */

  herr_t status;
  hid_t dataset_id, attribute_id, dataspace_id, group_id;
  hsize_t dims;
  char target_group[100];

  // Verify file is open
  if (HDF5_current_file_id < 0) {
    FATAL_ERROR("HDF5 file not open for writing attributes (file_id = %lld)",
                (long long)HDF5_current_file_id);
  }

  /* Write RunProperties metadata to per-file output for self-containment
   * (only written once per file, on first snapshot) */
  if (n == 0) {
    write_perfile_metadata(HDF5_current_file_id);
  }

  // Open the relevant group
  sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[n]);
  group_id = H5Gopen(HDF5_current_file_id, target_group, H5P_DEFAULT);

  dataset_id = H5Dopen(group_id, "Galaxies", H5P_DEFAULT);

  // Create the data space for the attributes.
  dims = 1;
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  // Write the number of trees
  attribute_id =
      H5Acreate(dataset_id, "Ntrees", H5T_NATIVE_INT, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  status = H5Awrite(attribute_id, H5T_NATIVE_INT, &Ntrees);
  if (status < 0) {
    FATAL_ERROR("Failed to write Ntrees attribute to HDF5 file (filenr %d)", filenr);
  }
  H5Aclose(attribute_id);

  // Write the total number of objects.
  attribute_id = H5Acreate(dataset_id, "TotHalosPerSnap", H5T_NATIVE_INT, dataspace_id, H5P_DEFAULT,
                           H5P_DEFAULT);
  status = H5Awrite(attribute_id, H5T_NATIVE_INT, &TotHalosPerSnap[n]);
  if (status < 0) {
    FATAL_ERROR("Failed to write TotHalosPerSnap attribute to HDF5 file (filenr %d)", filenr);
  }
  H5Aclose(attribute_id);

  // Close the scalar dataspace (reused below)
  H5Sclose(dataspace_id);

  // Close the dataset
  H5Dclose(dataset_id);

  /* FieldMetadata is identical for every snapshot, so it is written once per
   * file under RunProperties (see write_perfile_metadata) rather than being
   * duplicated in each snapshot group. */

  // Create an array dataset to hold the number of objects per tree and write
  // it.
  dims = Ntrees;
  if (dims <= 0) {
    FATAL_ERROR("Invalid number of trees (Ntrees=%d) in write_hdf5_attrs for "
                "snapshot %d (filenr %d)",
                (int)dims, MimicConfig.ListOutputSnaps[n], filenr);
  }
  dataspace_id = H5Screate_simple(1, &dims, NULL);
  dataset_id = H5Dcreate(group_id, "TreeHalosPerSnap", H5T_NATIVE_INT, dataspace_id, H5P_DEFAULT,
                         H5P_DEFAULT, H5P_DEFAULT);

  write_description_attr(dataset_id, "Number of halos per merger tree at this snapshot");

  // Write the halos per tree data
  status =
      H5Dwrite(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, InputHalosPerSnap[n]);
  if (status < 0) {
    FATAL_ERROR("Failed to write TreeHalosPerSnap dataset for snapshot %d "
                "(filenr %d, status=%d)",
                MimicConfig.ListOutputSnaps[n], filenr, (int)status);
  }

  H5Sclose(dataspace_id);
  H5Dclose(dataset_id);

  // Close only the group (file stays open, closed in main.c)
  H5Gclose(group_id);
}

/**
 * @brief   Saves output files for all requested snapshots using HDF5 format
 *
 * @param   filenr    Current file number being processed
 * @param   tree      Current tree number being processed
 *
 * This function writes all halos for the current tree to their respective
 * HDF5 output files. It mirrors the functionality of save_halos() in the
 * binary output system but uses HDF5 format. For each output snapshot, it:
 *
 * 1. Counts halos per requested snapshot
 * 2. Converts internal halo structures to output format
 * 3. Writes halos to HDF5 files using write_hdf5_halo_batch()
 * 4. Updates halo counts for the file and tree
 *
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

void flush_hdf5_buffers(int filenr) {
  for (int n = 0; n < MimicConfig.NOUT; n++) {
    flush_hdf5_buffer(n, filenr);
    if (hdf5_wbuf[n] != NULL) {
      myfree(hdf5_wbuf[n]);
      hdf5_wbuf[n] = NULL;
    }
  }
}

void save_halos_hdf5(int filenr, int tree) {
  int i, n;

  for (n = 0; n < MimicConfig.NOUT; n++) {
    for (i = 0; i < NumProcessedHalos; i++) {
      if (ProcessedHalos[i].SnapNum != MimicConfig.ListOutputSnaps[n])
        continue;

      if (hdf5_wbuf[n] == NULL) {
        hdf5_wbuf[n] = (struct HaloOutput *)mymalloc_cat(
            HDF5_WRITE_BUFFER_RECORDS * sizeof(struct HaloOutput), MEM_IO);
      }

      prepare_halo_for_output(&ProcessedHalos[i], &hdf5_wbuf[n][hdf5_wbuf_count[n]]);
      hdf5_wbuf_count[n]++;

      if (TotHalosPerSnap[n] == INT_MAX || InputHalosPerSnap[n][tree] == INT_MAX) {
        FATAL_ERROR("Halo counter overflow for output chunk %d at snapshot %d (tree %d)", filenr,
                    MimicConfig.ListOutputSnaps[n], tree);
      }

      /* Increment halo counters */
      TotHalosPerSnap[n]++;
      InputHalosPerSnap[n][tree]++;

      if (hdf5_wbuf_count[n] == HDF5_WRITE_BUFFER_RECORDS) {
        flush_hdf5_buffer(n, filenr);
      }
    }
  }
}

void free_hdf5_ids(void) {

  /*
   * Free any HDF5 objects which are still floating about at the end of the run.
   */
  myfree(HDF5_field_types);
  myfree(HDF5_field_names);
  myfree(HDF5_dst_sizes);
  myfree(HDF5_dst_offsets);
}
