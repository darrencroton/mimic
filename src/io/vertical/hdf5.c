/**
 * @file    vertical/hdf5.c
 * @brief   Functions for reading HDF5 format merger tree files
 *
 * This file implements functionality for loading merger trees from
 * HDF5 format files. It handles the reading of tree metadata and
 * halo data for individual trees, providing an interface to the core
 * Mimic code that is independent of the specific file format.
 *
 * HDF5 format trees are a newer, more flexible format compared to
 * the traditional binary format. The HDF5 format allows for:
 * - Self-describing data with attributes and metadata
 * - Better portability across different systems
 * - Easier extensibility for future enhancements
 *
 * Key functions:
 * - open_lhalo_hdf5_header(): The one parser of the per-file /Header, shared by
 *   the count, largest-tree and open hooks
 * - open_partition_hdf5(): Reads tree metadata from an HDF5 file
 * - load_unit_hdf5(): Loads a specific tree's halo data
 * - close_partition_hdf5(): Closes the HDF5 file
 * - read_attribute_int_checked(): Extent-checked reader for integer attributes
 * - read_dataset(): Helper for reading datasets of various types
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "globals.h"
#include "proto.h"
#include "vertical/hdf5.h"
#include "vertical/reader.h"
#include "types.h"
#include "generated/tree_property_accessors.h"

static hid_t hdf5_file = -1;

/* The per-file /Header attributes of the L-Halo-tree HDF5 layout: the tree count,
   the total halo count, and the per-tree halo counts. */
#define LHALO_HDF5_HEADER_GROUP "/Header"
#define LHALO_HDF5_NTREES "Ntrees"
#define LHALO_HDF5_TOTNHALOS "totNHalos"
#define LHALO_HDF5_TREE_NHALOS "InputTreeNHalos"

#define LHALO_HDF5_PATH_LEN (3 * MAX_STRING_LEN + 15)

/* How much of the /Header open_lhalo_hdf5_header() parses. */
enum LHaloHDF5HeaderScope {
  HDF5_HEADER_COUNTS, /* Ntrees and totNHalos only */
  HDF5_HEADER_MAX,    /* plus InputTreeNHalos, reduced to its largest entry */
  HDF5_HEADER_TABLE,  /* plus InputTreeNHalos, kept for load_unit_hdf5() */
};

/* What open_lhalo_hdf5_header() parsed; fields beyond the scope stay zero or NULL. */
struct LHaloHDF5Header {
  char path[LHALO_HDF5_PATH_LEN];
  int ntrees;
  int tot_nhalos;
  int64_t max_halos; /* largest InputTreeNHalos entry; 0 for a file with no trees */
  int *tree_nhalos;  /* MEM_TREES block of ntrees counts (HDF5_HEADER_TABLE only) */
};

/* How read_dataset() should interpret a dataset's elements */
enum ReadDatatype { READ_AS_INT = 0, READ_AS_FLOAT = 1, READ_AS_LLONG = 2 };

static void format_lhalo_hdf5_partition_path(char *buf, size_t size, int output_id);
static int32_t read_attribute_int_checked(hid_t my_hdf5_file, const char *file_path,
                                          const char *groupname, const char *attr_name,
                                          int *attribute, int64_t expected_count);
static int32_t read_dataset(char *dataset_name, enum ReadDatatype datatype, void *buffer);
static int64_t count_partition_units_hdf5(int partition);
static int64_t max_partition_unit_halos_hdf5(int partition);

/**
 * @brief   Open an HDF5 partition file and parse its /Header.
 * @param   output_id   Output id of the partition (the L-Halo filenr).
 * @param   scope       How much of the header to parse.
 * @param   header      Receives the file path and what was parsed.
 * @return  The open read-only file. The caller closes it and, for HDF5_HEADER_TABLE,
 *          owns header->tree_nhalos.
 *
 * Every attribute goes through the extent-checked reader: Ntrees and totNHalos must
 * hold one element each and InputTreeNHalos exactly Ntrees. A file with no trees has
 * no table to read, so every scope answers zero trees and a zero maximum whether it
 * carries an empty InputTreeNHalos or none. A missing or mis-sized attribute, a
 * negative Ntrees or a negative table entry is FATAL, after the file is closed and
 * any table released.
 */
