/**
 * @file    io_tree.c
 * @brief   Functions for loading and managing merger trees
 *
 * This file contains the core functionality for loading merger trees from
 * various file formats, managing the tree data in memory, and preparing
 * output files for halo data. It serves as a central hub for different
 * tree file formats (binary, HDF5) and handles the allocation/deallocation
 * of tree-related data structures.
 *
 * Key functions:
 * - load_tree_table(): Loads tree metadata from input files
 * - load_tree(): Loads a specific merger tree into memory
 * - free_tree_table(): Frees memory allocated for tree metadata
 * - free_halos_and_tree(): Cleans up halo and tree data structures
 *
 * The code supports different tree formats through a plugin architecture,
 * with format-specific implementations in the io/ directory.
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

#include "allvars.h"
#include "proto.h"
#include "tree/interface.h"
#include "util.h"
#include "error.h"

#include "tree/binary.h"
#ifdef HDF5
#include "output/hdf5.h"
#include "tree/hdf5.h"
#endif

/* Global file endianness variable - initialized to host endianness by default
 */
static int file_endianness = MIMIC_HOST_ENDIAN;

#ifndef MAX_BUF_SIZE
#define MAX_BUF_SIZE (3 * MAX_STRING_LEN + 40)
#endif

/**
 * @brief   Loads merger tree metadata and prepares output files
 *
 * @param   filenr       Current file number being processed
 * @param   my_TreeType  Type of merger tree format to load
 *
 * This function loads the table of merger trees from the specified file
 * and initializes data structures for processing these trees. It:
 *
 * 1. Calls the appropriate format-specific loader based on tree type
 * 2. Allocates memory for tracking objects per tree for each output snapshot
 * 3. Creates empty output files for each requested snapshot
 * 4. Initializes halo counters
 *
 * The function supports different tree formats (binary, HDF5) through a
 * dispatching mechanism to format-specific implementations.
 */
void load_tree_table(int filenr, enum Valid_TreeTypes my_TreeType) {
  int i, n;
  FILE *fd;
  char buf[MAX_BUF_SIZE + 1];

  switch (my_TreeType) {
#ifdef HDF5
  case genesis_lhalo_hdf5:
    load_tree_table_hdf5(filenr);
    break;
#endif

  case lhalo_binary:
    load_tree_table_binary(filenr);
    break;

  default:
    FATAL_ERROR("Unsupported tree type %d in load_tree_table(). Please add "
                "support in core_io_tree.c",
                my_TreeType);
  }

  for (n = 0; n < NOUT; n++) {
    InputHalosPerSnap[n] = mymalloc_cat(sizeof(int) * Ntrees, MEM_TREES);
    if (InputHalosPerSnap[n] == NULL) {
      FATAL_ERROR(
          "Memory allocation failed for InputHalosPerSnap[%d] array (%d trees, "
          "%zu bytes)",
          n, Ntrees, Ntrees * sizeof(int));
    }

    for (i = 0; i < Ntrees; i++)
      InputHalosPerSnap[n][i] = 0;

    TotHalosPerSnap[n] = 0;
  }

  /* Create output files based on format */
#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    /* For HDF5, create one file per filenr with all snapshots */
    snprintf(buf, MAX_BUF_SIZE, "%s/%s_%03d.hdf5", MimicConfig.OutputDir,
             MimicConfig.OutputFileBaseName, filenr);
    prep_hdf5_file(buf);

    /* Open the file and keep it open for fast writes */
    HDF5_current_file_id = H5Fopen(buf, H5F_ACC_RDWR, H5P_DEFAULT);
    if (HDF5_current_file_id < 0) {
      FATAL_ERROR("Failed to open HDF5 file '%s' for writing", buf);
    }
    DEBUG_LOG("HDF5 file '%s' opened with ID %lld", buf,
              (long long)HDF5_current_file_id);
  } else {
    /* For binary, create one file per snapshot per filenr */
    for (n = 0; n < NOUT; n++) {
      snprintf(buf, MAX_BUF_SIZE, "%s/%s_z%1.3f_%d", MimicConfig.OutputDir,
               MimicConfig.OutputFileBaseName, ZZ[ListOutputSnaps[n]], filenr);

      if (!(fd = fopen(buf, "w"))) {
        FATAL_ERROR("Failed to create output halo file '%s' for snapshot %d "
                    "(filenr %d)",
                    buf, ListOutputSnaps[n], filenr);
      }
      fclose(fd);
    }
  }
#else
  /* Binary format only (no HDF5 support) */
  for (n = 0; n < NOUT; n++) {
    snprintf(buf, MAX_BUF_SIZE, "%s/%s_z%1.3f_%d", MimicConfig.OutputDir,
             MimicConfig.OutputFileBaseName, ZZ[ListOutputSnaps[n]], filenr);

    if (!(fd = fopen(buf, "w"))) {
      FATAL_ERROR("Failed to create output halo file '%s' for snapshot %d "
                  "(filenr %d)",
                  buf, ListOutputSnaps[n], filenr);
    }
    fclose(fd);
  }
#endif
}

