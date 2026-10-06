/**
 * @file    horizontal_partition.c
 * @brief   Forest-block partition of a horizontal dataset across ranks
 *
 * See horizontal_partition.h for the contract. Nothing here calls MPI or reads a
 * file: the functions are pure over the arrays handed to them, so every rank
 * that computes the partition from the same weights gets the same cuts.
 */

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "error.h"
#include "horizontal_partition.h"
#include "memory.h"

/**
 * Number of ranges the left-to-right greedy packing needs at capacity `capacity`.
 *
 * A range closes when the next forest would push it past the capacity, so a
 * zero-weight forest always joins the open range. `capacity` is at least the
 * largest weight, so no single forest overflows an empty range, and at most the
 * weight sum (checked by the caller). The fit test compares against the room
 * left, `capacity - filled`, so it cannot overflow however large the weights.
 * When `forest_cuts` is non-NULL the interior cuts are recorded as the ranges
 * close, `forest_cuts[1 .. ranges - 1]`; the caller owns the remaining entries.
 */
static int greedy_pack(const int64_t *weights, int64_t n_forests_total, int64_t capacity,
                       int64_t *forest_cuts) {
  int ranges = 1;
  int64_t filled = 0;

  for (int64_t f = 0; f < n_forests_total; f++) {
    if (weights[f] > capacity - filled) { /* filled <= capacity, so this cannot overflow */
      if (forest_cuts != NULL) {
        forest_cuts[ranges] = f;
      }
      ranges++;
      filled = 0;
    }
    filled += weights[f];
  }
  return ranges;
}

struct HorizontalForestPartition *horizontal_partition_create(int ntask, int nchunk,
                                                              int64_t snapshot_count,
                                                              int64_t n_forests_total) {
  if (ntask < 1) {
    FATAL_ERROR("Horizontal partition needs at least one rank (got ntask = %d)", ntask);
  }
  if (nchunk < 1) {
    FATAL_ERROR("Horizontal partition needs at least one chunk per rank (got nchunk = %d)", nchunk);
  }
  if (snapshot_count < 0 || n_forests_total < 0) {
    FATAL_ERROR("Horizontal partition counts must not be negative (snapshot_count = %" PRId64
                ", n_forests_total = %" PRId64 ")",
                snapshot_count, n_forests_total);
  }

  /* ntask and nchunk are ints, so their product cannot overflow int64_t. */
  const int64_t ranges = (int64_t)ntask * nchunk;
  const int64_t entries_per_snapshot = ranges + 1;
  if (entries_per_snapshot > INT_MAX) {
    FATAL_ERROR("Horizontal partition has too many ranges (%d tasks x %d chunks)", ntask, nchunk);
  }
  if (snapshot_count > (int64_t)(SIZE_MAX / sizeof(int64_t)) / entries_per_snapshot) {
    FATAL_ERROR("Horizontal partition row-cut table is too large (%" PRId64 " snapshots x %" PRId64
                " entries)",
                snapshot_count, entries_per_snapshot);
  }
  if (snapshot_count > INT_MAX / entries_per_snapshot) {
    FATAL_ERROR("Horizontal partition row-cut table exceeds one MPI broadcast (%" PRId64
                " snapshots x %" PRId64 " entries)",
                snapshot_count, entries_per_snapshot);
  }

  struct HorizontalForestPartition *partition =
      mymalloc_cat(sizeof(*partition), MEM_HALOS); /* tracked; freed by _destroy */
  partition->ntask = ntask;
  partition->nchunk = nchunk;
  partition->snapshot_count = snapshot_count;
  partition->n_forests_total = n_forests_total;

  const size_t forest_bytes = (size_t)entries_per_snapshot * sizeof(int64_t);
  const size_t row_bytes = (size_t)(snapshot_count * entries_per_snapshot) * sizeof(int64_t);
  partition->forest_cuts = mymalloc_cat(forest_bytes, MEM_HALOS);
  partition->row_cuts = mymalloc_cat(row_bytes, MEM_HALOS);
  memset(partition->forest_cuts, 0, forest_bytes);
  memset(partition->row_cuts, 0, row_bytes);
  return partition;
}

void horizontal_partition_destroy(struct HorizontalForestPartition *partition) {
  if (partition == NULL) {
    return;
  }
  myfree(partition->row_cuts);
  myfree(partition->forest_cuts);
  myfree(partition);
}

void horizontal_partition_cut_forests(const int64_t *weights, int64_t n_forests_total, int ntask,
                                      int64_t *forest_cuts) {
  if (ntask < 1) {
    FATAL_ERROR("Horizontal partition needs at least one rank (got ntask = %d)", ntask);
  }
  if (n_forests_total < 0 || (n_forests_total > 0 && weights == NULL)) {
    FATAL_ERROR("Horizontal partition has no weights for %" PRId64 " forests", n_forests_total);
  }

  int64_t heaviest = 0;
  int64_t total = 0;
  for (int64_t f = 0; f < n_forests_total; f++) {
    const int64_t weight = weights[f];
    if (weight < 0) {
      FATAL_ERROR("Horizontal partition weight of forest %" PRId64 " is negative (%" PRId64 ")", f,
                  weight);
    }
    if (weight > INT64_MAX - total) {
      FATAL_ERROR("Horizontal partition weights sum beyond int64_t at forest %" PRId64, f);
    }
    total += weight;
    if (weight > heaviest) {
      heaviest = weight;
    }
  }

  /* Smallest capacity at which the greedy packing needs at most ntask ranges.
   * Capacity `total` always fits in one range, so the search interval is valid. */
  int64_t low = heaviest;
  int64_t high = total;
  while (low < high) {
    const int64_t middle = low + (high - low) / 2;
    if (greedy_pack(weights, n_forests_total, middle, NULL) <= ntask) {
      high = middle;
    } else {
      low = middle + 1;
    }
  }

  forest_cuts[0] = 0;
  const int ranges = greedy_pack(weights, n_forests_total, low, forest_cuts);
  for (int r = ranges; r <= ntask; r++) {
    forest_cuts[r] = n_forests_total; /* the last used range's end, then the idle ranges */
  }
}

