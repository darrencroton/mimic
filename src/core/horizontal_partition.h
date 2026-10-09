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
 * 1. Stream every slab's `ForestIndex` column through a struct HorizontalPeakScan
 *    to verify the slab is forest-blocked and fold each forest's row count in it
 *    into the forest's weight, its **peak occupancy**: the largest row count it
 *    holds in any one slab. A range's summed peaks bound its rows in every slab,
 *    so a forest that is small in one slab but large in another is weighed by
 *    the slab where it is large.
 * 2. horizontal_partition_cut() turns the weights into the task cuts and then each
 *    task's chunk cuts, filling every entry of `forest_cuts`.
 * 3. For every slab, stream the `ForestIndex` column through a
 *    struct HorizontalForestScan to fill its `row_cuts` row.
 *
 * Failures that mean a defective input (a weight sum beyond int64_t, a forest
 * index outside the dataset's forest count, a scan misused) abort through
 * FATAL_ERROR; the one expected, reportable failure -- a slab that is not
 * forest-blocked -- is a return value of horizontal_peak_scan_end() (and of
 * horizontal_forest_scan_end()), with the first offending pair recorded in the
 * scan so the caller can name it.
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
 * @brief   Streaming state for one slab's peak-occupancy scan
 *
 * Plain data, held by the caller (a stack local is fine); initialise it with
 * horizontal_peak_scan_begin(). The slab's current forest and its run length so
 * far carry across blocks, so a forest whose rows straddle any number of blocks
 * is counted once, in full. Read the `violation_*` members after
 * horizontal_peak_scan_end() returns -1; they mean what they mean in
 * struct HorizontalForestScan.
 */
struct HorizontalPeakScan {
  int64_t *weights;
  int64_t n_forests_total;
  int64_t snapnum;
  int64_t rows_seen;
  int64_t previous;
  int64_t run;
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
 * @brief   Cut the forests into `ntask` task ranges, then each task range into `nchunk` chunks
 * @param   weights    `partition->n_forests_total` non-negative per-forest weights (may be NULL
 *                     when there are no forests)
 * @param   partition  Partition whose `forest_cuts` receive all `ntask * nchunk + 1` cuts
 *
 * Entry `t * nchunk` is task `t`'s first cut and range `t * nchunk + c` is task `t`'s
 * chunk `c`. The task cuts are those horizontal_partition_cut_forests() makes over all
 * forests with `ntask` ranges; each task's chunk makespan is then the minimum over all
 * contiguous `nchunk`-partitions of its range, trailing chunks are idle when fewer are
 * needed, and with `ntask == 1` the chunks are the ranges `nchunk` tasks would own.
 * `nchunk == 1` yields the task cuts alone. Aborts as horizontal_partition_cut_forests()
 * does.
 */
void horizontal_partition_cut(const int64_t *weights, struct HorizontalForestPartition *partition);

/**
 * @brief   Start scanning snapshot `snapnum`'s `ForestIndex` column for peak occupancies
 * @param   scan             State to initialise
 * @param   weights          `n_forests_total` per-forest peaks, zeroed by the caller before the
 *                           first slab and carried from slab to slab; never reset here
 * @param   n_forests_total  Number of forests
 * @param   snapnum          The slab's index, used in diagnostics only
 *
 * Scan every slab in turn, each with its own begin/visit/end, over the same
 * `weights`: after the last slab's end each entry is that forest's largest row
 * count in any one slab, 0 for a forest no slab holds.
 */
void horizontal_peak_scan_begin(struct HorizontalPeakScan *scan, int64_t *weights,
                                int64_t n_forests_total, int64_t snapnum);

/**
 * @brief   Feed the next block of the column
 * @param   scan       Scan begun with horizontal_peak_scan_begin()
 * @param   first_row  Slab row of `values[0]`; blocks must arrive in ascending, gapless order
 *                     (aborts otherwise)
 * @param   values     The block's `ForestIndex` values
 * @param   count      Number of values, at least 0
 *
 * Aborts (FATAL_ERROR) on a value outside `[0, n_forests_total)`. After the
 * first value smaller than its predecessor the scan records the violation and
 * ignores the rest of the column: a slab that is not forest-blocked has no
 * per-forest runs to measure.
 */
void horizontal_peak_scan_visit(struct HorizontalPeakScan *scan, int64_t first_row,
                                const int64_t *values, int64_t count);

/**
 * @brief   Finish the scan, folding the slab's last run into its forest's peak
 * @return  0 when the column was non-decreasing, so every forest's rows in it formed one run and
 *          each forest's peak now covers this slab (an empty column changes nothing). -1 when a
 *          value was smaller than its predecessor: the first such pair is in the scan's
 *          `violation_*` members and the weights are unspecified.
 */
int horizontal_peak_scan_end(struct HorizontalPeakScan *scan);

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
