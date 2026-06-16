#ifndef IO_TREE_READER_H
#define IO_TREE_READER_H

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
 * within a partition. For the L-Halo readers a partition is one input file and
 * a unit is one tree, and the per-file helpers below supply the partition
 * enumeration. Because that enumeration comes from the reader, a reader can map
 * partitions onto the input differently without touching the core loop.
 */
struct TreeReader {
  const char *name;           /* tree_type string in the input YAML */
  const char *file_extension; /* appended after TreeName.<output_id>, e.g. ".hdf5" */

  /* Number of partitions to iterate. The driver applies the MPI stride over the
     partition index, so this returns the full count, not a per-task share. */
  int (*num_partitions)(void);

  /* Output id for a partition: the value used in output file names and unique
     galaxy ids (the filenr-equivalent). */
  int (*partition_output_id)(int partition);

  /* Open partition `output_id` and read its unit table (Ntrees and per-unit
     halo counts), retaining the open handle for subsequent load_unit calls. */
  void (*open_partition)(int output_id);

  /* Load every halo of one unit from the open partition into InputTreeHalos. */
  void (*load_unit)(int unit);

  /* Close the open partition. */
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

#endif /* IO_TREE_READER_H */
