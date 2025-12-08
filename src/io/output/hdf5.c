/**
 * @file    io/io_save_hdf5.c
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
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "proto.h"
#include "output/hdf5.h"
#include "output/util.h"
#include "error.h"
#include "_system/output_helpers.h"  /* -Isrc/modules makes this work */

#define TRUE 1
#define FALSE 0

/* Forward declarations for helper functions */
static void write_version_metadata(hid_t parent_group_id);
static void write_enabled_modules(hid_t parent_group_id);
static void write_parameters_metadata(hid_t parent_group_id);
static void write_redshifts(hid_t parent_group_id);
static void write_perfile_metadata(hid_t file_id);

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
    snap_group_id =
        H5Gcreate(file_id, target_group, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

    // Make the table
    status =
        H5TBmake_table("halo Table", snap_group_id, "Galaxies", HDF5_n_props, 0,
                       HDF5_dst_size, HDF5_field_names, HDF5_dst_offsets,
                       HDF5_field_types, chunk_size, fill_data, 0, NULL);
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
void write_hdf5_halo_batch(struct HaloOutput *halo_batch, int num_halos, int n,
                           int filenr) {

  herr_t status;
  hid_t group_id;
  char target_group[100];

  // Verify file is open
  if (HDF5_current_file_id < 0) {
    FATAL_ERROR("HDF5 file not open for writing (file_id = %lld)",
                (long long)HDF5_current_file_id);
  }

  if (num_halos <= 0)
    return; /* Nothing to write */

  // Open the relevant group
  sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[n]);
  group_id = H5Gopen(HDF5_current_file_id, target_group, H5P_DEFAULT);
  if (group_id < 0) {
    FATAL_ERROR("Failed to open HDF5 group '%s' for snapshot %d (filenr %d)",
                target_group, MimicConfig.ListOutputSnaps[n], filenr);
  }

  // Write entire batch at once
  status = H5TBappend_records(group_id, "Galaxies", num_halos, HDF5_dst_size,
                              HDF5_dst_offsets, HDF5_dst_sizes, halo_batch);
  if (status < 0) {
    FATAL_ERROR("Failed to append %d halo records to HDF5 file for snapshot %d "
                "(filenr %d)",
                num_halos, MimicConfig.ListOutputSnaps[n], filenr);
  }

  // Close only the group (file stays open)
  H5Gclose(group_id);
}

