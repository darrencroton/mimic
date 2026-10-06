#ifndef HORIZONTAL_PARTITION_H
#define HORIZONTAL_PARTITION_H

/**
 * @file    horizontal_partition.h
 * @brief   Forest-block partition of a horizontal dataset across ranks (no MPI, no I/O)
 *
 * A distributed horizontal run gives each rank a contiguous range of forests.
 * Because every snapshot slab is forest-blocked (`ForestIndex` non-decreasing
 * along the row order), those forests' rows form one contiguous row range in
 * every slab. This module holds the pure logic that turns the dataset's
 * per-forest weights into that decomposition; it makes no MPI call, reads no
 * file and never touches the driver, so the same code serves every rank count
 * and is testable against a brute-force oracle.
 *
 * The partition is two-level: the forests are cut into `ntask` contiguous task
 * ranges, then each task's range is cut again into `nchunk` contiguous chunks
 * that the task sweeps one after another. Both levels are cuts of the same
 * forest axis, so the tables hold `nranges = ntask * nchunk` ranges
 * (horizontal_partition_range_count()); range `t * nchunk + c` is task `t`'s
 * chunk `c`, and a run with `nchunk == 1` has exactly one range per task.
 *
 * Two tables describe a partition:
 *
 * - `forest_cuts[0 .. nranges]`: range `q` owns the forests `f` with
 *   `forest_cuts[q] <= f < forest_cuts[q + 1]`. `forest_cuts[0] == 0`,
 *   `forest_cuts[nranges] == n_forests_total`, and the cuts never decrease.
 *   Entry `t * nchunk` is task `t`'s first cut, so every `nchunk`-th entry is a
 *   task cut and the entries between two task cuts are that task's chunk cuts.
 * - `row_cuts[s * (nranges + 1) + q]`: the first row of snapshot slab `s` whose
 *   `ForestIndex >= forest_cuts[q]` (a lower bound), so range `q` owns the rows
 *   `[row_cuts[s][q], row_cuts[s][q + 1])` of slab `s` and
 *   `row_cuts[s][nranges]` is the slab's row count.
 *
 * Usage, in the order the distribution contract runs it:
 *
 * 1. Stream the widest slab's `ForestIndex` column through
 *    horizontal_partition_accumulate_weights() to get per-forest weights.
 * 2. horizontal_partition_cut_forests() turns the weights into the task cuts,
 *    `ntask + 1` of them written to `forest_cuts[0 .. ntask]`; then
 *    horizontal_partition_cut_chunks() moves each task cut to its entry
 *    `t * nchunk` and fills the chunk cuts inside every task's range (a no-op
 *    when `nchunk == 1`, where the two layouts coincide).
 * 3. For every slab, stream the `ForestIndex` column through a
 *    struct HorizontalForestScan to verify the slab is forest-blocked and fill
 *    its `row_cuts` row.
 *
 * Failures that mean a defective input (a weight sum beyond int64_t, a forest
 * index outside the dataset's forest count, a scan misused) abort through
 * FATAL_ERROR; the one expected, reportable failure -- a slab that is not
 * forest-blocked -- is a return value of horizontal_forest_scan_end(), with the
 * first offending pair recorded in the scan so the caller can name it.
 */

#include <stdint.h>

/**
 * @brief   The partition tables for one run
 *
 * Owned by the creator: allocate with horizontal_partition_create() and release
 * with horizontal_partition_destroy(). Both arrays are tracked MEM_HALOS
 * allocations, zero-filled at creation. `forest_cuts` has `ntask * nchunk + 1`
 * entries; `row_cuts` has `snapshot_count * (ntask * nchunk + 1)` entries,
 * row-major by snapshot. Entry `t * nchunk` of `forest_cuts` is task `t`'s
 * first cut and range `t * nchunk + c` is task `t`'s chunk `c`.
 */
