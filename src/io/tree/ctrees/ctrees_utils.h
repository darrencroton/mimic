#ifndef IO_TREE_CTREES_UTILS_H
#define IO_TREE_CTREES_UTILS_H

/**
 * @file    tree/ctrees/ctrees_utils.h
 * @brief   Format-independent Consistent-Trees topology helpers.
 *
 * Ported from sage-model (io/ctrees_utils.{c,h}) with minimal edits so the code
 * stays easy to re-sync with upstream. The functions reconstruct L-Halo-tree
 * merger pointers from the Consistent-Trees `id`/`pid`/`upid`/`desc_id` columns:
 * read the forest/location index files, group trees into forests, fix flybys
 * and unbound-parent ids, then assign FirstHaloInFOFgroup / Descendant /
 * FirstProgenitor / NextProgenitor indices.
 *
 * They operate on the fixed ctrees-local `struct halo_data` (see ctrees_compat.h),
 * NOT on the per-simulation generated `struct RawHalo`; the reader bridges the
 * two at the package boundary.
 */

#include <stdint.h>

#include "tree/ctrees/ctrees_compat.h"

/* One row of the Consistent-Trees `locations.dat` index, annotated with the
   forest id resolved from `forests.list`. */
struct locations_with_forests {
  int64_t forestid;
  int64_t treeid;
  int64_t offset; /* byte offset of the tree's first data line within its file */
  int32_t fileid;
  int32_t unused; /* padding, kept for explicit alignment */
};

/* Open file descriptors for the `tree_*_*_*.dat` files, indexed by fileid. */
struct filenames_and_fd {
  int *fd;                     /* per-file descriptor (-1 until opened) */
  int32_t numfiles;            /* number of unique tree_*_*_*.dat files */
  uint32_t nallocated;         /* elements allocated for `fd` / `numtrees_per_file` */
  uint64_t *numtrees_per_file; /* number of trees in each tree_*_*_*.dat file */
};

/* Per-halo Consistent-Trees scratch used only during topology reconstruction
   (the values that are not carried into struct halo_data). */
struct additional_info {
  int64_t id;
  int64_t pid;
  int64_t upid;
  double desc_scale;
  int64_t descid;
  double scale;
};

int64_t read_forests(const char *filename, int64_t **forestids, int64_t **tree_rootids);
int64_t read_locations(const char *filename, const int64_t ntrees, struct locations_with_forests *l,
                       struct filenames_and_fd *filenames_and_fd);
int assign_forest_ids(const int64_t ntrees, struct locations_with_forests *locations,
                      int64_t *forests, int64_t *tree_roots);
void sort_locations_on_fid_file_offset(const int64_t ntrees,
                                       struct locations_with_forests *locations);
int fix_flybys(const int64_t totnhalos, struct halo_data *forest, struct additional_info *info,
               int verbose);
int fix_upid(const int64_t totnhalos, struct halo_data *forest, struct additional_info *info,
             const int verbose);
int assign_mergertree_indices(const int64_t totnhalos, struct halo_data *forest,
                              struct additional_info *info, const int max_snapnum);

#endif /* IO_TREE_CTREES_UTILS_H */