/**
 * @brief   Frees memory allocated for the merger tree table
 *
 * @param   my_TreeType  Type of merger tree format being used
 *
 * This function releases all memory allocated for the merger tree metadata.
 * It frees:
 *
 * 1. Arrays tracking objects per tree for each output snapshot
 * 2. The array of first halo indices for each tree
 * 3. The array of halo counts per tree
 * 4. Format-specific resources (e.g., closing file handles)
 *
 * The function ensures proper cleanup of resources after processing
 * is complete, preventing memory leaks.
 */
void free_tree_table(enum Valid_TreeTypes my_TreeType) {
  int n;

  for (n = NOUT - 1; n >= 0; n--) {
    myfree(InputHalosPerSnap[n]);
    InputHalosPerSnap[n] = NULL;
  }

  myfree(InputTreeFirstHalo);
  InputTreeFirstHalo = NULL;

  myfree(InputTreeNHalos);
  InputTreeNHalos = NULL;

  // Don't forget to free the open file handle

  switch (my_TreeType) {
#ifdef HDF5
  case genesis_lhalo_hdf5:
    close_hdf5_file();
    break;
#endif

  case lhalo_binary:
    close_binary_file();
    break;

  default:
    FATAL_ERROR("Unsupported tree type %d in free_tree_table(). Please add "
                "support in core_io_tree.c",
                my_TreeType);
  }
}

/**
 * @brief   Loads a specific merger tree into memory
 *
 * @param   filenr       Current file number being processed
 * @param   treenr       Index of the tree to load
 * @param   my_TreeType  Type of merger tree format to load
 *
 * This function loads a single merger tree from the input file and allocates
 * memory for processing its halos. It:
 *
 * 1. Calls the appropriate format-specific loader based on tree type
 * 2. Calculates the maximum number of objects for this tree
 * 3. Allocates memory for halo auxiliary data
 * 4. Allocates memory for halo data structures
 * 5. Initializes the halo auxiliary data
 *
 * The memory allocation is proportional to the number of halos in the tree,
 * ensuring efficient memory usage while providing sufficient space for the
 * objects that will be created during processing.
 */
void load_tree(int treenr, enum Valid_TreeTypes my_TreeType) {
  int32_t i;

  switch (my_TreeType) {

#ifdef HDF5
  case genesis_lhalo_hdf5:
    load_tree_hdf5(treenr);
    break;
#endif
  case lhalo_binary:
    load_tree_binary(treenr);
    break;

  default:
    FATAL_ERROR("Unsupported tree type %d in load_tree(). Please add support "
                "in core_io_tree.c",
                my_TreeType);
  }

  /* Calculate MaxProcessedHalos based on number of halos with a sensible
   * minimum */
  MaxProcessedHalos = (int)(MAXHALOFAC * InputTreeNHalos[treenr]);
  if (MaxProcessedHalos < MIN_HALO_ARRAY_GROWTH)
    MaxProcessedHalos = MIN_HALO_ARRAY_GROWTH;

  /* Start with a reasonable size for MaxFoFWorkspace based on tree
   * characteristics
   */
  MaxFoFWorkspace = INITIAL_FOF_HALOS;
  if ((int)(0.1 * MaxProcessedHalos) > MaxFoFWorkspace)
    MaxFoFWorkspace = (int)(0.1 * MaxProcessedHalos);

  HaloAux = mymalloc_cat(sizeof(struct HaloAuxData) * InputTreeNHalos[treenr], MEM_HALOS);
  if (HaloAux == NULL) {
    FATAL_ERROR(
        "Memory allocation failed for HaloAux array (%d halos, %zu bytes)",
        InputTreeNHalos[treenr],
        InputTreeNHalos[treenr] * sizeof(struct HaloAuxData));
  }

  ProcessedHalos = mymalloc_cat(sizeof(struct Halo) * MaxProcessedHalos, MEM_HALOS);
  if (ProcessedHalos == NULL) {
    FATAL_ERROR("Memory allocation failed for ProcessedHalos array (%d halos, "
                "%zu bytes)",
                MaxProcessedHalos, MaxProcessedHalos * sizeof(struct Halo));
  }

  FoFWorkspace = mymalloc_cat(sizeof(struct Halo) * MaxFoFWorkspace, MEM_HALOS);
  if (FoFWorkspace == NULL) {
    FATAL_ERROR(
        "Memory allocation failed for FoFWorkspace array (%d halos, %zu bytes)",
        MaxFoFWorkspace, MaxFoFWorkspace * sizeof(struct Halo));
  }

  for (i = 0; i < InputTreeNHalos[treenr]; i++) {
    HaloAux[i].DoneFlag = 0;
    HaloAux[i].HaloFlag = 0;
    HaloAux[i].NHalos = 0;
  }
}

/**
 * @brief   Frees memory allocated for the current merger tree
 *
 * This function releases all memory allocated for halo and halo data
 * structures after a merger tree has been processed. It frees:
 *
 * 1. The temporary halo array used during processing (Gal)
 * 2. The permanent halo array for output (HaloGal)
 * 3. The halo auxiliary data array (HaloAux)
 * 4. The halo data array (Halo)
 *
 * This cleanup is performed after each tree is fully processed, allowing
 * the memory to be reused for the next tree.
 */