struct HorizontalForestPartition {
  int ntask;
  int nchunk;
  int64_t snapshot_count;
  int64_t n_forests_total;
  int64_t *forest_cuts;
  int64_t *row_cuts;
};

/**
 * @brief   Streaming state for one slab's `ForestIndex` scan
 *
 * Plain data, held by the caller (a stack local is fine); initialise it with
 * horizontal_forest_scan_begin() and read the `violation_*` members after
 * horizontal_forest_scan_end() returns -1. `violation_row` is the slab row of
 * the first value smaller than its predecessor, `violation_previous` the
 * predecessor's value and `violation_value` the offending value.
 */
struct HorizontalForestScan {
  struct HorizontalForestPartition *partition;
  int64_t snapnum;
  int64_t rows_seen;
  int64_t previous;
  int next_cut;
  int violated;
  int64_t violation_row;
  int64_t violation_previous;
  int64_t violation_value;
};

/**
 * @brief   Number of ranges the tables describe, `ntask * nchunk`
 *
 * The stride of `row_cuts` is this plus one and the length of `forest_cuts` is
 * this plus one; every table index in the driver is a range index, `t * nchunk
 * + c` for task `t`'s chunk `c`. horizontal_partition_create() guarantees the
 * product (and the sum with one) fits an `int`.
 */
static inline int
horizontal_partition_range_count(const struct HorizontalForestPartition *partition) {
  return partition->ntask * partition->nchunk;
}

/**
 * @brief   Allocate a zero-filled partition for `ntask` ranks of `nchunk` chunks each
 * @param   ntask            Number of ranks, at least 1
 * @param   nchunk           Number of chunks each rank sweeps in turn, at least 1
 * @param   snapshot_count   Number of snapshot slabs, at least 0
 * @param   n_forests_total  The dataset's forest count, at least 0
 * @return  The partition; never NULL (an invalid argument or allocation failure aborts)
 *
 * Also aborts when a table size overflows `int64_t`, or when the range count plus
 * one or the row-cut count (`snapshot_count * (ntask * nchunk + 1)`) overflows
 * `int`, the latter because the row cuts travel in one `MPI_Bcast`.
 */
struct HorizontalForestPartition *
horizontal_partition_create(int ntask, int nchunk, int64_t snapshot_count, int64_t n_forests_total);

/** @brief  Release a partition and its tables; NULL is ignored. */
void horizontal_partition_destroy(struct HorizontalForestPartition *partition);

/**
 * @brief   Cut the forests into `ntask` contiguous ranges of minimum makespan
 * @param   weights          `n_forests_total` non-negative per-forest weights (may be NULL when
 *                           `n_forests_total == 0`)
 * @param   n_forests_total  Number of forests
 * @param   ntask            Number of ranges, at least 1
 * @param   forest_cuts      Receives `ntask + 1` cuts
 *
 * Finds the smallest capacity `M*` in `[max w_f, sum w_f]` for which a
 * left-to-right greedy packing (a range closes when the next forest would
 * exceed `M`) needs at most `ntask` ranges, by binary search, then takes that
 * packing as is. Hence the largest range weight is the minimum over all
 * contiguous partitions; a heavy forest shares its range with lighter
 * neighbours that fit under `M*`; zero-weight forests join the range being
 * filled and so never force a cut; and when fewer than `ntask` ranges are
 * needed the trailing ranges are idle (`forest_cuts[r] == n_forests_total`).
 * `ntask == 1` yields `[0, n_forests_total]` and `n_forests_total == 0` yields
 * all-zero cuts. The result is a pure function of the weights and `ntask`.
 *
 * Aborts (FATAL_ERROR) on a negative weight, on `ntask < 1`, and when the
 * weight sum would overflow int64_t.
 */
void horizontal_partition_cut_forests(const int64_t *weights, int64_t n_forests_total, int ntask,
                                      int64_t *forest_cuts);

