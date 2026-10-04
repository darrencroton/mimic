/**
 * @file    vertical/binary.c
 * @brief   Functions for reading binary merger tree files
 *
 * This file implements functionality for loading merger trees from
 * binary format files. It handles the reading of tree metadata and
 * halo data for individual trees, providing an interface to the core
 * Mimic code that is independent of the specific file format.
 *
 * Binary format trees are the traditional binary input format, consisting
 * of a simple structure with tree counts, halo counts, and arrays of
 * halo data. This format is efficient to read but less flexible than
 * newer formats like HDF5.
 *
 * Key functions:
 * - open_lhalo_binary_header(): The one parser of the per-file header, shared by
 *   the count, largest-tree and open hooks
 * - open_partition_binary(): Reads tree metadata from a binary file
 * - load_unit_binary(): Loads a specific tree's halo data
 * - close_partition_binary(): Closes the binary file
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
#include "proto.h"
#include "globals.h"
#include "vertical/interface.h"
#include "vertical/binary.h"
#include "vertical/reader.h"
#include "types.h"
#include "error.h"

static FILE *load_fd;

#ifndef MAX_BUF_SIZE
#define MAX_BUF_SIZE (3 * MAX_STRING_LEN + 40)
#endif

/* Halo counts read per fread() while parsing the InputTreeNHalos table: a bounded
 * stack block, so a scan that keeps no table allocates nothing however many trees
 * a file holds. */
#define BINARY_TREE_COUNT_BLOCK 4096

/* How much of the per-file header open_lhalo_binary_header() parses. */
enum LHaloBinaryHeaderScope {
  BINARY_HEADER_COUNTS, /* Ntrees and totNHalos only */
  BINARY_HEADER_MAX,    /* plus the table, reduced to its largest entry */
  BINARY_HEADER_TABLE,  /* plus the table, kept for load_unit_binary() */
};

/* What open_lhalo_binary_header() parsed; fields beyond the scope stay zero or NULL. */
struct LHaloBinaryHeader {
  int ntrees;
  int tot_nhalos;
  int64_t max_halos; /* largest InputTreeNHalos entry; 0 for a file with no trees */
  int *tree_nhalos;  /* MEM_TREES block of ntrees counts (BINARY_HEADER_TABLE only) */
};

/**
 * @brief   Open a binary partition file and parse its header.
 * @param   output_id   Output id of the partition (the L-Halo filenr).
 * @param   scope       How much of the header to parse.
 * @param   header      Receives what was parsed.
 * @return  The open file, positioned after the InputTreeNHalos table when the scope
 *          reads it and after the two header ints otherwise. The caller closes it and,
 *          for BINARY_HEADER_TABLE, owns header->tree_nhalos.
 *
 * Reads the legacy headerless layout (host endianness): Ntrees, totNHalos, then
 * InputTreeNHalos[Ntrees]. The table is read in bounded blocks, straight into the
 * kept table when there is one. A short read, a negative Ntrees or a negative table
 * entry is FATAL, after the file is closed and any table released.
 */
static FILE *open_lhalo_binary_header(int output_id, enum LHaloBinaryHeaderScope scope,
                                      struct LHaloBinaryHeader *header) {
  char buf[MAX_BUF_SIZE + 1];
  int counts[BINARY_TREE_COUNT_BLOCK];

  *header = (struct LHaloBinaryHeader){0, 0, 0, NULL};
  snprintf(buf, MAX_BUF_SIZE, "%s/%s.%d%s", MimicConfig.SimulationDir, MimicConfig.TreeName,
           output_id, MimicConfig.TreeExtension);

  FILE *fd = fopen(buf, "r");
  if (fd == NULL) {
    FATAL_ERROR("Failed to open binary tree file '%s' (filenr %d)", buf, output_id);
  }

  /* Parse with the handle open, then report: every failure closes it exactly once. */
  enum { SCAN_OK, SCAN_BAD_HEADER, SCAN_NEGATIVE_NTREES, SCAN_SHORT_TABLE, SCAN_NEGATIVE_COUNT };
  int scan_status = SCAN_OK;
  int bad_tree = -1, bad_count = 0;

  if (fread(&header->ntrees, sizeof(int), 1, fd) != 1 ||
      fread(&header->tot_nhalos, sizeof(int), 1, fd) != 1) {
    scan_status = SCAN_BAD_HEADER;
  } else if (header->ntrees < 0) {
    scan_status = SCAN_NEGATIVE_NTREES;
  } else if (scope == BINARY_HEADER_TABLE) {
    header->tree_nhalos = mymalloc_cat(sizeof(int) * (size_t)header->ntrees, MEM_TREES);
  }

  const int table_entries = scope == BINARY_HEADER_COUNTS ? 0 : header->ntrees;
  for (int done = 0; scan_status == SCAN_OK && done < table_entries;) {
    const int block = table_entries - done < BINARY_TREE_COUNT_BLOCK ? table_entries - done
                                                                     : BINARY_TREE_COUNT_BLOCK;
    int *dest = header->tree_nhalos != NULL ? header->tree_nhalos + done : counts;
    if (fread(dest, sizeof(int), (size_t)block, fd) != (size_t)block) {
      scan_status = SCAN_SHORT_TABLE;
      break;
    }
    for (int i = 0; i < block; i++) {
      if (dest[i] < 0) {
        scan_status = SCAN_NEGATIVE_COUNT;
        bad_tree = done + i;
        bad_count = dest[i];
        break;
      }
      if (dest[i] > header->max_halos) {
        header->max_halos = dest[i];
      }
    }
    done += block;
  }

  if (scan_status == SCAN_OK) {
    return fd;
  }

  fclose(fd);
  myfree(header->tree_nhalos);
  header->tree_nhalos = NULL;
  switch (scan_status) {
  case SCAN_BAD_HEADER:
    FATAL_ERROR("Failed to read the Ntrees/totNHalos header from file '%s'", buf);
    break;
  case SCAN_NEGATIVE_NTREES:
    FATAL_ERROR("Binary tree file '%s' reports negative Ntrees=%d", buf, header->ntrees);
    break;
  case SCAN_SHORT_TABLE:
    FATAL_ERROR("Failed to read tree halo counts from file '%s'", buf);
    break;
  default:
    FATAL_ERROR("Binary tree file '%s' reports negative halo count %d for tree %d", buf, bad_count,
                bad_tree);
    break;
  }
  return NULL;
}