static hid_t open_lhalo_hdf5_header(int output_id, enum LHaloHDF5HeaderScope scope,
                                    struct LHaloHDF5Header *header) {
  memset(header, 0, sizeof(*header));
  format_lhalo_hdf5_partition_path(header->path, sizeof(header->path), output_id);
  const char *path = header->path;

  const hid_t file = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) {
    FATAL_ERROR("Failed to open HDF5 tree file '%s'", path);
  }

  if (read_attribute_int_checked(file, path, LHALO_HDF5_HEADER_GROUP, LHALO_HDF5_NTREES,
                                 &header->ntrees, 1) != EXIT_SUCCESS) {
    H5Fclose(file);
    FATAL_ERROR("Failed to read the %s attribute from file '%s'", LHALO_HDF5_NTREES, path);
  }
  if (header->ntrees < 0) {
    H5Fclose(file);
    FATAL_ERROR("HDF5 tree file '%s' reports negative NTrees=%d", path, header->ntrees);
  }
  if (read_attribute_int_checked(file, path, LHALO_HDF5_HEADER_GROUP, LHALO_HDF5_TOTNHALOS,
                                 &header->tot_nhalos, 1) != EXIT_SUCCESS) {
    H5Fclose(file);
    FATAL_ERROR("Failed to read the %s attribute from file '%s'", LHALO_HDF5_TOTNHALOS, path);
  }
  if (scope == HDF5_HEADER_COUNTS) {
    return file;
  }

  int *table = mymalloc_cat(sizeof(int) * (size_t)header->ntrees, MEM_TREES);
  if (header->ntrees > 0 &&
      read_attribute_int_checked(file, path, LHALO_HDF5_HEADER_GROUP, LHALO_HDF5_TREE_NHALOS, table,
                                 header->ntrees) != EXIT_SUCCESS) {
    myfree(table);
    H5Fclose(file);
    IO_FATAL_ERROR(IO_ERROR_HDF5, "read_attribute", path,
                   "Failed to read the %s attribute for %d trees", LHALO_HDF5_TREE_NHALOS,
                   header->ntrees);
  }
  for (int i = 0; i < header->ntrees; i++) {
    if (table[i] < 0) {
      const int negative = table[i];
      myfree(table);
      H5Fclose(file);
      FATAL_ERROR("HDF5 tree file '%s' reports negative halo count %d for tree %d", path, negative,
                  i);
    }
    if (table[i] > header->max_halos) {
      header->max_halos = table[i];
    }
  }

  if (scope == HDF5_HEADER_TABLE) {
    header->tree_nhalos = table;
  } else {
    myfree(table);
  }
  return file;
}

/**
 * @brief   Open the HDF5 partition file and read its /Header tree-count attributes.
 * @param   output_id   Output id of the partition (the L-Halo filenr).
 *
 * Stages Ntrees and InputTreeNHalos[Ntrees] from the /Header group, then builds
 * InputTreeFirstHalo[]. Leaves the file handle open for load_unit_hdf5.
 */
void open_partition_hdf5(int output_id) {
  struct LHaloHDF5Header header;

  hdf5_file = open_lhalo_hdf5_header(output_id, HDF5_HEADER_TABLE, &header);
  Ntrees = header.ntrees;
  InputTreeNHalos = header.tree_nhalos;
  DEBUG_LOG("There are %d trees and %d total halos", Ntrees, header.tot_nhalos);

  InputTreeFirstHalo = mymalloc_cat(sizeof(int) * Ntrees, MEM_TREES);
  if (Ntrees)
    InputTreeFirstHalo[0] = 0;
  for (int i = 1; i < Ntrees; i++)
    InputTreeFirstHalo[i] = InputTreeFirstHalo[i - 1] + InputTreeNHalos[i - 1];
}