void free_halos_and_tree(void) {
  myfree(FoFWorkspace);
  myfree(ProcessedHalos);
  myfree(HaloAux);
  myfree(InputTreeHalos);
}

/**
 * @brief   Set the endianness for binary file operations
 *
 * @param   endianness    The endianness to use (MIMIC_LITTLE_ENDIAN or
 * MIMIC_BIG_ENDIAN)
 *
 * This function sets the global endianness value used for all binary file
 * operations. It's typically called after detecting the endianness of a file.
 * The functions myfread and myfwrite will use this value to perform any
 * necessary byte swapping.
 */
void set_file_endianness(int endianness) {
  if (endianness != MIMIC_LITTLE_ENDIAN && endianness != MIMIC_BIG_ENDIAN) {
    WARNING_LOG("Invalid endianness value %d. Using host endianness (%d).",
                endianness, MIMIC_HOST_ENDIAN);
    file_endianness = MIMIC_HOST_ENDIAN;
  } else {
    file_endianness = endianness;
  }
}

/**
 * @brief   Get the current file endianness setting
 *
 * @return  Current file endianness (MIMIC_LITTLE_ENDIAN or MIMIC_BIG_ENDIAN)
 *
 * This function returns the global endianness value currently used for
 * binary file operations.
 */
int get_file_endianness(void) { return file_endianness; }

/**
 * @brief   Wrapper for fread with endianness handling
 *
 * @param   ptr      Pointer to the data buffer
 * @param   size     Size of each element
 * @param   nmemb    Number of elements to read
 * @param   stream   File stream to read from
 * @return  Number of elements successfully read
 *
 * This function provides a wrapper around the standard fread function
 * with endianness conversion and error handling.
 */
size_t myfread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
  size_t items_read;

  if (ptr == NULL || stream == NULL) {
    IO_ERROR_LOG(IO_ERROR_READ_FAILED, "myfread", NULL,
                 "NULL pointer passed to myfread");
    return 0;
  }

  /* Use standard library fread */
  items_read = fread(ptr, size, nmemb, stream);

  /* Perform endianness conversion if needed */
  if (items_read > 0 && !is_same_endian(file_endianness)) {
    /* Only swap if element size is appropriate */
    if (size == 2 || size == 4 || size == 8) {
      swap_bytes_if_needed(ptr, size, items_read, file_endianness);
    }
  }

  return items_read;
}

/**
 * @brief   Wrapper for fwrite with endianness handling
 *
 * @param   ptr      Pointer to the data buffer
 * @param   size     Size of each element
 * @param   nmemb    Number of elements to write
 * @param   stream   File stream to write to
 * @return  Number of elements successfully written
 *
 * This function provides a wrapper around the standard fwrite function
 * with endianness conversion and error handling.
 */
size_t myfwrite(void *ptr, size_t size, size_t nmemb, FILE *stream) {
  void *tmp_buffer = NULL;
  size_t items_written;

  if (ptr == NULL || stream == NULL) {
    IO_ERROR_LOG(IO_ERROR_WRITE_FAILED, "myfwrite", NULL,
                 "NULL pointer passed to myfwrite");
    return 0;
  }

  /* Create a clean copy of the data for writing */
  tmp_buffer = malloc(size * nmemb);
  if (tmp_buffer == NULL) {
    WARNING_LOG("Failed to allocate temporary buffer for write operation");
    return 0;
  }

  /* First zero the buffer to avoid any garbage data */
  memset(tmp_buffer, 0, size * nmemb);

  /* Then copy the data to ensure clean transfer */
  memcpy(tmp_buffer, ptr, size * nmemb);

  /* Perform endianness conversion if needed */
  if (!is_same_endian(file_endianness) &&
      (size == 2 || size == 4 || size == 8)) {
    /* Swap bytes in temporary buffer */
    swap_bytes_if_needed(tmp_buffer, size, nmemb, file_endianness);
  }

  /* Use standard library fwrite */
  items_written = fwrite(tmp_buffer, size, nmemb, stream);

  /* Free temporary buffer */
  free(tmp_buffer);

  return items_written;
}

/**
 * @brief   Wrapper for fseek with error handling
 *
 * @param   stream   File stream to seek within
 * @param   offset   Offset from the position specified by whence
 * @param   whence   Position from which to seek (SEEK_SET, SEEK_CUR, SEEK_END)
 * @return  0 on success, non-zero on error
 *
 * This function provides a wrapper around the standard fseek function
 * with error handling.
 */
int myfseek(FILE *stream, long offset, int whence) {
  int result;

  if (stream == NULL) {
    IO_ERROR_LOG(IO_ERROR_SEEK_FAILED, "myfseek", NULL,
                 "NULL stream pointer passed to myfseek");
    return -1;
  }

  /* Use standard library fseek */
  result = fseek(stream, offset, whence);
  if (result != 0) {
    IO_ERROR_LOG(IO_ERROR_SEEK_FAILED, "myfseek", NULL,
                 "fseek failed with error %d", result);
  }
  return result;
}
