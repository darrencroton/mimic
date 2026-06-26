#ifndef IO_TREE_READER_H
#define IO_TREE_READER_H

#include <stddef.h>
#include <stdint.h>

/**
 * @file    tree/reader.h
 * @brief   Format-agnostic merger-tree reader interface.
 *
 * Each supported input format registers exactly one TreeReader (see
 * registry.c). The core dispatches through these function pointers instead of
 * switching on a format enum, so adding a format is a single registry entry
 * plus its implementation file -- no edits to the core read path.
 *
 * The core driver iterates the input as *partitions* of *units*. A partition is
 * the unit of output: the core opens one set of output files per partition,
 * names them by the partition's output id, and finalizes them when the
 * partition is done. A unit is one independently processed merger structure
 * within a partition.
 *
 * Two partition models are supported (see the driver loop in core/tree_driver.c):
 *
 * - PARTITION_PER_FILE: a partition is one input file and a unit is one tree.
 *   The driver strides partitions across MPI tasks; the per-file helpers below
 *   supply the partition enumeration (num_partitions / partition_output_id).
 *   Both L-Halo readers use this model.
 *
 * - PARTITION_ENUMERATED: the reader publishes a global list of output
 *   partitions independent of MPI task count. The driver assigns partitions to
 *   tasks with deterministic LPT load balancing, processes each task's assigned
 *   partitions in ascending partition id, and obtains existence, counts,
 *   offsets, and costs from the reader hooks below.
 */
enum TreePartitionModel {
  PARTITION_PER_FILE = 0,   /* one partition per input file (L-Halo) */
  PARTITION_ENUMERATED = 1, /* reader-enumerated output partitions */
};

enum InputProcessingOrder {
  INPUT_PROCESSING_ORDER_TREE = 0,
  INPUT_PROCESSING_ORDER_SNAPSHOT = 1,
};

static inline const char *input_processing_order_name(enum InputProcessingOrder order) {
  switch (order) {
  case INPUT_PROCESSING_ORDER_TREE:
    return "tree_ordered";
  case INPUT_PROCESSING_ORDER_SNAPSHOT:
    return "snapshot_ordered";
  }
  return "unknown";
}

struct TreeReader {
  const char *name; /* tree_type string in the input YAML */
  /* Reader-owned filename suffix copied into MimicConfig.TreeExtension. Only
     readers with a fixed filename suffix should set this; readers whose
     tree_name is a literal filename or filename pattern leave it empty. */
  const char *file_extension;

  /* How the driver maps partitions onto the input (see enum above). */
  enum TreePartitionModel partition_model;

  /* Processing-order driver this reader feeds. Current readers are tree ordered. */
  enum InputProcessingOrder processing_order;

  /* Optional run-scoped lifecycle hooks. Readers that keep no run-scoped state
     leave these NULL. The driver calls prepare_run once before any partition
     opens and teardown_run once after the partition loop, including idle ranks. */
  void (*prepare_run)(void);
  void (*teardown_run)(void);

  /* PARTITION_PER_FILE and PARTITION_ENUMERATED readers: */
  /* Number of partitions to iterate. The driver applies the MPI stride over the
     partition index, so this returns the full count, not a per-task share. */
  int (*num_partitions)(void);
  /* Output id for a partition: the value used in output file names. Galaxy ids
     use the run-scoped GlobalForestOffset plus unit index instead. */
  int (*partition_output_id)(int partition);
  /* Required existence check for a partition. Per-file readers check input files;
     enumerated readers check reader-defined chunks. */
  int (*partition_exists)(int partition);
  /* Optional PARTITION_PER_FILE path formatter. If NULL, the driver uses the
     legacy L-Halo binary path: <simulation_dir>/<tree_name>.<output_id><ext>.
     Readers whose tree_name is a literal filename or filename pattern should
     provide this rather than relying on file_extension. */
  void (*format_partition_path)(char *buf, size_t size, int output_id);
  /* Count units in a present partition without staging per-unit metadata or
     holding an open handle. Required for PARTITION_PER_FILE readers so the core
     can build run-scoped global forest offsets; required for PARTITION_ENUMERATED
     readers so serial progress can span assigned chunks. */
  int64_t (*count_partition_units)(int partition);
  /* Global forest offset for a reader-enumerated partition. Required for
     PARTITION_ENUMERATED readers; ignored for other models. */
  int64_t (*global_forest_offset)(int partition);
  /* Processing cost for a reader-enumerated partition. Required for
     PARTITION_ENUMERATED readers and consumed by chunk_plan_assign_lpt(). */
  double (*partition_cost)(int partition);

  /* Open partition `output_id` and read its unit table (Ntrees and per-unit
     halo counts), retaining the open handle for subsequent load_unit calls.
     Enumerated readers receive their partition output id here after the driver
     has published the partition's GlobalForestOffset. */
  void (*open_partition)(int output_id);

  /* Load every halo of one unit from the open partition into InputTreeHalos. */
  void (*load_unit)(int unit);

  /* Close the open partition (and release any per-partition scaffolding). */
  void (*close_partition)(void);
};

/**
 * @brief   Resolve a tree_type string to its reader.
 * @param   name  tree_type value from the input YAML.
 * @return  The matching reader, or NULL if no format with that name is
 *          registered in this build (readers needing HDF5 are absent from
 *          non-HDF5 builds).
 */
const struct TreeReader *tree_reader_lookup(const char *name);

/**
 * Per-file partition model shared by the L-Halo readers: one partition per
 * input file across the configured FirstFile..LastFile range, with the output
 * id equal to the file number. Defined in tree/interface.c.
 */
int tree_partition_per_file_count(void);
int tree_partition_per_file_output_id(int partition);
int tree_partition_per_file_exists(int partition);

#endif /* IO_TREE_READER_H */
