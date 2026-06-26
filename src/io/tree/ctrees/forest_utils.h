#ifndef IO_TREE_CTREES_FOREST_UTILS_H
#define IO_TREE_CTREES_FOREST_UTILS_H

/**
 * @file    tree/ctrees/forest_utils.h
 * @brief   Forest cost and range helpers for Consistent-Trees.
 *
 * Ported from sage-model (io/forest_utils.{c,h}) with minimal edits. Provides
 * the legacy forest-distribution routines still covered by unit tests, the
 * per-forest cost helper used for chunk LPT assignment, and helpers that map a
 * forest range back onto the input files it must read.
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