/** @brief Tree count of a present partition, from its header's Ntrees alone. */
static int64_t count_partition_units_binary(int partition) {
  struct LHaloBinaryHeader header;
  FILE *fd = open_lhalo_binary_header(tree_partition_per_file_output_id(partition),
                                      BINARY_HEADER_COUNTS, &header);
  fclose(fd);
  return (int64_t)header.ntrees;
}

/**
 * @brief   Largest tree in a present partition, from its header alone.
 *
 * Scans the InputTreeNHalos table that precedes the halo records in bounded
 * blocks, allocates nothing, and never touches a halo row.
 */
static int64_t max_partition_unit_halos_binary(int partition) {
  struct LHaloBinaryHeader header;
  FILE *fd = open_lhalo_binary_header(tree_partition_per_file_output_id(partition),
                                      BINARY_HEADER_MAX, &header);
  fclose(fd);
  return header.max_halos;
}

/**
 * @brief   Open binary partition file and read its tree-count header.
 * @param   output_id   Output id of the partition (the L-Halo filenr).
 *
 * Stages Ntrees and InputTreeNHalos[Ntrees], builds InputTreeFirstHalo[], and
 * leaves the file open at the first halo record for load_unit_binary() calls.
 */
void open_partition_binary(int output_id) {
  struct LHaloBinaryHeader header;

  load_fd = open_lhalo_binary_header(output_id, BINARY_HEADER_TABLE, &header);
  Ntrees = header.ntrees;
  InputTreeNHalos = header.tree_nhalos;
  DEBUG_LOG("Reading %d trees with %d total halos", Ntrees, header.tot_nhalos);

  InputTreeFirstHalo = mymalloc_cat(sizeof(int) * Ntrees, MEM_TREES);
  if (Ntrees > 0) {
    InputTreeFirstHalo[0] = 0;
    for (int i = 1; i < Ntrees; i++)
      InputTreeFirstHalo[i] = InputTreeFirstHalo[i - 1] + InputTreeNHalos[i - 1];
  }
}

/**
 * @brief   Load one tree's halo data from the open binary file.
 * @param   unit   Tree index within the open partition.
 */
void load_unit_binary(int unit) {

  assert(load_fd);

  InputTreeHalos = mymalloc_cat(sizeof(struct RawHalo) * InputTreeNHalos[unit], MEM_TREES);
  if (fread(InputTreeHalos, sizeof(struct RawHalo), InputTreeNHalos[unit], load_fd) !=
      (size_t)InputTreeNHalos[unit]) {
    FATAL_ERROR("Failed to read halo data for tree %d", unit);
  }
}

/** @brief Close the open binary partition file. */
void close_partition_binary(void) {
  if (load_fd) {
    fclose(load_fd);
    load_fd = NULL;
  }
}

/* L-Halo binary merger trees: the traditional headerless binary catalog. One
   partition per input file, one unit per tree; see vertical/registry.c. */
const struct VerticalReader LHaloBinaryReader = {
    .name = "lhalo_binary",
    .file_extension = "",
    .partition_model = PARTITION_PER_FILE,
    .processing_order = INPUT_PROCESSING_ORDER_VERTICAL,
    .num_partitions = tree_partition_per_file_count,
    .partition_output_id = tree_partition_per_file_output_id,
    .partition_exists = tree_partition_per_file_exists,
    .format_partition_path = NULL,
    .count_partition_units = count_partition_units_binary,
    .max_partition_unit_halos = max_partition_unit_halos_binary,
    .open_partition = open_partition_binary,
    .load_unit = load_unit_binary,
    .close_partition = close_partition_binary,
};