/**
 * @brief   Cut each task's forest range into `nchunk` chunks of minimum makespan
 * @param   weights    `partition->n_forests_total` non-negative per-forest weights, the ones the
 *                     task cuts were made from (may be NULL when there are no forests)
 * @param   partition  Partition whose `forest_cuts[0 .. ntask]` hold the task cuts exactly as
 *                     horizontal_partition_cut_forests() wrote them; receives all
 *                     `ntask * nchunk + 1` cuts
 *
 * Moves task cut `t` to entry `t * nchunk`, then for every task `t` applies
 * horizontal_partition_cut_forests() to the weights of the task's forests with
 * `nchunk` ranges and offsets the result into place, so the chunk cuts nest
 * inside the task cuts and every task cut keeps its value. A task's chunk
 * makespan is the minimum over all contiguous `nchunk`-partitions of its range,
 * trailing chunks are idle when fewer are needed, and with `ntask == 1` the
 * chunks are the ranges `nchunk` tasks would own. `nchunk == 1` changes nothing.
 * One-shot transform, not idempotent: call it exactly once, directly after
 * horizontal_partition_cut_forests() has written the compact task cuts; a second
 * call on the expanded table would treat the chunk cuts as task cuts. It takes a
 * const partition but writes the `forest_cuts` table the partition points to.
 * Aborts as horizontal_partition_cut_forests() does.
 */
void horizontal_partition_cut_chunks(const int64_t *weights,
                                     const struct HorizontalForestPartition *partition);

/**
 * @brief   Add one to a forest's weight for each streamed `ForestIndex` value
 * @param   weights          `n_forests_total` counters, zeroed by the caller before the first call
 * @param   n_forests_total  Number of forests
 * @param   values           A block of `ForestIndex` values
 * @param   count            Number of values in the block
 *
 * Call once per block of the widest slab's column; the result is each forest's
 * halo count in that slab. Aborts (FATAL_ERROR) on a value outside
 * `[0, n_forests_total)`.
 */
void horizontal_partition_accumulate_weights(int64_t *weights, int64_t n_forests_total,
                                             const int64_t *values, int64_t count);

/**
 * @brief   Start scanning snapshot `snapnum`'s `ForestIndex` column
 * @param   scan       State to initialise
 * @param   partition  Partition whose `forest_cuts` are final and whose `row_cuts` row for
 *                     `snapnum` is to be filled
 * @param   snapnum    Slab index, in `[0, partition->snapshot_count)`; aborts otherwise
 */
void horizontal_forest_scan_begin(struct HorizontalForestScan *scan,
                                  struct HorizontalForestPartition *partition, int64_t snapnum);

/**
 * @brief   Feed the next block of the column
 * @param   scan       Scan begun with horizontal_forest_scan_begin()
 * @param   first_row  Slab row of `values[0]`; blocks must arrive in ascending, gapless order
 *                     (aborts otherwise)
 * @param   values     The block's `ForestIndex` values, each in `[0, n_forests_total)` (the
 *                     reader guarantees this; it is not rechecked here)
 * @param   count      Number of values, at least 0
 *
 * Block boundaries are invisible: the previous value carries across calls, so
 * a forest's run may straddle any number of blocks. After the first violation
 * further blocks are accepted and ignored.
 */
void horizontal_forest_scan_visit(struct HorizontalForestScan *scan, int64_t first_row,
                                  const int64_t *values, int64_t count);

/**
 * @brief   Finish the scan
 * @return  0 when the column was non-decreasing: `row_cuts[snapnum][0 .. nranges]` then holds the
 *          lower bound of each `forest_cuts[q]` in the column, with `row_cuts[snapnum][nranges]`
 *          the number of rows seen (an empty column gives all zeros). -1 when a value was
 *          smaller than its predecessor: the first such pair is in the scan's `violation_*`
 *          members and the `row_cuts` row for `snapnum` is unspecified.
 */
int horizontal_forest_scan_end(struct HorizontalForestScan *scan);

#endif /* HORIZONTAL_PARTITION_H */