#ifdef MINIMIZE_IO
void write_hdf5_galsnap_data(int n, int filenr) {

  /*
   * Write a batch of objects to the output HDF5 table.
   */

  herr_t status;
  hid_t file_id, group_id;
  char target_group[100];
  char fname[2 * MAX_STRING_LEN + 50];  // Sufficient for dir + "/" + basename + "_NNN.hdf5"

  // Generate the filename to be opened.
  int ret = snprintf(fname, sizeof(fname), "%s/%s_%03d.hdf5",
                     MimicConfig.OutputDir, MimicConfig.OutputFileBaseName, filenr);
  if (ret < 0) {
    FATAL_ERROR("Path formatting error for: %s/%s_%03d.hdf5",
                MimicConfig.OutputDir, MimicConfig.OutputFileBaseName, filenr);
  }
  if (ret >= (int)sizeof(fname)) {
    FATAL_ERROR("Output path too long: %s/%s_%03d.hdf5",
                MimicConfig.OutputDir, MimicConfig.OutputFileBaseName, filenr);
  }

  // Open the file.
  file_id = H5Fopen(fname, H5F_ACC_RDWR, H5P_DEFAULT);

  // Open the relevant group.
  sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[n]);
  group_id = H5Gopen(file_id, target_group, H5P_DEFAULT);

  // Write
  if (TotHalosPerSnap[n] > 0) {
    status = H5TBappend_records(
        group_id, "Galaxies", (hsize_t)(TotHalosPerSnap[n]), HDF5_dst_size,
        HDF5_dst_offsets, HDF5_dst_sizes,
        (struct HaloOutput *)(ptr_galsnapdata[n] + offset_galsnapdata[n]));
    if (status < 0) {
      FATAL_ERROR(
          "Failed to append %d halo records to HDF5 file '%s', snapshot %d",
          TotHalosPerSnap[n], fname, MimicConfig.ListOutputSnaps[n]);
    }
  }

  // Close the group and file.
  H5Gclose(group_id);
  H5Fclose(file_id);
}
#endif //  MINIMIZE_IO

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
  attribute_id = H5Acreate(dataset_id, "Ntrees", H5T_NATIVE_INT, dataspace_id,
                           H5P_DEFAULT, H5P_DEFAULT);
  status = H5Awrite(attribute_id, H5T_NATIVE_INT, &Ntrees);
  if (status < 0) {
    FATAL_ERROR("Failed to write Ntrees attribute to HDF5 file (filenr %d)",
                filenr);
  }
  H5Aclose(attribute_id);

  // Write the total number of objects.
  attribute_id = H5Acreate(dataset_id, "TotHalosPerSnap", H5T_NATIVE_INT,
                           dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  status = H5Awrite(attribute_id, H5T_NATIVE_INT, &TotHalosPerSnap[n]);
  if (status < 0) {
    FATAL_ERROR(
        "Failed to write TotHalosPerSnap attribute to HDF5 file (filenr %d)",
        filenr);
  }
  H5Aclose(attribute_id);

  // Close the scalar dataspace (reused below)
  H5Sclose(dataspace_id);

  // Close the dataset
  H5Dclose(dataset_id);

  // Write field metadata table (auto-generated from property metadata)
  // Creates FieldMetadata dataset with field names and units for discoverability
  #include "../../include/generated/hdf5_field_metadata.inc"

  // Create an array dataset to hold the number of objects per tree and write
  // it.
  dims = Ntrees;
  if (dims <= 0) {
    FATAL_ERROR("Invalid number of trees (Ntrees=%d) in write_hdf5_attrs for "
                "snapshot %d (filenr %d)",
                (int)dims, MimicConfig.ListOutputSnaps[n], filenr);
  }
  dataspace_id = H5Screate_simple(1, &dims, NULL);
  dataset_id = H5Dcreate(group_id, "TreeHalosPerSnap", H5T_NATIVE_INT,
                         dataspace_id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

  // Add description attribute for self-documentation
  hid_t attr_space_desc = H5Screate(H5S_SCALAR);
  hid_t str_type_desc = H5Tcopy(H5T_C_S1);
  H5Tset_size(str_type_desc, 64);
  hid_t attr_desc = H5Acreate(dataset_id, "description", str_type_desc,
                              attr_space_desc, H5P_DEFAULT, H5P_DEFAULT);
  const char *desc_text = "Number of halos per merger tree at this snapshot";
  herr_t attr_status = H5Awrite(attr_desc, str_type_desc, desc_text);
  if (attr_status < 0) {
    FATAL_ERROR("Failed to write description attribute for TreeHalosPerSnap "
                "(snapshot %d, filenr %d)",
                MimicConfig.ListOutputSnaps[n], filenr);
  }
  H5Aclose(attr_desc);
  H5Tclose(str_type_desc);
  H5Sclose(attr_space_desc);

  // Write the halos per tree data
  status = H5Dwrite(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    InputHalosPerSnap[n]);
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
 * @brief   Configuration parameter descriptor for HDF5 output
 *
 * Local structure to define which MimicConfig fields to write to HDF5.
 * This provides a generic, table-driven approach that maintains
 * core-physics separation (Vision Principle #1).
 */
typedef struct {
  const char *name;     /* HDF5 attribute name */
  int type;             /* Parameter type (INT, DOUBLE, STRING) */
  void *address;        /* Pointer to MimicConfig field */
} ConfigParamDescriptor;

/**
 * @brief   Writes version information to HDF5 file
 *
 * @param   parent_group_id   HDF5 group ID to create Version subgroup in
 *
 * Creates a Version subgroup containing git version information and
 * HDF5 format version for reproducibility tracking.
 */
static void write_version_metadata(hid_t parent_group_id) {
  hid_t version_group_id, attribute_id, dataspace_id, str_type;
  hsize_t dims = 1;
  herr_t status;

  /* Include git version info generated at build time */
  #include "../../build/generated/git_version.h"

  /* Create Version subgroup */
  version_group_id = H5Gcreate(parent_group_id, "Version", H5P_DEFAULT,
                               H5P_DEFAULT, H5P_DEFAULT);
  if (version_group_id < 0) {
    FATAL_ERROR("Failed to create Version subgroup in HDF5 file");
  }

  /* Set up string type and dataspace */
  dataspace_id = H5Screate_simple(1, &dims, NULL);
  str_type = H5Tcopy(H5T_C_S1);
  status = H5Tset_size(str_type, 128);
  if (status < 0) {
    FATAL_ERROR("Failed to set HDF5 string type size for version metadata");
  }

  /* Write git commit SHA */
  attribute_id = H5Acreate(version_group_id, "git_commit", str_type,
                           dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, GIT_COMMIT);
  H5Aclose(attribute_id);

  /* Write git branch */
  attribute_id = H5Acreate(version_group_id, "git_branch", str_type,
                           dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, GIT_BRANCH);
  H5Aclose(attribute_id);

  /* Write git date */
  attribute_id = H5Acreate(version_group_id, "git_date", str_type,
                           dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, GIT_DATE);
  H5Aclose(attribute_id);

  /* Write build date */
  attribute_id = H5Acreate(version_group_id, "build_date", str_type,
                           dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, BUILD_DATE);
  H5Aclose(attribute_id);

  /* Write HDF5 format version (increment when output schema changes) */
  const char *hdf5_format_version = "1.0";
  attribute_id = H5Acreate(version_group_id, "hdf5_format_version",
                           str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, hdf5_format_version);
  H5Aclose(attribute_id);

  /* Clean up */
  H5Sclose(dataspace_id);
  H5Tclose(str_type);
  H5Gclose(version_group_id);
}

/**
 * @brief   Writes runtime parameters to HDF5 file as compound dataset
 *
 * @param   parent_group_id   HDF5 group ID to create Parameters dataset in
 *
 * Creates a Parameters dataset containing all model parameters from the
 * input YAML file. Stores parameters as string key-value pairs, preserving
 * exact input values for perfect reproducibility.
 *
 * Vision Principle 1 (Physics-Agnostic Core): Iterates parameters generically
 * without knowledge of specific physics meanings.
 *
 * Vision Principle 4 (Single Source of Truth): Parameters are already validated
 * and stored in MimicConfig.ModelParams[] during input parsing.
 */
static void write_parameters_metadata(hid_t parent_group_id) {
  hid_t dataset_id, dataspace_id, memtype, filetype;
  hsize_t dims;
  herr_t status;

  /* Check if there are any parameters to write */
  if (MimicConfig.NumModelParams == 0) {
    DEBUG_LOG("No model parameters to write to HDF5");
    return;
  }

  /* Create compound datatype for parameter table (name, value pairs) */
  memtype = H5Tcreate(H5T_COMPOUND, sizeof(struct {
    char param_name[MAX_STRING_LEN];
    char value[MAX_STRING_LEN];
  }));

  hid_t str_type = H5Tcopy(H5T_C_S1);
  H5Tset_size(str_type, MAX_STRING_LEN);

  H5Tinsert(memtype, "param_name", 0, str_type);
  H5Tinsert(memtype, "value", MAX_STRING_LEN, str_type);

  /* Create matching file type */
  filetype = H5Tcreate(H5T_COMPOUND, sizeof(struct {
    char param_name[MAX_STRING_LEN];
    char value[MAX_STRING_LEN];
  }));
  H5Tinsert(filetype, "param_name", 0, str_type);
  H5Tinsert(filetype, "value", MAX_STRING_LEN, str_type);

  /* Create dataspace */
  dims = MimicConfig.NumModelParams;
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  /* Create dataset */
  dataset_id = H5Dcreate(parent_group_id, "Parameters", filetype,
                         dataspace_id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create Parameters dataset in HDF5 file");
  }

  /* Write the parameter data directly from MimicConfig.ModelParams */
  status = H5Dwrite(dataset_id, memtype, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    MimicConfig.ModelParams);
  if (status < 0) {
    FATAL_ERROR("Failed to write Parameters dataset to HDF5 file");
  }

  /* Add description attribute for self-documentation */
  hid_t attr_space = H5Screate(H5S_SCALAR);
  hid_t attr_str_type = H5Tcopy(H5T_C_S1);
  H5Tset_size(attr_str_type, 256);
  hid_t attr_id = H5Acreate(dataset_id, "description", attr_str_type,
                            attr_space, H5P_DEFAULT, H5P_DEFAULT);
  const char *desc = "Runtime model parameters from input YAML file (modules.parameters section)";
  H5Awrite(attr_id, attr_str_type, desc);
  H5Aclose(attr_id);
  H5Tclose(attr_str_type);
  H5Sclose(attr_space);

  /* Clean up */
  H5Dclose(dataset_id);
  H5Sclose(dataspace_id);
  H5Tclose(filetype);
  H5Tclose(memtype);
  H5Tclose(str_type);
}

/**
 * @brief   Writes redshift array to HDF5 file
 *
 * @param   parent_group_id   HDF5 group ID to create Redshifts dataset in
 *
 * Creates a Redshifts dataset containing the redshift for each snapshot index.
 * This enables self-contained files that don't require external snapshot list
 * files for analysis.
 */
static void write_redshifts(hid_t parent_group_id) {
  hid_t dataset_id, dataspace_id, attribute_id, attr_space, str_type;
  hsize_t dims;
  herr_t status;

  /* Create dataspace for redshift array */
  dims = MimicConfig.LastSnapshotNr + 1;  /* E.g., 64 snapshots (0-63) */
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  /* Create dataset */
  dataset_id = H5Dcreate(parent_group_id, "Redshifts", H5T_NATIVE_DOUBLE,
                         dataspace_id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create Redshifts dataset in HDF5 file");
  }

  /* Write the redshift array from MimicConfig.ZZ */
  status = H5Dwrite(dataset_id, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                    H5P_DEFAULT, MimicConfig.ZZ);
  if (status < 0) {
    FATAL_ERROR("Failed to write Redshifts dataset to HDF5 file");
  }

  /* Add description attribute */
  attr_space = H5Screate(H5S_SCALAR);
  str_type = H5Tcopy(H5T_C_S1);
  H5Tset_size(str_type, 128);
  attribute_id = H5Acreate(dataset_id, "description", str_type, attr_space,
                           H5P_DEFAULT, H5P_DEFAULT);
  const char *desc = "Redshift for each snapshot index (0 to LastSnapshotNr)";
  status = H5Awrite(attribute_id, str_type, desc);
  if (status < 0) {
    FATAL_ERROR("Failed to write description attribute for Redshifts");
  }
  H5Aclose(attribute_id);
  H5Tclose(str_type);
  H5Sclose(attr_space);

  /* Clean up */
  H5Dclose(dataset_id);
  H5Sclose(dataspace_id);
}

/**
 * @brief   Writes enabled modules list to HDF5 file
 *
 * @param   parent_group_id   HDF5 group ID to create EnabledModules dataset in
 *
 * Creates an EnabledModules dataset containing the list of enabled module
 * names in execution order. Uses variable-length string dataset for
 * professional HDF5 storage.
 */
static void write_enabled_modules(hid_t parent_group_id) {
  hid_t dataset_id, dataspace_id, str_type, attribute_id, attr_space, attr_str_type;
  hsize_t dims;
  herr_t status;

  /* Check if there are any enabled modules */
  if (MimicConfig.NumEnabledModules == 0) {
    DEBUG_LOG("No enabled modules to write to HDF5");
    return;
  }

  /* Create variable-length string datatype */
  str_type = H5Tcopy(H5T_C_S1);
  status = H5Tset_size(str_type, MAX_STRING_LEN);
  if (status < 0) {
    FATAL_ERROR("Failed to set string type size for EnabledModules dataset");
  }

  /* Create dataspace */
  dims = MimicConfig.NumEnabledModules;
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  /* Create dataset */
  dataset_id = H5Dcreate(parent_group_id, "EnabledModules", str_type,
                         dataspace_id, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  if (dataset_id < 0) {
    FATAL_ERROR("Failed to create EnabledModules dataset in HDF5 file");
  }

  /* Write the module names array */
  status = H5Dwrite(dataset_id, str_type, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                    MimicConfig.EnabledModules);
  if (status < 0) {
    FATAL_ERROR("Failed to write EnabledModules dataset to HDF5 file");
  }

  /* Add description attribute */
  attr_space = H5Screate(H5S_SCALAR);
  attr_str_type = H5Tcopy(H5T_C_S1);
  H5Tset_size(attr_str_type, 128);
  attribute_id = H5Acreate(dataset_id, "description", attr_str_type,
                           attr_space, H5P_DEFAULT, H5P_DEFAULT);
  const char *desc = "List of enabled physics modules in execution order";
  H5Awrite(attribute_id, attr_str_type, desc);
  H5Aclose(attribute_id);
  H5Tclose(attr_str_type);
  H5Sclose(attr_space);

  /* Clean up */
  H5Dclose(dataset_id);
  H5Sclose(dataspace_id);
  H5Tclose(str_type);
}

/**
 * @brief   Writes essential metadata to per-file output for self-containment
 *
 * @param   file_id   HDF5 file ID for per-file output
 *
 * Creates a RunProperties group in per-file outputs containing version
 * information and runtime parameters. This makes each output file self-
 * contained and analyzable without the master file.
 *
 * Vision Principle 4 (Single Source of Truth): Metadata is written by
 * calling the same helper functions used for master file, maintaining DRY.
 */
static void write_perfile_metadata(hid_t file_id) {
  hid_t props_group_id;

  /* Create RunProperties group for per-file metadata */
  props_group_id = H5Gcreate(file_id, "RunProperties", H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT);
  if (props_group_id < 0) {
    FATAL_ERROR("Failed to create RunProperties group in per-file HDF5 output");
  }

  /* Write essential metadata for self-containment (same order as master) */
  write_version_metadata(props_group_id);     /* Identity */
  write_enabled_modules(props_group_id);      /* Configuration */
  write_parameters_metadata(props_group_id);  /* Configuration */
  write_redshifts(props_group_id);            /* Auxiliary */

  H5Gclose(props_group_id);
}

/**
 * @brief   Stores simulation configuration to HDF5 file as attributes
 *
 * @param   master_file_id   HDF5 file ID to write parameters to
 *
 * This function creates a group in the HDF5 file to store simulation
 * configuration as attributes. Uses a local parameter table for generic,
 * physics-agnostic iteration over MimicConfig fields (Vision Principle #1).
 *
 * The table-driven approach ensures:
 * - Core code doesn't hardcode specific parameter names
 * - Adding/removing config fields only requires updating the local table
 * - Maintains separation between core infrastructure and configuration
 *
 * Note: OutputDir is intentionally excluded (may contain sensitive paths)
 */
static void store_run_properties(hid_t master_file_id) {
  hid_t props_group_id, dataspace_id, attribute_id, str_type;
  hsize_t dims;
  herr_t status;
  time_t t;
  struct tm *local;
  int i;

  /* Configuration parameter table - defines what to write to HDF5
   * This table approach keeps the code generic and agnostic to specific parameters */
  ConfigParamDescriptor config_params[] = {
    /* File information */
    {"OutputFileBaseName", STRING, &MimicConfig.OutputFileBaseName},
    {"TreeName", STRING, &MimicConfig.TreeName},
    {"SimulationDir", STRING, &MimicConfig.SimulationDir},
    {"FileWithSnapList", STRING, &MimicConfig.FileWithSnapList},

    /* Simulation parameters */
    {"LastSnapshotNr", INT, &MimicConfig.LastSnapshotNr},
    {"FirstFile", INT, &MimicConfig.FirstFile},
    {"LastFile", INT, &MimicConfig.LastFile},
    {"NumOutputs", INT, &MimicConfig.NOUT},
    {"BoxSize", DOUBLE, &MimicConfig.BoxSize},

    /* Cosmology */
    {"Omega", DOUBLE, &MimicConfig.Omega},
    {"OmegaLambda", DOUBLE, &MimicConfig.OmegaLambda},
    {"Hubble_h", DOUBLE, &MimicConfig.Hubble_h},
    {"PartMass", DOUBLE, &MimicConfig.PartMass},

    /* Units */
    {"UnitVelocity_in_cm_per_s", DOUBLE, &MimicConfig.UnitVelocity_in_cm_per_s},
    {"UnitLength_in_cm", DOUBLE, &MimicConfig.UnitLength_in_cm},
    {"UnitMass_in_g", DOUBLE, &MimicConfig.UnitMass_in_g},
  };
  int num_config_params = sizeof(config_params) / sizeof(ConfigParamDescriptor);

  /* Create the group to hold the run properties */
  props_group_id = H5Gcreate(master_file_id, "RunProperties", H5P_DEFAULT,
                             H5P_DEFAULT, H5P_DEFAULT);

  /* Set up common data structures for attributes */
  dims = 1;
  dataspace_id = H5Screate_simple(1, &dims, NULL);
  str_type = H5Tcopy(H5T_C_S1);
  status = H5Tset_size(str_type, MAX_STRING_LEN);
  if (status < 0) {
    FATAL_ERROR("Failed to set HDF5 string type size for run properties");
  }

  /* Write all config parameters from table (generic iteration) */
  for (i = 0; i < num_config_params; i++) {
    switch (config_params[i].type) {
    case INT:
      attribute_id = H5Acreate(props_group_id, config_params[i].name,
                              H5T_NATIVE_INT, dataspace_id,
                              H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attribute_id, H5T_NATIVE_INT, config_params[i].address);
      H5Aclose(attribute_id);
      break;

    case DOUBLE:
      attribute_id = H5Acreate(props_group_id, config_params[i].name,
                              H5T_NATIVE_DOUBLE, dataspace_id,
                              H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attribute_id, H5T_NATIVE_DOUBLE, config_params[i].address);
      H5Aclose(attribute_id);
      break;

    case STRING:
      attribute_id = H5Acreate(props_group_id, config_params[i].name,
                              str_type, dataspace_id,
                              H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attribute_id, str_type, config_params[i].address);
      H5Aclose(attribute_id);
      break;
    }
  }

  /* TreeType enum requires special handling (enum -> string conversion) */
  const char *tree_type_str;
  switch (MimicConfig.TreeType) {
  case lhalo_binary:
    tree_type_str = "lhalo_binary";
    break;
  case genesis_lhalo_hdf5:
    tree_type_str = "genesis_lhalo_hdf5";
    break;
  default:
    tree_type_str = "unknown";
  }
  attribute_id = H5Acreate(props_group_id, "TreeType", str_type,
                          dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, tree_type_str);
  H5Aclose(attribute_id);

  /* Runtime metadata */
  attribute_id = H5Acreate(props_group_id, "NCores", H5T_NATIVE_INT,
                           dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
#ifdef MPI
  H5Awrite(attribute_id, H5T_NATIVE_INT, &NTask);
#else
  int ncores = 1;
  H5Awrite(attribute_id, H5T_NATIVE_INT, &ncores);
#endif
  H5Aclose(attribute_id);

  time(&t);
  local = localtime(&t);
  attribute_id = H5Acreate(props_group_id, "RunEndTime", str_type, dataspace_id,
                           H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, asctime(local));
  H5Aclose(attribute_id);

  /* Add input simulation info if defined */
#ifdef INPUTSIM
  attribute_id = H5Acreate(props_group_id, "InputSimulation", str_type,
                           dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, INPUTSIM);
  H5Aclose(attribute_id);
#endif

  /* Clean up attribute resources (reused above) */
  H5Sclose(dataspace_id);
  H5Tclose(str_type);

  /* Add extended metadata using helper functions (ordered by importance) */
  write_version_metadata(props_group_id);     /* Identity: version & provenance */
  write_enabled_modules(props_group_id);      /* Configuration: which physics */
  write_parameters_metadata(props_group_id);  /* Configuration: parameter values */
  write_redshifts(props_group_id);            /* Auxiliary: snapshot mapping */

  /* Close the RunProperties group */
  H5Gclose(props_group_id);
}

void write_master_file(void) {

  /*
   * Generate a 'master' file that holds soft links to the data in all of the
   * standard output files.
   *
   * Known minor inefficiency: This function re-opens each HDF5 file to read
   * the TotHalosPerSnap attribute (lines 667-681). The data was available in
   * memory during processing but is reset between files. Fixing this would
   * require a new 2D global array, adding complexity for minimal performance
   * benefit (one-time file I/O after all computation). The current simple
   * approach is preferred for code maintainability.
   */

  int filenr, n, ngal_in_file, ngal_in_core;
  char master_file[2 * MAX_STRING_LEN + 50], target_file[2 * MAX_STRING_LEN + 50];
  char target_group[100], source_ds[100];
  hid_t master_file_id, dataset_id, attribute_id, dataspace_id, group_id,
      target_file_id;
  herr_t status;
  hsize_t dims;
  float redshift;
  int ret;

  // Open the master file.
  ret = snprintf(master_file, sizeof(master_file), "%s/%s.hdf5",
                 MimicConfig.OutputDir, MimicConfig.OutputFileBaseName);
  if (ret < 0) {
    FATAL_ERROR("Path formatting error for: %s/%s.hdf5",
                MimicConfig.OutputDir, MimicConfig.OutputFileBaseName);
  }
  if (ret >= (int)sizeof(master_file)) {
    FATAL_ERROR("Master file path too long: %s/%s.hdf5",
                MimicConfig.OutputDir, MimicConfig.OutputFileBaseName);
  }
  DEBUG_LOG("Creating master HDF5 file '%s'", master_file);
  master_file_id =
      H5Fcreate(master_file, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);

  // Loop through each snapshot.
  for (n = 0; n < MimicConfig.NOUT; n++) {

    // Create a group to hold this snapshot's data
    sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[n]);
    group_id = H5Gcreate(master_file_id, target_group, H5P_DEFAULT, H5P_DEFAULT,
                         H5P_DEFAULT);

    // Save the redshift of this snapshot as an attribute
    dims = 1;
    dataspace_id = H5Screate_simple(1, &dims, NULL);
    attribute_id = H5Acreate(group_id, "Redshift", H5T_NATIVE_FLOAT,
                             dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
    redshift = (float)(MimicConfig.ZZ[MimicConfig.ListOutputSnaps[n]]);
    H5Awrite(attribute_id, H5T_NATIVE_FLOAT, &redshift);
    H5Aclose(attribute_id);
    H5Sclose(dataspace_id);

    // Add FieldMetadata table to master file for self-documentation
    // (auto-generated from property metadata, same as per-file output)
    #include "../../include/generated/hdf5_field_metadata.inc"

    H5Gclose(group_id);

    // Loop through each file for this snapshot.
    for (filenr = MimicConfig.FirstFile; filenr <= MimicConfig.LastFile;
         filenr++) {
      // Create a group to hold this snapshot's data
      sprintf(target_group, "Snap%03d/File%03d", MimicConfig.ListOutputSnaps[n], filenr);
      group_id = H5Gcreate(master_file_id, target_group, H5P_DEFAULT,
                           H5P_DEFAULT, H5P_DEFAULT);
      H5Gclose(group_id);

      ngal_in_file = 0;
      // Generate the *relative* path to the actual output file.
      sprintf(target_file, "%s_%03d.hdf5", MimicConfig.OutputFileBaseName, filenr);

      // Create a dataset which will act as the soft link to the output
      sprintf(target_group, "Snap%03d/File%03d/Galaxies", MimicConfig.ListOutputSnaps[n],
              filenr);
      sprintf(source_ds, "Snap%03d/Galaxies", MimicConfig.ListOutputSnaps[n]);
      DEBUG_LOG("Creating external DS link - %s", target_group);
      status = H5Lcreate_external(target_file, source_ds, master_file_id,
                                  target_group, H5P_DEFAULT, H5P_DEFAULT);
      if (status < 0) {
        FATAL_ERROR(
            "Failed to create external link for Galaxies in master file");
      }

      // Create a dataset which will act as the soft link to the array storing
      // the number of objects per tree for this file.
      sprintf(target_group, "Snap%03d/File%03d/TreeHalosPerSnap",
              MimicConfig.ListOutputSnaps[n], filenr);
      sprintf(source_ds, "Snap%03d/TreeHalosPerSnap", MimicConfig.ListOutputSnaps[n]);
      DEBUG_LOG("Creating external DS link - %s", target_group);
      status = H5Lcreate_external(target_file, source_ds, master_file_id,
                                  target_group, H5P_DEFAULT, H5P_DEFAULT);
      if (status < 0) {
        FATAL_ERROR("Failed to create external link for TreeHalosPerSnap in "
                    "master file");
      }

      // Increment the total number of objects for this file.
      ret = snprintf(target_file, sizeof(target_file), "%s/%s_%03d.hdf5",
                     MimicConfig.OutputDir, MimicConfig.OutputFileBaseName, filenr);
      if (ret < 0) {
        FATAL_ERROR("Path formatting error for: %s/%s_%03d.hdf5",
                    MimicConfig.OutputDir, MimicConfig.OutputFileBaseName, filenr);
      }
      if (ret >= (int)sizeof(target_file)) {
        FATAL_ERROR("Target file path too long: %s/%s_%03d.hdf5",
                    MimicConfig.OutputDir, MimicConfig.OutputFileBaseName, filenr);
      }
      target_file_id = H5Fopen(target_file, H5F_ACC_RDONLY, H5P_DEFAULT);
      sprintf(source_ds, "Snap%03d/Galaxies", MimicConfig.ListOutputSnaps[n]);
      dataset_id = H5Dopen(target_file_id, source_ds, H5P_DEFAULT);
      attribute_id = H5Aopen(dataset_id, "TotHalosPerSnap", H5P_DEFAULT);
      status = H5Aread(attribute_id, H5T_NATIVE_INT, &ngal_in_core);
      if (status < 0) {
        FATAL_ERROR("Failed to read TotHalosPerSnap attribute from file '%s'",
                    target_file);
      }
      H5Aclose(attribute_id);
      H5Dclose(dataset_id);
      H5Fclose(target_file_id);
      ngal_in_file += ngal_in_core;

      // Save the total number of objects in this file.
      dims = 1;
      dataspace_id = H5Screate_simple(1, &dims, NULL);
      sprintf(target_group, "Snap%03d/File%03d", MimicConfig.ListOutputSnaps[n], filenr);
      group_id = H5Gopen(master_file_id, target_group, H5P_DEFAULT);
      attribute_id = H5Acreate(group_id, "TotHalosPerSnap", H5T_NATIVE_INT,
                               dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attribute_id, H5T_NATIVE_INT, &ngal_in_file);
      H5Aclose(attribute_id);
      H5Gclose(group_id);
      H5Sclose(dataspace_id);
    }
  }

#ifdef GITREF_STR
  // Save the git ref if requested
  char tempstr[45];

  dims = 1;
  hid_t str_type = H5Tcopy(H5T_C_S1);
  H5Tset_size(str_type, 45);
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  sprintf(tempstr, GITREF_STR);
  attribute_id = H5Acreate(master_file_id, "GitRef", str_type, dataspace_id,
                           H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, tempstr);

  sprintf(tempstr, MODELNAME);
  attribute_id = H5Acreate(master_file_id, "Model", str_type, dataspace_id,
                           H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, tempstr);

  H5Aclose(attribute_id);
  H5Sclose(dataspace_id);
#endif

  // Finally - store the properites of the run...
  store_run_properties(master_file_id);

  // Close the master file.
  H5Fclose(master_file_id);
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
 * 1. Determines output ordering for halos
 * 2. Converts internal halo structures to output format
 * 3. Writes halos to HDF5 files using write_hdf5_halo()
 * 4. Updates halo counts for the file and tree
 *
 * The function handles the indexing system for organizing halos across different
 * trees and files.
 */
void save_halos_hdf5(int filenr, int tree) {
  int i, n;
  int OutputGalCount[MAXSNAPS], *OutputGalOrder;

  /* Prepare output ordering and update merger pointers using shared utility */
  OutputGalOrder = prepare_output_for_tree(OutputGalCount);

  // Now prepare and write halos to HDF5 (BATCH WRITE for performance)
  for (n = 0; n < MimicConfig.NOUT; n++) {
    if (OutputGalCount[n] == 0)
      continue; /* Skip snapshots with no halos */

    /* Allocate array for batch writing using tracked memory allocation */
    struct HaloOutput *halo_batch = (struct HaloOutput *)mymalloc_cat(
        OutputGalCount[n] * sizeof(struct HaloOutput), MEM_IO);
    if (halo_batch == NULL) {
      FATAL_ERROR("Memory allocation failed for HaloOutput batch array (%d "
                  "halos, %zu bytes)",
                  OutputGalCount[n],
                  OutputGalCount[n] * sizeof(struct HaloOutput));
    }

    /* Prepare all halos for this snapshot */
    int batch_idx = 0;
    for (i = 0; i < NumProcessedHalos; i++) {
      if (ProcessedHalos[i].SnapNum == MimicConfig.ListOutputSnaps[n]) {
        prepare_halo_for_output(filenr, tree, &ProcessedHalos[i],
                                &halo_batch[batch_idx]);
        batch_idx++;

        /* Increment halo counters */
        TotHalosPerSnap[n]++;
        InputHalosPerSnap[n][tree]++;
      }
    }

    /* Write entire batch at once */
    if (batch_idx > 0) {
      write_hdf5_halo_batch(halo_batch, batch_idx, n, filenr);
    }

    /* Free batch array using tracked memory deallocation */
    myfree(halo_batch);
  }

  /* Free the workspace using tracked memory deallocation */
  myfree(OutputGalOrder);
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
