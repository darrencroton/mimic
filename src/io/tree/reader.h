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
 */
struct TreeReader {
  const char *name;           /* tree_type string in the input YAML */
  const char *file_extension; /* appended after TreeName.<filenr>, e.g. ".hdf5" */

  /* Read tree-table metadata for one input file (Ntrees and per-tree halo
     counts) and retain the open file handle for subsequent load_tree calls. */
  void (*load_tree_table)(int filenr);

  /* Load every halo of one tree from the open file into InputTreeHalos. */
  void (*load_tree)(int treenr);

  /* Close the open input file. */
  void (*close_file)(void);
};

/**
 * @brief   Resolve a tree_type string to its reader.
 * @param   name  tree_type value from the input YAML.
 * @return  The matching reader, or NULL if no format with that name is
 *          registered in this build (readers needing HDF5 are absent from
 *          non-HDF5 builds).
 */
const struct TreeReader *tree_reader_lookup(const char *name);

#endif /* IO_TREE_READER_H */
