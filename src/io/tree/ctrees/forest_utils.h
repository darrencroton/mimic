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
 * Unwired in Phase 4: compiled and unit-tested in isolation; the Phase-5 ctrees
 * reader uses these during its per-task setup.
 */

#include <stdint.h>

#include "tree/ctrees/ctrees_compat.h"

int distribute_forests_over_ntasks(const int64_t totnforests, const int NTasks, const int ThisTask,
                                   int64_t *nforests_thistask, int64_t *start_forestnum_thistask);

int distribute_weighted_forests_over_ntasks(
    const int64_t totnforests, const int64_t *nhalos_per_forest,
    const enum Valid_Forest_Distribution_Schemes forest_weighting, const double power_law_index,
    const int NTasks, const int ThisTask, int64_t *nforests_thistask,
    int64_t *start_forestnum_thistask);

int find_start_and_end_filenum(const int64_t start_forestnum, const int64_t end_forestnum,
                               const int64_t *totnforests_per_file, const int64_t totnforests,
                               const int firstfile, const int lastfile, const int ThisTask,
                               const int NTasks, int64_t *num_forests_to_process_per_file,
                               int64_t *start_forestnum_to_process_per_file, int *start_file,
                               int *end_file);

#endif /* IO_TREE_CTREES_FOREST_UTILS_H */
