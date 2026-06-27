/**
 * @file    master_hdf5.c
 * @brief   Master-file aggregation for HDF5 output
 *
 * Builds the run-level master HDF5 file: external links into every per-filenr
 * output file, per-snapshot redshift attributes, per-file halo counts, and
 * the full RunProperties metadata group.
 */

#include <hdf5.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include "proto.h"
#include "error.h"
#include "globals.h"
#include "hdf5_internal.h"
#include "output/hdf5.h"
#include "output/util.h"
#include "tree/reader.h"

void write_master_file(void) {
  int filenr, n, ngal_in_core;
  char master_file[2 * MAX_STRING_LEN + 50], target_file[2 * MAX_STRING_LEN + 50];
  char relative_target_file[MAX_STRING_LEN + 50], target_group[100], source_ds[100];
  hid_t master_file_id, dataset_id, attribute_id, dataspace_id, group_id, target_file_id;
  herr_t status;
  hsize_t dims;
  float redshift;
  int ret;

  ret = snprintf(master_file, sizeof(master_file), "%s/%s.hdf5", MimicConfig.OutputDir,
                 MimicConfig.OutputFileBaseName);
  if (ret < 0) {
    FATAL_ERROR("Path formatting error for: %s/%s.hdf5", MimicConfig.OutputDir,
                MimicConfig.OutputFileBaseName);
  }
  if (ret >= (int)sizeof(master_file)) {
    FATAL_ERROR("Master file path too long: %s/%s.hdf5", MimicConfig.OutputDir,
                MimicConfig.OutputFileBaseName);
  }
  DEBUG_LOG("Creating master HDF5 file '%s'", master_file);
  master_file_id = H5Fcreate(master_file, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);

  if (MimicConfig.reader->prepare_run != NULL) {
    MimicConfig.reader->prepare_run();
  }

  if (MimicConfig.reader->num_partitions == NULL ||
      MimicConfig.reader->partition_output_id == NULL) {
    FATAL_ERROR("Tree reader '%s' cannot enumerate HDF5 master partitions",
                MimicConfig.reader->name);
  }
  const int npartitions = MimicConfig.reader->num_partitions();

  for (n = 0; n < MimicConfig.NOUT; n++) {
    sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[n]);
    group_id = H5Gcreate(master_file_id, target_group, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);

    dims = 1;
    dataspace_id = H5Screate_simple(1, &dims, NULL);
    attribute_id =
        H5Acreate(group_id, "Redshift", H5T_NATIVE_FLOAT, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
    redshift = (float)(MimicConfig.ZZ[MimicConfig.ListOutputSnaps[n]]);
    H5Awrite(attribute_id, H5T_NATIVE_FLOAT, &redshift);
    H5Aclose(attribute_id);
    H5Sclose(dataspace_id);

    /* FieldMetadata is written once under RunProperties (store_run_properties),
     * not duplicated per snapshot group. */

    H5Gclose(group_id);
  }

  for (int partition = 0; partition < npartitions; partition++) {
    filenr = MimicConfig.reader->partition_output_id(partition);

    if (MimicConfig.reader->partition_model == PARTITION_ENUMERATED &&
        !MimicConfig.reader->partition_exists(partition)) {
      INFO_LOG("Skipping master-file links for missing input partition %d", partition);
      continue;
    }

    output_path_hdf5(target_file, sizeof(target_file), filenr);
    if (access(target_file, F_OK) != 0) {
      INFO_LOG("Skipping master-file links for missing output file %s", target_file);
      continue;
    }

    target_file_id = H5Fopen(target_file, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (target_file_id < 0) {
      FATAL_ERROR("Failed to open output file '%s' while building master file", target_file);
    }

    sprintf(relative_target_file, "%s_%03d.hdf5", MimicConfig.OutputFileBaseName, filenr);

    for (n = 0; n < MimicConfig.NOUT; n++) {
      sprintf(target_group, "Snap%03d/File%03d", MimicConfig.ListOutputSnaps[n], filenr);
      group_id = H5Gcreate(master_file_id, target_group, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      H5Gclose(group_id);

      sprintf(target_group, "Snap%03d/File%03d/Galaxies", MimicConfig.ListOutputSnaps[n], filenr);
      sprintf(source_ds, "Snap%03d/Galaxies", MimicConfig.ListOutputSnaps[n]);
      DEBUG_LOG("Creating external DS link - %s", target_group);
      status = H5Lcreate_external(relative_target_file, source_ds, master_file_id, target_group,
                                  H5P_DEFAULT, H5P_DEFAULT);
      if (status < 0) {
        FATAL_ERROR("Failed to create external link for Galaxies in master file");
      }

      sprintf(target_group, "Snap%03d/File%03d/TreeHalosPerSnap", MimicConfig.ListOutputSnaps[n],
              filenr);
      sprintf(source_ds, "Snap%03d/TreeHalosPerSnap", MimicConfig.ListOutputSnaps[n]);
      DEBUG_LOG("Creating external DS link - %s", target_group);
      status = H5Lcreate_external(relative_target_file, source_ds, master_file_id, target_group,
                                  H5P_DEFAULT, H5P_DEFAULT);
      if (status < 0) {
        FATAL_ERROR("Failed to create external link for TreeHalosPerSnap in "
                    "master file");
      }

      sprintf(source_ds, "Snap%03d/Galaxies", MimicConfig.ListOutputSnaps[n]);
      dataset_id = H5Dopen(target_file_id, source_ds, H5P_DEFAULT);
      if (dataset_id < 0) {
        FATAL_ERROR("Failed to open dataset '%s' from file '%s'", source_ds, target_file);
      }
      attribute_id = H5Aopen(dataset_id, "TotHalosPerSnap", H5P_DEFAULT);
      status = H5Aread(attribute_id, H5T_NATIVE_INT, &ngal_in_core);
      if (status < 0) {
        FATAL_ERROR("Failed to read TotHalosPerSnap attribute from file '%s'", target_file);
      }
      H5Aclose(attribute_id);
      H5Dclose(dataset_id);

      dims = 1;
      dataspace_id = H5Screate_simple(1, &dims, NULL);
      sprintf(target_group, "Snap%03d/File%03d", MimicConfig.ListOutputSnaps[n], filenr);
      group_id = H5Gopen(master_file_id, target_group, H5P_DEFAULT);
      attribute_id = H5Acreate(group_id, "TotHalosPerSnap", H5T_NATIVE_INT, dataspace_id,
                               H5P_DEFAULT, H5P_DEFAULT);
      H5Awrite(attribute_id, H5T_NATIVE_INT, &ngal_in_core);
      H5Aclose(attribute_id);
      H5Gclose(group_id);
      H5Sclose(dataspace_id);
    }

    H5Fclose(target_file_id);
  }

  if (MimicConfig.reader->teardown_run != NULL) {
    MimicConfig.reader->teardown_run();
  }

#ifdef GITREF_STR
  char tempstr[45];

  dims = 1;
  hid_t str_type = H5Tcopy(H5T_C_S1);
  H5Tset_size(str_type, 45);
  dataspace_id = H5Screate_simple(1, &dims, NULL);

  sprintf(tempstr, GITREF_STR);
  attribute_id =
      H5Acreate(master_file_id, "GitRef", str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, tempstr);

  sprintf(tempstr, MODELNAME);
  attribute_id =
      H5Acreate(master_file_id, "Model", str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  H5Awrite(attribute_id, str_type, tempstr);

  H5Aclose(attribute_id);
  H5Sclose(dataspace_id);
#endif

  store_run_properties(master_file_id);

  H5Fclose(master_file_id);
}