#define READ_TREE_PROPERTY(field_name, hdf5_name, type_int, data_type)                             \
  {                                                                                                \
    snprintf(dataset_name, MAX_STRING_LEN, "tree_%03d/%s", unit, hdf5_name);                       \
    status = read_dataset(dataset_name, type_int, buffer);                                         \
    if (status != EXIT_SUCCESS) {                                                                  \
      IO_FATAL_ERROR(IO_ERROR_HDF5, "read_dataset", dataset_name,                                  \
                     "Failed to read property for tree %d", unit);                                 \
    }                                                                                              \
    for (halo_idx = 0; halo_idx < NHalos_ThisTree; ++halo_idx) {                                   \
      InputTreeHalos[halo_idx].field_name = ((data_type *)buffer)[halo_idx];                       \
    }                                                                                              \
  }

#define READ_TREE_PROPERTY_MULTIPLEDIM(field_name, hdf5_name, type_int, data_type)                 \
  {                                                                                                \
    snprintf(dataset_name, MAX_STRING_LEN, "tree_%03d/%s", unit, hdf5_name);                       \
    status = read_dataset(dataset_name, type_int, buffer_multipledim);                             \
    if (status != EXIT_SUCCESS) {                                                                  \
      IO_FATAL_ERROR(IO_ERROR_HDF5, "read_dataset", dataset_name,                                  \
                     "Failed to read property for tree %d", unit);                                 \
    }                                                                                              \
    for (halo_idx = 0; halo_idx < NHalos_ThisTree; ++halo_idx) {                                   \
      for (dim = 0; dim < NDIM; ++dim) {                                                           \
        InputTreeHalos[halo_idx].field_name[dim] =                                                 \
            ((data_type *)buffer_multipledim)[halo_idx * NDIM + dim];                              \
      }                                                                                            \
    }                                                                                              \
  }

/**
 * @brief   Load one tree's halo data from the open HDF5 file.
 * @param   unit   Tree index within the open partition.
 *
 * Reads each property via READ_TREE_PROPERTY / READ_TREE_PROPERTY_MULTIPLEDIM into
 * temporary buffers, then slots the values into InputTreeHalos[]. Property names and
 * accessor types come from the generated read_tree_hdf5_properties.inc.
 */
void load_unit_hdf5(int unit) {

  char dataset_name[MAX_STRING_LEN + 1];
  int32_t NHalos_ThisTree, status, halo_idx, dim;

  double *buffer;             /* scalar-field read buffer (largest native type = double) */
  double *buffer_multipledim; /* multidim-field buffer (3× size for position/velocity) */

  if (hdf5_file < 0) {
    IO_FATAL_ERROR(IO_ERROR_HDF5, "read_tree", NULL,
                   "HDF5 file not open when reading tree %d (handle=%lld)", unit,
                   (long long)hdf5_file);
  }

  NHalos_ThisTree = InputTreeNHalos[unit];

  InputTreeHalos = mymalloc_cat(sizeof(struct RawHalo) * NHalos_ThisTree, MEM_TREES);

  buffer = calloc(NHalos_ThisTree, sizeof(*(buffer)));
  if (buffer == NULL) {
    FATAL_ERROR("Memory allocation failed for HDF5 buffer: tree %d, %d halos, "
                "%zu bytes",
                unit, NHalos_ThisTree, NHalos_ThisTree * sizeof(*buffer));
  }

  buffer_multipledim = calloc(NHalos_ThisTree * NDIM, sizeof(*(buffer_multipledim)));
  if (buffer_multipledim == NULL) {
    FATAL_ERROR("Memory allocation failed for HDF5 multidim buffer: tree %d, "
                "%d halos, %zu bytes",
                unit, NHalos_ThisTree, NHalos_ThisTree * NDIM * sizeof(*buffer_multipledim));
  }

#include "../../include/generated/read_tree_hdf5_properties.inc"

  free(buffer);
  free(buffer_multipledim);

#ifdef DEBUG_HDF5_READER
  const struct HaloInputView view = {InputTreeHalos, (int64_t)NHalos_ThisTree};
  int32_t i;
  for (i = 0; i < 20; ++i) {
    DEBUG_LOG("halo %d: Descendant %lld FirstProg %lld x %.4f y %.4f z %.4f", i,
              (long long)mimic_tree_get_Descendant(view, i),
              (long long)mimic_tree_get_FirstProgenitor(view, i), view.halos[i].Pos[0],
              view.halos[i].Pos[1], view.halos[i].Pos[2]);
  }
  // Debug exit point
  FATAL_ERROR("Debug exit after showing first 20 halos");
#endif
}

