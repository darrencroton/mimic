#ifndef IO_TREE_CTREES_FOREST_UTILS_H
#define IO_TREE_CTREES_FOREST_UTILS_H

/**
 * @file    tree/ctrees/forest_utils.h
 * @brief   Forest-to-MPI-task distribution for Consistent-Trees.
 *
 * Ported from sage-model (io/forest_utils.{c,h}) with minimal edits. Splits the
 * global list of forests across MPI tasks either uniformly or weighted by a
 * per-forest cost (halo count raised to a configurable power), and maps a task's
 * forest range back onto the input files it must read.
 *
 * The Consistent-Trees readers use these during per-task setup; the ASCII reader
 * uses uniform splitting, while the HDF5 reader can weight by per-forest halo
 * count.
 */

#include <stdint.h>

#include "tree/ctrees/ctrees_compat.h"
#include "tree/forest_distribution.h"

int distribute_forests_over_ntasks(const int64_t totnforests, const int NTasks, const int ThisTask,
                                   int64_t *nforests_thistask, int64_t *start_forestnum_thistask);

int distribute_weighted_forests_over_ntasks(const int64_t totnforests,
                                            const int64_t *nhalos_per_forest,
                                            const enum ForestDistributionScheme forest_weighting,
                                            const double power_law_index, const int NTasks,
                                            const int ThisTask, int64_t *nforests_thistask,
                                            int64_t *start_forestnum_thistask);

double compute_forest_cost_from_nhalos(const enum ForestDistributionScheme forest_weighting,
                                       int64_t nhalos, double exponent);

int find_start_and_end_filenum(const int64_t start_forestnum, const int64_t end_forestnum,
                               const int64_t *totnforests_per_file, const int64_t totnforests,
                               const int firstfile, const int lastfile, const int ThisTask,
                               const int NTasks, int64_t *num_forests_to_process_per_file,
                               int64_t *start_forestnum_to_process_per_file, int *start_file,
                               int *end_file);

#endif /* IO_TREE_CTREES_FOREST_UTILS_H */