void horizontal_partition_cut_chunks(const int64_t *weights,
                                     const struct HorizontalForestPartition *partition) {
  const int ntask = partition->ntask;
  const int nchunk = partition->nchunk;
  int64_t *forest_cuts = partition->forest_cuts;

  if (nchunk == 1) {
    return; /* the task cuts are already the only range level */
  }

  /* Spread the compact task cuts [0 .. ntask] to entries t * nchunk. Walking down, every
   * write lands at or above the entry still to be read (t * nchunk >= t), so none is lost. */
  for (int t = ntask; t >= 1; t--) {
    forest_cuts[(int64_t)t * nchunk] = forest_cuts[t];
  }

  for (int t = 0; t < ntask; t++) {
    int64_t *task_cuts = forest_cuts + (int64_t)t * nchunk;
    const int64_t forest_lo = task_cuts[0];
    const int64_t forest_hi = forest_cuts[((int64_t)t + 1) * nchunk];
    const int64_t count = forest_hi - forest_lo;

    /* Writes entries [0 .. nchunk] of the task: the first is 0 and the last the sub-range's
     * length, so offsetting restores both task cuts exactly. */
    horizontal_partition_cut_forests(count > 0 ? weights + forest_lo : NULL, count, nchunk,
                                     task_cuts);
    for (int c = 0; c <= nchunk; c++) {
      task_cuts[c] += forest_lo;
    }
  }
}

void horizontal_partition_accumulate_weights(int64_t *weights, int64_t n_forests_total,
                                             const int64_t *values, int64_t count) {
  for (int64_t i = 0; i < count; i++) {
    const int64_t forest = values[i];
    if (forest < 0 || forest >= n_forests_total) {
      FATAL_ERROR("ForestIndex %" PRId64 " is outside [0, %" PRId64 ") (value %" PRId64
                  " of the streamed block)",
                  forest, n_forests_total, i);
    }
    weights[forest]++;
  }
}

void horizontal_forest_scan_begin(struct HorizontalForestScan *scan,
                                  struct HorizontalForestPartition *partition, int64_t snapnum) {
  if (snapnum < 0 || snapnum >= partition->snapshot_count) {
    FATAL_ERROR("Forest scan snapshot %" PRId64 " is outside [0, %" PRId64 ")", snapnum,
                partition->snapshot_count);
  }
  memset(scan, 0, sizeof(*scan));
  scan->partition = partition;
  scan->snapnum = snapnum;
}

void horizontal_forest_scan_visit(struct HorizontalForestScan *scan, int64_t first_row,
                                  const int64_t *values, int64_t count) {
  if (first_row != scan->rows_seen) {
    FATAL_ERROR("Forest scan of snapshot %" PRId64 " expected the block to start at row %" PRId64
                " but it starts at row %" PRId64,
                scan->snapnum, scan->rows_seen, first_row);
  }
  scan->rows_seen += count;
  if (scan->violated) {
    return;
  }

  const struct HorizontalForestPartition *partition = scan->partition;
  const int nranges = horizontal_partition_range_count(partition);
  const int64_t *forest_cuts = partition->forest_cuts;
  int64_t *row_cuts = partition->row_cuts + scan->snapnum * ((int64_t)nranges + 1);

  for (int64_t i = 0; i < count; i++) {
    const int64_t value = values[i];
    const int64_t row = first_row + i;

    if (row > 0 && value < scan->previous) {
      scan->violated = 1;
      scan->violation_row = row;
      scan->violation_previous = scan->previous;
      scan->violation_value = value;
      return;
    }
    scan->previous = value;

    /* The column is non-decreasing so far, so the first row at or past each cut is
     * the first row whose value reaches it; cuts are non-decreasing too, so one
     * forward pointer resolves them all. The final cut, n_forests_total, is
     * resolved at the end: it is the number of rows. */
    while (scan->next_cut < nranges && value >= forest_cuts[scan->next_cut]) {
      row_cuts[scan->next_cut++] = row;
    }
  }
}

int horizontal_forest_scan_end(struct HorizontalForestScan *scan) {
  if (scan->violated) {
    return -1;
  }

  const int nranges = horizontal_partition_range_count(scan->partition);
  int64_t *row_cuts = scan->partition->row_cuts + scan->snapnum * ((int64_t)nranges + 1);

  /* Cuts no row reached lie past every value: their lower bound is the row count. */
  while (scan->next_cut <= nranges) {
    row_cuts[scan->next_cut++] = scan->rows_seen;
  }
  return 0;
}