#undef READ_TREE_PROPERTY
#undef READ_TREE_PROPERTY_MULTIPLEDIM

/** @brief Close the open HDF5 partition file handle. */
void close_partition_hdf5(void) {
  if (hdf5_file >= 0) {
    H5Fclose(hdf5_file);
    hdf5_file = -1;
  }
}

static void format_lhalo_hdf5_partition_path(char *buf, size_t size, int output_id) {
  char filename[2 * MAX_STRING_LEN + 32];
  const char *slot = strstr(MimicConfig.TreeName, "%d");

  if (slot != NULL) {
    const int prefix_len = (int)(slot - MimicConfig.TreeName);
    int filename_len = snprintf(filename, sizeof(filename), "%.*s%d%s", prefix_len,
                                MimicConfig.TreeName, output_id, slot + 2);
    if (filename_len < 0 || (size_t)filename_len >= sizeof(filename)) {
      FATAL_ERROR("L-Halo HDF5 tree filename too long (%d chars, max %zu)", filename_len,
                  sizeof(filename) - 1);
    }
  } else {
    if (MimicConfig.FirstFile != MimicConfig.LastFile) {
      FATAL_ERROR("lhalo_hdf5 with multiple input files requires input.tree_name to include a "
                  "%%d file-number placeholder, e.g. trees_063.%%d.hdf5");
    }
    int filename_len = snprintf(filename, sizeof(filename), "%s", MimicConfig.TreeName);
    if (filename_len < 0 || (size_t)filename_len >= sizeof(filename)) {
      FATAL_ERROR("L-Halo HDF5 tree filename too long (%d chars, max %zu)", filename_len,
                  sizeof(filename) - 1);
    }
  }

  int path_len = snprintf(buf, size, "%s/%s", MimicConfig.SimulationDir, filename);
  if (path_len < 0 || (size_t)path_len >= size) {
    FATAL_ERROR("Tree file path too long (%d chars, max %zu)", path_len, size - 1);
  }
}

/** @brief Tree count of a present partition, from its /Header Ntrees alone. */
static int64_t count_partition_units_hdf5(int partition) {
  struct LHaloHDF5Header header;
  const hid_t file = open_lhalo_hdf5_header(tree_partition_per_file_output_id(partition),
                                            HDF5_HEADER_COUNTS, &header);
  if (H5Fclose(file) < 0) {
    FATAL_ERROR("Failed to close HDF5 tree file '%s'", header.path);
  }
  return (int64_t)header.ntrees;
}

/**
 * @brief   Largest tree in a present partition, from its /Header alone.
 *
 * Reads the InputTreeNHalos[Ntrees] header attribute into a transient buffer
 * released before returning; no tree group is opened.
 */
static int64_t max_partition_unit_halos_hdf5(int partition) {
  struct LHaloHDF5Header header;
  const hid_t file = open_lhalo_hdf5_header(tree_partition_per_file_output_id(partition),
                                            HDF5_HEADER_MAX, &header);
  if (H5Fclose(file) < 0) {
    FATAL_ERROR("Failed to close HDF5 tree file '%s'", header.path);
  }
  return header.max_halos;
}

/* L-Halo-tree HDF5 merger trees: per-tree groups (tree_NNN/<field>) with a
   /Header carrying Ntrees/totNHalos/InputTreeNHalos. One partition per input
   file, one unit per tree; see vertical/registry.c. */
const struct VerticalReader LHaloHDF5Reader = {
    .name = "lhalo_hdf5",
    .file_extension = "",
    .partition_model = PARTITION_PER_FILE,
    .processing_order = INPUT_PROCESSING_ORDER_VERTICAL,
    .num_partitions = tree_partition_per_file_count,
    .partition_output_id = tree_partition_per_file_output_id,
    .partition_exists = tree_partition_per_file_exists,
    .format_partition_path = format_lhalo_hdf5_partition_path,
    .count_partition_units = count_partition_units_hdf5,
    .max_partition_unit_halos = max_partition_unit_halos_hdf5,
    .open_partition = open_partition_hdf5,
    .load_unit = load_unit_hdf5,
    .close_partition = close_partition_hdf5,
};

