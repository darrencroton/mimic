/**
 * @file    master_hdf5.c
 * @brief   Master-file aggregation for HDF5 output
 *
 * Builds the run-level master HDF5 file: external links into every per-filenr
 * output file, per-snapshot redshift attributes, per-file halo counts, and
 * the full RunProperties metadata group.
 *
 * Each partition is linked under Snap<SSS>/File<NNN> when it has no task
 * component and under Snap<SSS>/File<NNN>_task<TTT> when it does (a multi-task
 * horizontal run), matching its file name (output_path_hdf5()).
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
#include "vertical/reader.h" /* enum InputProcessingOrder only (the ProcessingOrder field), never the active reader pointer */

/** The master-file group name of a partition: File<NNN>, or File<NNN>_task<TTT> with a task. */
static void partition_group_name(char *buf, size_t size, int filenr, int task) {
  int written;
  if (task < 0) {
    written = snprintf(buf, size, "File%03d", filenr);
  } else {
    written = snprintf(buf, size, "File%03d_task%03d", filenr, task);
  }
  if (written < 0 || (size_t)written >= size) {
    FATAL_ERROR("Master file group name too long (filenr %d, task %d)", filenr, task);
  }
}

void write_master_file(void) {
  int filenr, n;
  int64_t ngal_in_core;
  char master_file[2 * MAX_STRING_LEN + 50], target_file[2 * MAX_STRING_LEN + 50];
  char relative_target_file[MAX_STRING_LEN + 50], target_group[100], source_ds[100];
  char file_group[32];
  hid_t master_file_id, dataset_id, attribute_id, dataspace_id, group_id, target_file_id;
  herr_t status;
  hsize_t dims;
  float redshift;
  const int horizontal_run =
      (enum InputProcessingOrder)MimicConfig.ProcessingOrder == INPUT_PROCESSING_ORDER_HORIZONTAL;

  output_master_path_hdf5(master_file, sizeof(master_file));
  DEBUG_LOG("Creating master HDF5 file '%s'", master_file);
  master_file_id = H5Fcreate(master_file, H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT);
  if (master_file_id < 0) {
    FATAL_ERROR("Failed to create master HDF5 file '%s'", master_file);
  }

  const struct OutputPartitionSource source = get_output_partition_source();

  if (source.prepare_run != NULL) {
    source.prepare_run();
  }

  if (source.num_partitions == NULL || source.partition_output_id == NULL ||
      source.partition_snapshots == NULL || source.partition_task == NULL) {
    FATAL_ERROR("Output partition source '%s' cannot enumerate HDF5 master partitions",
                source.format_name);
  }
  const int npartitions = source.num_partitions();

  for (n = 0; n < MimicConfig.NOUT; n++) {
    sprintf(target_group, "Snap%03d", MimicConfig.ListOutputSnaps[n]);
    group_id = H5Gcreate(master_file_id, target_group, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
    if (group_id < 0) {
      FATAL_ERROR("Failed to create group '%s' in master file '%s'", target_group, master_file);
    }

    dims = 1;
    dataspace_id = H5Screate_simple(1, &dims, NULL);
    if (dataspace_id < 0) {
      FATAL_ERROR("Failed to create dataspace for Redshift attribute in group '%s' of master "
                  "file '%s'",
                  target_group, master_file);
    }
    attribute_id =
        H5Acreate(group_id, "Redshift", H5T_NATIVE_FLOAT, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
    if (attribute_id < 0) {
      FATAL_ERROR("Failed to create Redshift attribute in group '%s' of master file '%s'",
                  target_group, master_file);
    }
    redshift = (float)(MimicConfig.ZZ[MimicConfig.ListOutputSnaps[n]]);
    status = H5Awrite(attribute_id, H5T_NATIVE_FLOAT, &redshift);
    if (status < 0) {
      FATAL_ERROR("Failed to write Redshift attribute in group '%s' of master file '%s'",
                  target_group, master_file);
    }
    H5Aclose(attribute_id);
    H5Sclose(dataspace_id);

    /* FieldMetadata is written once under RunProperties (store_run_properties),
     * not duplicated per snapshot group. */

    H5Gclose(group_id);
  }

  for (int partition = 0; partition < npartitions; partition++) {
    filenr = source.partition_output_id(partition);
    const int task = source.partition_task(partition);

    if (!source.partition_exists(partition)) {
      INFO_LOG("Skipping master-file links for missing input partition %d", partition);
      continue;
    }

    output_path_hdf5(target_file, sizeof(target_file), filenr, task);
    if (access(target_file, F_OK) != 0) {
      INFO_LOG("Skipping master-file links for missing output file %s", target_file);
      continue;
    }

    target_file_id = H5Fopen(target_file, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (target_file_id < 0) {
      FATAL_ERROR("Failed to open output file '%s' while building master file", target_file);
    }

    output_partition_basename(relative_target_file, sizeof(relative_target_file), filenr, task);
    partition_group_name(file_group, sizeof(file_group), filenr, task);

    const struct OutputSnapshotSelection selection = source.partition_snapshots(partition);

    for (int idx = 0; idx < selection.count; idx++) {
      n = selection.indices[idx];
      sprintf(target_group, "Snap%03d/%s", MimicConfig.ListOutputSnaps[n], file_group);
      group_id = H5Gcreate(master_file_id, target_group, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
      if (group_id < 0) {
        FATAL_ERROR("Failed to create group '%s' in master file '%s'", target_group, master_file);
      }
      H5Gclose(group_id);

      sprintf(target_group, "Snap%03d/%s/Galaxies", MimicConfig.ListOutputSnaps[n], file_group);
      sprintf(source_ds, "Snap%03d/Galaxies", MimicConfig.ListOutputSnaps[n]);
      DEBUG_LOG("Creating external DS link - %s", target_group);
      status = H5Lcreate_external(relative_target_file, source_ds, master_file_id, target_group,
                                  H5P_DEFAULT, H5P_DEFAULT);
      if (status < 0) {
        FATAL_ERROR("Failed to create external link for Galaxies in master file");
      }

      if (!horizontal_run) {
        sprintf(target_group, "Snap%03d/%s/TreeHalosPerSnap", MimicConfig.ListOutputSnaps[n],
                file_group);
        sprintf(source_ds, "Snap%03d/TreeHalosPerSnap", MimicConfig.ListOutputSnaps[n]);
        DEBUG_LOG("Creating external DS link - %s", target_group);
        status = H5Lcreate_external(relative_target_file, source_ds, master_file_id, target_group,
                                    H5P_DEFAULT, H5P_DEFAULT);
        if (status < 0) {
          FATAL_ERROR("Failed to create external link for TreeHalosPerSnap in "
                      "master file");
        }
      }

      sprintf(source_ds, "Snap%03d/Galaxies", MimicConfig.ListOutputSnaps[n]);
      dataset_id = H5Dopen(target_file_id, source_ds, H5P_DEFAULT);
      if (dataset_id < 0) {
        FATAL_ERROR("Failed to open dataset '%s' from file '%s'", source_ds, target_file);
      }
      attribute_id = H5Aopen(dataset_id, "TotHalosPerSnap", H5P_DEFAULT);
      status = H5Aread(attribute_id, H5T_NATIVE_INT64, &ngal_in_core);
      if (status < 0) {
        FATAL_ERROR("Failed to read TotHalosPerSnap attribute from file '%s'", target_file);
      }
      H5Aclose(attribute_id);
      H5Dclose(dataset_id);

      dims = 1;
      dataspace_id = H5Screate_simple(1, &dims, NULL);
      if (dataspace_id < 0) {
        FATAL_ERROR("Failed to create dataspace for TotHalosPerSnap attribute in master file "
                    "'%s'",
                    master_file);
      }
      sprintf(target_group, "Snap%03d/%s", MimicConfig.ListOutputSnaps[n], file_group);
      group_id = H5Gopen(master_file_id, target_group, H5P_DEFAULT);
      if (group_id < 0) {
        FATAL_ERROR("Failed to open group '%s' in master file '%s'", target_group, master_file);
      }
      attribute_id = H5Acreate(group_id, "TotHalosPerSnap", H5T_NATIVE_INT64, dataspace_id,
                               H5P_DEFAULT, H5P_DEFAULT);
      if (attribute_id < 0) {
        FATAL_ERROR("Failed to create TotHalosPerSnap attribute in group '%s' of master file "
                    "'%s'",
                    target_group, master_file);
      }
      status = H5Awrite(attribute_id, H5T_NATIVE_INT64, &ngal_in_core);
      if (status < 0) {
        FATAL_ERROR("Failed to write TotHalosPerSnap attribute in group '%s' of master file "
                    "'%s'",
                    target_group, master_file);
      }
      H5Aclose(attribute_id);
      H5Gclose(group_id);
      H5Sclose(dataspace_id);
    }

    status = H5Fclose(target_file_id);
    if (status < 0) {
      FATAL_ERROR("Failed to close output file '%s' while building master file", target_file);
    }
  }

  if (source.teardown_run != NULL) {
    source.teardown_run();
  }

#ifdef GITREF_STR
  char tempstr[45];

  dims = 1;
  hid_t str_type = H5Tcopy(H5T_C_S1);
  if (str_type < 0) {
    FATAL_ERROR("Failed to copy string type for GitRef/Model attributes in master file '%s'",
                master_file);
  }
  status = H5Tset_size(str_type, 45);
  if (status < 0) {
    FATAL_ERROR("Failed to set string size for GitRef/Model attributes in master file '%s'",
                master_file);
  }
  dataspace_id = H5Screate_simple(1, &dims, NULL);
  if (dataspace_id < 0) {
    FATAL_ERROR("Failed to create dataspace for GitRef/Model attributes in master file '%s'",
                master_file);
  }

  sprintf(tempstr, GITREF_STR);
  attribute_id =
      H5Acreate(master_file_id, "GitRef", str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  if (attribute_id < 0) {
    FATAL_ERROR("Failed to create GitRef attribute in master file '%s'", master_file);
  }
  status = H5Awrite(attribute_id, str_type, tempstr);
  if (status < 0) {
    FATAL_ERROR("Failed to write GitRef attribute in master file '%s'", master_file);
  }

  sprintf(tempstr, MODELNAME);
  attribute_id =
      H5Acreate(master_file_id, "Model", str_type, dataspace_id, H5P_DEFAULT, H5P_DEFAULT);
  if (attribute_id < 0) {
    FATAL_ERROR("Failed to create Model attribute in master file '%s'", master_file);
  }
  status = H5Awrite(attribute_id, str_type, tempstr);
  if (status < 0) {
    FATAL_ERROR("Failed to write Model attribute in master file '%s'", master_file);
  }

  H5Aclose(attribute_id);
  H5Sclose(dataspace_id);
#endif

  store_run_properties(master_file_id);

  status = H5Fclose(master_file_id);
  if (status < 0) {
    FATAL_ERROR("Failed to close master HDF5 file '%s'", master_file);
  }
}
