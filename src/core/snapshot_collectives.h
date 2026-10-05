#ifndef SNAPSHOT_COLLECTIVES_H
#define SNAPSHOT_COLLECTIVES_H

/**
 * @file    snapshot_collectives.h
 * @brief   Whole-population operations for process_snapshot modules across tasks
 *
 * A distributed horizontal run gives each task only its own forests' part of
 * every snapshot, so a process_snapshot callback sees a partial population.
 * A module that needs a whole-population quantity -- a global rank, a total, an
 * extent, a failure agreed by every task -- obtains it through these
 * collectives and declares `snapshot_distribution: collective` in its
 * module_info.yaml (enum SnapshotDistribution in module_interface.h).
 *
 * Return kinds:
 * - Status functions (module_snapshot_rank(), module_snapshot_sum_i64(),
 *   module_snapshot_sum_f64(), module_snapshot_min_max_f64()) return 0 on
 *   success and -1 on error. Under several tasks every task returns the same
 *   status.
 * - module_snapshot_any() returns 1 or 0 for the logical or of every task's
 *   flag, and -1 on error.
 * - module_snapshot_is_root_task() returns 1 on task 0 (and in a serial run)
 *   and 0 elsewhere; it cannot fail.
 *
 * Where they may be called. Every function except module_snapshot_is_root_task()
 * is refused (ERROR_LOG, -1) while a module's init(), a process_full_halo,
 * process_per_event or process_by_galaxy call, or a module's cleanup() is
 * running, because those callbacks are not synchronised across tasks. They are
 * allowed during snapshot dispatch (execute_post_snapshot()) and when no
 * callback is running, so a unit test may call a process_snapshot entry point
 * directly.
 *
 * Rules for module authors:
 * - Every task reaches every collective, in the same order, with the same `n`.
 *   Branch only on snapshot-level facts (the context) or on a previous
 *   collective's result, never on a local count: a task holding no galaxies
 *   still makes every call (with `count == 0` for the rank).
 * - module_snapshot_rank() requires ids unique within the caller's keys (checked
 *   on every task) and across tasks (the caller's precondition; UniqueGalaxyID
 *   gives it by construction because forests are disjoint).
 *
 * Serial runs (NTask <= 1) and non-MPI builds implement every function as the
 * identity: the rank is the position in the local sort, the reductions leave
 * their inputs unchanged and module_snapshot_any() returns `flag != 0`. Under
 * MPI the rank is an exact sample-sort whose result depends only on the
 * multiset of keys, so it is the same for every task count;
 * module_snapshot_sum_f64() is identical on every task of one run but may differ
 * from a serial sum in the last bits (reduction order).
 *
 * Scratch memory is tracked under MEM_UTILITY and freed before each call returns.
 */

#include <stdint.h>

#include "module_interface.h"

/**
 * @brief   One key ranked by module_snapshot_rank()
 *
 * Keys are ordered by descending `value`, ties broken by ascending `id`, so
 * rank 0 is the largest value. `value` must not be NaN; `id` must be unique.
 */
struct SnapshotRankKey {
  double value;
  int64_t id;
};

/**
 * @brief   Exact global rank of every key across all tasks
 *
 * On success, ranks[i] is the number of keys, over every task's keys, that
 * precede keys[i] in the order (value descending, id ascending); the ranks
 * across all tasks are therefore exactly 0 .. total - 1. In a serial run that
 * is the position of keys[i] in the sorted local keys.
 *
 * Errors (every task returns -1 when any task meets one; @p ranks is left
 * unchanged): refused callback kind, `count < 0`, NULL @p keys or @p ranks with
 * a positive count, a NaN value, a duplicate id within the caller's keys, and,
 * under MPI, a count or received bucket beyond INT_MAX or a duplicate key met
 * inside a bucket.
 *
 * @param   ctx    Snapshot context of the calling callback (not read; may be NULL)
 * @param   keys   The caller's keys (may be NULL when @p count is 0)
 * @param   count  Number of keys (>= 0)
 * @param   ranks  Output, @p count entries (may be NULL when @p count is 0)
 * @return  0 on success, -1 on error
 */
int module_snapshot_rank(const struct SnapshotContext *ctx, const struct SnapshotRankKey *keys,
                         int64_t count, int64_t *ranks);

/**
 * @brief   Sum each of @p n int64 values over all tasks, in place
 *
 * @param   ctx     Snapshot context of the calling callback (not read; may be NULL)
 * @param   values  In: this task's values; out: the sums (may be NULL when @p n is 0)
 * @param   n       Number of values (>= 0, equal on every task)
 * @return  0 on success, -1 on error (refused, invalid arguments, unequal @p n)
 */
int module_snapshot_sum_i64(const struct SnapshotContext *ctx, int64_t *values, int n);

/**
 * @brief   Sum each of @p n double values over all tasks, in place
 *
 * The result is identical on every task of one run; it may differ from a serial
 * sum of the same values in the last bits (reduction order).
 *
 * NaN-free input is the caller's precondition, like cross-task id uniqueness
 * for module_snapshot_rank(): no NaN check is made, and with a NaN input the
 * result is unspecified and may differ between tasks, so a module branching on
 * it would desynchronise the tasks. Exclude NaN before reducing.
 *
 * @param   ctx     Snapshot context of the calling callback (not read; may be NULL)
 * @param   values  In: this task's values; out: the sums (may be NULL when @p n is 0)
 * @param   n       Number of values (>= 0, equal on every task)
 * @return  0 on success, -1 on error (refused, invalid arguments, unequal @p n)
 */
int module_snapshot_sum_f64(const struct SnapshotContext *ctx, double *values, int n);

/**
 * @brief   Element-wise minimum of @p minima and maximum of @p maxima over all tasks
 *
 * NaN-free input is the caller's precondition, like cross-task id uniqueness
 * for module_snapshot_rank(): no NaN check is made, and with a NaN input the
 * result is unspecified and may differ between tasks (MPI_MIN and MPI_MAX do
 * not define NaN). Exclude NaN before reducing, e.g. by seeding the local
 * extent with +/-INFINITY and folding values in with fmin()/fmax().
 *
 * @param   ctx     Snapshot context of the calling callback (not read; may be NULL)
 * @param   minima  In: this task's values; out: the minima (may be NULL when @p n is 0)
 * @param   maxima  In: this task's values; out: the maxima (may be NULL when @p n is 0)
 * @param   n       Entries in each array (>= 0, equal on every task)
 * @return  0 on success, -1 on error (refused, invalid arguments, unequal @p n)
 */
int module_snapshot_min_max_f64(const struct SnapshotContext *ctx, double *minima, double *maxima,
                                int n);

/**
 * @brief   Logical or of every task's flag (for agreeing on a failure)
 *
 * @param   ctx   Snapshot context of the calling callback (not read; may be NULL)
 * @param   flag  This task's flag (any non-zero value is true)
 * @return  1 if any task's flag is non-zero, 0 if none is, -1 if refused
 */
int module_snapshot_any(const struct SnapshotContext *ctx, int flag);

/**
 * @brief   Whether this process is the root task (task 0)
 *
 * Use it to emit a whole-population log line once. Allowed in every callback
 * kind and outside any callback; it makes no collective call.
 *
 * @return  1 in a serial run (NTask <= 1) or a non-MPI build and on task 0 of a
 *          distributed run, 0 on every other task
 */
int module_snapshot_is_root_task(void);

#endif /* SNAPSHOT_COLLECTIVES_H */
