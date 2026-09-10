#ifndef IO_TREE_CTREES_UTILS_H
#define IO_TREE_CTREES_UTILS_H

/**
 * @file    tree/ctrees/ctrees_utils.h
 * @brief   Format-independent Consistent-Trees topology helpers.
 *
 * Ported from sage-model (io/ctrees_utils.{c,h}) with minimal edits so the code
 * stays easy to re-sync with upstream. The functions reconstruct L-Halo-tree
 * merger pointers from the Consistent-Trees `id`/`pid`/`upid`/`desc_id` columns:
 * read the forest/location index files, group trees into forests, fix
 * unbound-parent ids, then assign FirstHaloInFOFgroup / Descendant /
 * FirstProgenitor / NextProgenitor indices.
 *
 * They operate on the fixed ctrees-local `struct halo_data` (see ctrees_compat.h),
 * NOT on the per-simulation generated `struct RawHalo`; the reader bridges the
 * two at the package boundary.
 *
 * `verify_fof_centrals_present()` is Mimic-native, not ported: it restores,
 * standalone, a corrupt-input guard that used to be a side effect of the
 * now-removed `fix_flybys()` (docs/dev/SHIN-UCHUU-FLYBY-DEFECT-ADDENDUM.md,
 * decision D9(c)).
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

/**
 * @brief   Assert that a forest has at least one FoF central at its maximum scale.
 *
 * A Consistent-Trees forest is rooted at its maximum scale factor by
 * construction, so the set of halos at that scale can legitimately contain
 * one FoF central (`pid == -1`), or many once a forest percolates across
 * independent FoF groups — but it can never legitimately contain zero. The
 * now-removed `fix_flybys()` enforced this as a side effect of the flyby
 * demotion it performed; this function restores the corrupt-input guard on
 * its own, with no demotion or topology-rewriting behaviour of any kind.
 *
 * @param totnhalos Number of halos in the forest.
 * @param info      Parallel Consistent-Trees scratch info (`scale`, `pid`);
 *                  need not be pre-sorted by scale.
 * @param unit      Forest/tree-file identifier, used only for the diagnostic
 *                  message.
 * @return EXIT_SUCCESS if at least one `pid == -1` halo exists at the
 *         forest's maximum scale; -1, after logging an ERROR diagnostic
 *         naming @p unit, if none does (corrupt input) — including the
 *         degenerate case `totnhalos <= 0`, which has none by construction
 *         and must not be treated as vacuously valid.
 */
int verify_fof_centrals_present(const int64_t totnhalos, const struct additional_info *info,
                                const int unit);

int fix_upid(const int64_t totnhalos, struct halo_data *forest, struct additional_info *info,
             const int verbose);
int assign_mergertree_indices(const int64_t totnhalos, struct halo_data *forest,
                              struct additional_info *info, const int max_snapnum);

/**
 * @brief   Run the reader's full Consistent-Trees topology pipeline: the
 *          corrupt-input guard, upid resolution, then FoF/mergertree index
 *          assignment, in the exact order `read_ctrees_ascii.c` uses.
 *
 * Shared by the reader and by `tests/unit/test_ctrees_support.c` so a
 * regression that inserts a step between these three calls (such as a
 * reintroduced `fix_flybys()`) is exercised by the same unit test that
 * covers the reader's own sequence, rather than only by tests that call
 * `fix_upid`/`assign_mergertree_indices` directly.
 *
 * @param totnhalos Number of halos in the forest.
 * @param forest    L-Halo-shaped records to fill with merger-tree indices.
 * @param info      Parallel Consistent-Trees scratch info.
 * @param unit      Forest/tree-file identifier, used only for diagnostics.
 * @return The max snapshot number (>= 0) on success; -1 if any stage failed
 *         (each stage logs its own diagnostic before returning).
 */
int ctrees_apply_topology(const int64_t totnhalos, struct halo_data *forest,
                          struct additional_info *info, const int unit);

#endif /* IO_TREE_CTREES_UTILS_H */