/**
 * @brief   Read a native-int attribute whose element count must equal expected_count.
 * @param   file_path       Path of the open file, named in the error.
 * @param   expected_count  Required number of elements (1 for a scalar or 1-element attribute).
 * @return  EXIT_SUCCESS on success, -1 on any failure (logged with the file, attribute,
 *          expected and actual counts on a size mismatch).
 *
 * An unchecked H5Aread() writes whatever the attribute holds into the caller's buffer, so
 * a larger attribute overruns it and a smaller one leaves its tail uninitialised. This
 * reader checks the dataspace's element count first and reads nothing on a mismatch; a
 * failed close of the dataspace or attribute also fails the read.
 */
static int32_t read_attribute_int_checked(hid_t my_hdf5_file, const char *file_path,
                                          const char *groupname, const char *attr_name,
                                          int *attribute, int64_t expected_count) {
  int32_t result = -1;
  hid_t space_id = -1;
  hssize_t actual_count = -1;

  const hid_t attr_id =
      H5Aopen_by_name(my_hdf5_file, groupname, attr_name, H5P_DEFAULT, H5P_DEFAULT);
  if (attr_id < 0) {
    ERROR_LOG("Could not open the attribute %s in group %s of file '%s'", attr_name, groupname,
              file_path);
    return -1;
  }

  space_id = H5Aget_space(attr_id);
  if (space_id < 0) {
    ERROR_LOG("Could not get the dataspace of attribute %s in group %s of file '%s'", attr_name,
              groupname, file_path);
    goto checked_attribute_cleanup;
  }
  actual_count = H5Sget_simple_extent_npoints(space_id);
  if (actual_count < 0 || (int64_t)actual_count != expected_count) {
    ERROR_LOG("Attribute %s in group %s of file '%s' has %lld element(s); expected %lld", attr_name,
              groupname, file_path, (long long)actual_count, (long long)expected_count);
    goto checked_attribute_cleanup;
  }
  if (expected_count > 0 && H5Aread(attr_id, H5T_NATIVE_INT, attribute) < 0) {
    ERROR_LOG("Could not read the attribute %s in group %s of file '%s'", attr_name, groupname,
              file_path);
    goto checked_attribute_cleanup;
  }
  result = EXIT_SUCCESS;

checked_attribute_cleanup:
  if (space_id >= 0 && H5Sclose(space_id) < 0) {
    result = -1;
  }
  if (H5Aclose(attr_id) < 0) {
    ERROR_LOG("Could not close the attribute %s in group %s of file '%s'", attr_name, groupname,
              file_path);
    result = -1;
  }
  return result;
}

/**
 * @brief   Read a dataset from the currently open hdf5_file into buffer.
 * @return  EXIT_SUCCESS on success, negative HDF5 error code on failure.
 */
static int32_t read_dataset(char *dataset_name, enum ReadDatatype datatype, void *buffer) {
  hid_t dataset_id;

  dataset_id = H5Dopen2(hdf5_file, dataset_name, H5P_DEFAULT);
  if (dataset_id < 0) {
    ERROR_LOG("Error %d when trying to open dataset %s", dataset_id, dataset_name);
    return dataset_id;
  }

  herr_t status;
  if (datatype == READ_AS_INT) {
    status = H5Dread(dataset_id, H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer);
  } else if (datatype == READ_AS_FLOAT) {
    status = H5Dread(dataset_id, H5T_NATIVE_FLOAT, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer);
  } else if (datatype == READ_AS_LLONG) {
    status = H5Dread(dataset_id, H5T_NATIVE_LLONG, H5S_ALL, H5S_ALL, H5P_DEFAULT, buffer);
  } else {
    ERROR_LOG("Invalid datatype %d for dataset %s", datatype, dataset_name);
    H5Dclose(dataset_id);
    return -1;
  }

  if (status < 0) {
    ERROR_LOG("Failed to read dataset %s (error %d)", dataset_name, status);
    H5Dclose(dataset_id);
    return status;
  }

  H5Dclose(dataset_id);
  return EXIT_SUCCESS;
}
