/**
 * @file    test_horizontal_partition.c
 * @brief   Unit tests for the forest-block partition logic of the distributed horizontal driver
 *
 * horizontal_partition_cut_forests() is checked against a brute-force oracle that
 * enumerates EVERY contiguous partition (every non-decreasing tuple of cuts,
 * empty ranges included) of every weight vector of length 0..8 over the weights
 * {0, 1, 2, 5} for 1..4 ranks, so the minimum makespan is established without
 * reference to the greedy rule under test. A second, independently written
 * greedy packer taken at the oracle's optimum pins the exact cuts D3 prescribes.
 * The streaming scan is checked against a binary-search lower bound on columns
 * delivered in blocks of varying size, and against hand-built cases for zero-weight
 * forests, idle ranks, decreasing columns and empty columns.
 */

#include "../../src/core/horizontal_partition.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"
#include "../framework/child_capture.h"
#include "../framework/test_framework.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

#define MAX_FORESTS 8
#define MAX_TASKS 4
#define WEIGHT_CHOICES 4

static const int64_t WEIGHT_VALUES[WEIGHT_CHOICES] = {0, 1, 2, 5};

/* ------------------------------------------------------------------------- */
/* Oracle and reference helpers                                              */
/* ------------------------------------------------------------------------- */

/**
 * Brute-force optimum: the smallest possible largest-range weight over EVERY way
 * of placing `ranges_left - 1` cuts at or after `start` (non-decreasing, so empty
 * ranges are enumerated too). `prefix[i]` is the weight of forests [0, i).
 */
static int64_t oracle_best_makespan(const int64_t *prefix, int n, int start, int ranges_left) {
  if (ranges_left == 1) {
    return prefix[n] - prefix[start];
  }
  int64_t best = INT64_MAX;
  for (int end = start; end <= n; end++) {
    const int64_t here = prefix[end] - prefix[start];
    const int64_t rest = oracle_best_makespan(prefix, n, end, ranges_left - 1);
    const int64_t worst = here > rest ? here : rest;
    if (worst < best) {
      best = worst;
    }
  }
  return best;
}

/** Largest range weight of a cut vector. */
static int64_t cuts_makespan(const int64_t *prefix, const int64_t *cuts, int ntask) {
  int64_t worst = 0;
  for (int r = 0; r < ntask; r++) {
    const int64_t weight = prefix[cuts[r + 1]] - prefix[cuts[r]];
    if (weight > worst) {
      worst = weight;
    }
  }
  return worst;
}

/**
 * The D3 greedy packing at a given capacity, written independently of the code
 * under test: ranges are built explicitly, one forest at a time. Cuts beyond the
 * last range are n (idle ranges).
 */
static void reference_greedy_cuts(const int64_t *weights, int n, int ntask, int64_t capacity,
                                  int64_t *cuts) {
  int range = 0;
  int64_t filled = 0;
  cuts[0] = 0;
  for (int f = 0; f < n; f++) {
    if (filled + weights[f] > capacity) {
      range++;
      cuts[range] = f;
      filled = 0;
    }
    filled += weights[f];
  }
  for (int r = range + 1; r <= ntask; r++) {
    cuts[r] = n;
  }
}

/** First index in the non-decreasing column whose value is >= `x`, or `n`. */
static int64_t lower_bound(const int64_t *column, int64_t n, int64_t x) {
  int64_t low = 0;
  int64_t high = n;
  while (low < high) {
    const int64_t middle = low + (high - low) / 2;
    if (column[middle] < x) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  return low;
}

/** Expand per-forest weights into the column they would be counted from. */
static int64_t build_column(const int64_t *weights, int n, int64_t *column) {
  int64_t rows = 0;
  for (int f = 0; f < n; f++) {
    for (int64_t k = 0; k < weights[f]; k++) {
      column[rows++] = f;
    }
  }
  return rows;
}

/** Decode `code` into a weight vector of length `n` over WEIGHT_VALUES. */
static void decode_weights(int64_t code, int n, int64_t *weights) {
  for (int i = 0; i < n; i++) {
    weights[i] = WEIGHT_VALUES[code % WEIGHT_CHOICES];
    code /= WEIGHT_CHOICES;
  }
}

static int64_t power_of_choices(int n) {
  int64_t count = 1;
  for (int i = 0; i < n; i++) {
    count *= WEIGHT_CHOICES;
  }
  return count;
}

static void print_vector(const char *label, const int64_t *values, int count) {
  fprintf(stderr, "  %s = [", label);
  for (int i = 0; i < count; i++) {
    fprintf(stderr, "%s%" PRId64, i > 0 ? ", " : "", values[i]);
  }
  fprintf(stderr, "]\n");
}

/** Feed `column` to the scan in blocks whose (positive) sizes cycle through `sizes`. */
static void scan_in_blocks(struct HorizontalForestScan *scan, const int64_t *column, int64_t rows,
                           const int64_t *sizes, int n_sizes) {
  int64_t row = 0;
  int which = 0;
  while (row < rows) {
    int64_t take = sizes[which++ % n_sizes];
    if (take > rows - row) {
      take = rows - row;
    }
    horizontal_forest_scan_visit(scan, row, column + row, take);
    row += take;
  }
}

/** Check one weight vector and rank count against the oracle; 1 when everything agrees. */
static int check_cut_vector(const int64_t *weights, int n, int ntask) {
  int64_t prefix[MAX_FORESTS + 1];
  int64_t cuts[MAX_TASKS + 1];
  int64_t reference[MAX_TASKS + 1];

  prefix[0] = 0;
  for (int i = 0; i < n; i++) {
    prefix[i + 1] = prefix[i] + weights[i];
  }
  for (int r = 0; r <= ntask; r++) {
    cuts[r] = -1; /* the function must write every entry */
  }

  horizontal_partition_cut_forests(weights, n, ntask, cuts);

  const int64_t optimum = oracle_best_makespan(prefix, n, 0, ntask);
  int ok = cuts[0] == 0 && cuts[ntask] == n;
  for (int r = 0; ok && r < ntask; r++) {
    ok = cuts[r] <= cuts[r + 1];
  }
  ok = ok && cuts_makespan(prefix, cuts, ntask) == optimum;

  reference_greedy_cuts(weights, n, ntask, optimum, reference);
  for (int r = 0; ok && r <= ntask; r++) {
    ok = cuts[r] == reference[r];
  }

  if (!ok) {
    fprintf(stderr, "  cut_forests disagrees with the oracle (ntask = %d, optimum = %" PRId64 ")\n",
            ntask, optimum);
    print_vector("weights", weights, n);
    print_vector("cuts", cuts, ntask + 1);
    print_vector("greedy at optimum", reference, ntask + 1);
  }
  return ok;
}

/**
 * Every rank's rows must hold exactly the forests its range names: row_cuts is
 * monotone, brackets [0, rows], and each row's value lies in its rank's forest range.
 */
static int rows_agree_with_forest_ranges(const struct HorizontalForestPartition *partition,
                                         int64_t snapnum, const int64_t *column, int64_t rows) {
  const int64_t *row_cuts = partition->row_cuts + snapnum * (partition->ntask + 1);
  if (row_cuts[0] != 0 || row_cuts[partition->ntask] != rows) {
    return 0;
  }
  for (int r = 0; r < partition->ntask; r++) {
    if (row_cuts[r] > row_cuts[r + 1]) {
      return 0;
    }
    for (int64_t i = row_cuts[r]; i < row_cuts[r + 1]; i++) {
      if (column[i] < partition->forest_cuts[r] || column[i] >= partition->forest_cuts[r + 1]) {
        return 0;
      }
    }
  }
  return 1;
}

/* ------------------------------------------------------------------------- */
/* cut_forests                                                               */
/* ------------------------------------------------------------------------- */

/**
 * @test    test_cut_forests_matches_brute_force_over_all_small_vectors
 * @brief   Makespan equals the enumerated optimum, and the cuts are D3's exact greedy packing
 */
int test_cut_forests_matches_brute_force_over_all_small_vectors(void) {
  int64_t weights[MAX_FORESTS];
  int64_t vectors = 0;

  for (int n = 0; n <= MAX_FORESTS; n++) {
    const int64_t count = power_of_choices(n);
    for (int64_t code = 0; code < count; code++) {
      decode_weights(code, n, weights);
      for (int ntask = 1; ntask <= MAX_TASKS; ntask++) {
        TEST_ASSERT(check_cut_vector(weights, n, ntask),
                    "cut_forests must match the brute-force optimum and the D3 greedy cuts");
        vectors++;
      }
    }
  }
  /* 4^0 + ... + 4^8 vectors times four rank counts: the sweep really was exhaustive. */
  TEST_ASSERT_EQUAL(vectors, (int64_t)87381 * MAX_TASKS, "every vector and rank count is covered");
  return 0;
}

/**
 * @test    test_cut_forests_heavy_forest_shares_range_with_light_neighbour
 * @brief   Weights [1, 5, 1] on two ranks give M* = 6 and cuts [0, 2, 3]
 */
int test_cut_forests_heavy_forest_shares_range_with_light_neighbour(void) {
  const int64_t weights[3] = {1, 5, 1};
  int64_t cuts[3] = {-1, -1, -1};

  horizontal_partition_cut_forests(weights, 3, 2, cuts);

  TEST_ASSERT_EQUAL(cuts[0], 0, "first cut is zero");
  TEST_ASSERT_EQUAL(cuts[1], 2, "the heavy forest closes the first range with its left neighbour");
  TEST_ASSERT_EQUAL(cuts[2], 3, "last cut is the forest count");
  const int64_t prefix[4] = {0, 1, 6, 7};
  TEST_ASSERT_EQUAL(cuts_makespan(prefix, cuts, 2), 6, "the makespan is 6");
  return 0;
}

/**
 * @test    test_cut_forests_single_rank_and_empty_dataset
 * @brief   ntask == 1 gives [0, n]; no forests give all-zero cuts for any rank count
 */
int test_cut_forests_single_rank_and_empty_dataset(void) {
  const int64_t weights[4] = {3, 0, 7, 1};
  int64_t cuts[2] = {-1, -1};

  horizontal_partition_cut_forests(weights, 4, 1, cuts);
  TEST_ASSERT(cuts[0] == 0 && cuts[1] == 4, "a single rank owns every forest");

  for (int ntask = 1; ntask <= 5; ntask++) {
    int64_t empty[6];
    for (int r = 0; r <= ntask; r++) {
      empty[r] = -1;
    }
    horizontal_partition_cut_forests(NULL, 0, ntask, empty);
    for (int r = 0; r <= ntask; r++) {
      TEST_ASSERT_EQUAL(empty[r], 0, "an empty dataset cuts at zero everywhere");
    }
  }
  return 0;
}

/**
 * @test    test_zero_weight_forests_are_assigned_and_never_cut_row_order
 * @brief   Forests with no rows ride in a neighbouring range; row_cuts stays a valid split
 */
int test_zero_weight_forests_are_assigned_and_never_cut_row_order(void) {
  /* Forests 0, 2, 3 and 5 hold no rows. */
  const int64_t column[5] = {1, 1, 1, 4, 4};
  const int64_t n_forests = 6;
  int64_t weights[6] = {0};

  horizontal_partition_accumulate_weights(weights, n_forests, column, 5);
  TEST_ASSERT(weights[1] == 3 && weights[4] == 2 &&
                  weights[0] + weights[2] + weights[3] + weights[5] == 0,
              "accumulate counts rows per forest");

  struct HorizontalForestPartition *partition = horizontal_partition_create(2, 1, n_forests);
  horizontal_partition_cut_forests(weights, n_forests, 2, partition->forest_cuts);
  TEST_ASSERT(partition->forest_cuts[0] == 0 && partition->forest_cuts[1] == 4 &&
                  partition->forest_cuts[2] == 6,
              "empty forests 2 and 3 join the first range, empty forest 5 the last");

  struct HorizontalForestScan scan;
  const int64_t sizes[1] = {2};
  horizontal_forest_scan_begin(&scan, partition, 0);
  scan_in_blocks(&scan, column, 5, sizes, 1);
  TEST_ASSERT_EQUAL(horizontal_forest_scan_end(&scan), 0, "the column is forest-blocked");
  TEST_ASSERT(partition->row_cuts[0] == 0 && partition->row_cuts[1] == 3 &&
                  partition->row_cuts[2] == 5,
              "rows split between forests 1 and 4 only");
  TEST_ASSERT(rows_agree_with_forest_ranges(partition, 0, column, 5),
              "every row lies in its rank's forest range");

  /* Zero-weight forests are cut positions only: every forest is in exactly one range. */
  for (int64_t f = 0; f < n_forests; f++) {
    int owners = 0;
    for (int r = 0; r < 2; r++) {
      owners += f >= partition->forest_cuts[r] && f < partition->forest_cuts[r + 1];
    }
    TEST_ASSERT_EQUAL(owners, 1, "every forest, including empty ones, has exactly one owner");
  }
  horizontal_partition_destroy(partition);
  return 0;
}

/**
 * @test    test_more_ranks_than_nonzero_forests_leaves_idle_ranges
 * @brief   Trailing idle ranks own no forests and no rows (equal adjacent row cuts)
 */
int test_more_ranks_than_nonzero_forests_leaves_idle_ranges(void) {
  const struct {
    int64_t n_forests;
    int64_t column[8];
    int64_t rows;
    int64_t expected_forest_cuts[5];
    int64_t expected_row_cuts[5];
  } cases[2] = {
      /* Two non-zero forests on four ranks. */
      {2, {0, 0, 1, 1, 1}, 5, {0, 1, 2, 2, 2}, {0, 2, 5, 5, 5}},
      /* Two non-zero forests among empty ones: the trailing empty forests stay in the last
       * used range. */
      {5, {1, 1, 1, 1, 3, 3, 3}, 7, {0, 3, 5, 5, 5}, {0, 4, 7, 7, 7}},
  };

  for (int c = 0; c < 2; c++) {
    int64_t weights[5] = {0};
    horizontal_partition_accumulate_weights(weights, cases[c].n_forests, cases[c].column,
                                            cases[c].rows);
    struct HorizontalForestPartition *partition =
        horizontal_partition_create(4, 1, cases[c].n_forests);
    horizontal_partition_cut_forests(weights, cases[c].n_forests, 4, partition->forest_cuts);

    struct HorizontalForestScan scan;
    const int64_t sizes[2] = {3, 1};
    horizontal_forest_scan_begin(&scan, partition, 0);
    scan_in_blocks(&scan, cases[c].column, cases[c].rows, sizes, 2);
    TEST_ASSERT_EQUAL(horizontal_forest_scan_end(&scan), 0, "the column is forest-blocked");

    for (int r = 0; r <= 4; r++) {
      TEST_ASSERT_EQUAL(partition->forest_cuts[r], cases[c].expected_forest_cuts[r],
                        "forest cuts match the greedy packing");
      TEST_ASSERT_EQUAL(partition->row_cuts[r], cases[c].expected_row_cuts[r],
                        "row cuts match the lower bounds");
    }
    /* The two trailing ranks are idle: no forests and no rows. */
    for (int r = 2; r < 4; r++) {
      TEST_ASSERT(partition->row_cuts[r] == partition->row_cuts[r + 1],
                  "an idle range has equal adjacent row cuts");
      TEST_ASSERT(partition->forest_cuts[r] == partition->forest_cuts[r + 1],
                  "an idle range has equal adjacent forest cuts");
    }
    horizontal_partition_destroy(partition);
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* Streaming scan                                                            */
/* ------------------------------------------------------------------------- */

/**
 * @test    test_scan_row_cuts_equal_lower_bound_for_hand_built_columns
 * @brief   Blocks of varying size, including boundaries inside a forest run, change nothing
 */
int test_scan_row_cuts_equal_lower_bound_for_hand_built_columns(void) {
  /* Forests 2 and 4 are empty; forest 3's run (rows 5-8) and forest 0's (rows 0-2) straddle
   * block boundaries for the sizes below. */
  const int64_t column[10] = {0, 0, 0, 1, 1, 3, 3, 3, 3, 5};
  const int64_t n_forests = 6;
  const int64_t size_patterns[4][4] = {{1, 3, 2, 5}, {10, 1, 1, 1}, {4, 4, 4, 4}, {1, 1, 1, 1}};
  const int64_t cut_sets[3][4] = {{0, 2, 4, 6}, {0, 2, 2, 6}, {0, 1, 3, 6}};

  for (int cs = 0; cs < 3; cs++) {
    for (int pattern = 0; pattern < 4; pattern++) {
      struct HorizontalForestPartition *partition = horizontal_partition_create(3, 2, n_forests);
      memcpy(partition->forest_cuts, cut_sets[cs], sizeof(cut_sets[cs]));
      for (int r = 0; r < 4; r++) {
        partition->row_cuts[r] = 12345;     /* snapshot 0 must stay untouched */
        partition->row_cuts[4 + r] = -7777; /* snapshot 1 must be fully overwritten */
      }

      struct HorizontalForestScan scan;
      horizontal_forest_scan_begin(&scan, partition, 1);
      scan_in_blocks(&scan, column, 10, size_patterns[pattern], 4);
      TEST_ASSERT_EQUAL(horizontal_forest_scan_end(&scan), 0, "non-decreasing column is accepted");

      for (int r = 0; r < 4; r++) {
        TEST_ASSERT_EQUAL(partition->row_cuts[4 + r], lower_bound(column, 10, cut_sets[cs][r]),
                          "row_cuts[s][r] is the lower bound of forest_cuts[r]");
        TEST_ASSERT_EQUAL(partition->row_cuts[r], 12345, "another snapshot's row is untouched");
      }
      TEST_ASSERT_EQUAL(partition->row_cuts[4 + 3], 10, "the last row cut is the rows seen");
      horizontal_partition_destroy(partition);
    }
  }
  return 0;
}

/**
 * @test    test_scan_row_cuts_equal_lower_bound_for_every_small_column
 * @brief   Sweep: every weight vector up to length 5, 1..4 ranks, four block sizes
 */
int test_scan_row_cuts_equal_lower_bound_for_every_small_column(void) {
  const int64_t block_sizes[4] = {1, 2, 3, 7};
  int64_t weights[5];
  int64_t column[5 * 5];

  for (int n = 0; n <= 5; n++) {
    const int64_t count = power_of_choices(n);
    for (int64_t code = 0; code < count; code++) {
      decode_weights(code, n, weights);
      const int64_t rows = build_column(weights, n, column);
      for (int ntask = 1; ntask <= MAX_TASKS; ntask++) {
        for (int b = 0; b < 4; b++) {
          struct HorizontalForestPartition *partition = horizontal_partition_create(ntask, 1, n);
          horizontal_partition_cut_forests(weights, n, ntask, partition->forest_cuts);

          struct HorizontalForestScan scan;
          horizontal_forest_scan_begin(&scan, partition, 0);
          scan_in_blocks(&scan, column, rows, &block_sizes[b], 1);
          TEST_ASSERT_EQUAL(horizontal_forest_scan_end(&scan), 0, "column is forest-blocked");

          int ok = rows_agree_with_forest_ranges(partition, 0, column, rows);
          for (int r = 0; ok && r <= ntask; r++) {
            ok = partition->row_cuts[r] == lower_bound(column, rows, partition->forest_cuts[r]);
          }
          if (!ok) {
            print_vector("weights", weights, n);
            fprintf(stderr, "  ntask = %d, block size = %" PRId64 "\n", ntask, block_sizes[b]);
          }
          TEST_ASSERT(ok, "streamed row_cuts must equal the lower bound of every forest cut");
          horizontal_partition_destroy(partition);
        }
      }
    }
  }
  return 0;
}

/**
 * @test    test_scan_reports_decreasing_pair_with_row_and_values
 * @brief   The first descent is recorded across block boundaries; equal values are accepted
 */
int test_scan_reports_decreasing_pair_with_row_and_values(void) {
  struct HorizontalForestPartition *partition = horizontal_partition_create(2, 1, 6);
  partition->forest_cuts[1] = 3;
  partition->forest_cuts[2] = 6;
  struct HorizontalForestScan scan;

  /* Descent between two blocks (the pair straddles the boundary after row 3). Later
   * descents must not replace the first. */
  const int64_t straddling[7] = {0, 0, 2, 2, 1, 5, 4};
  const int64_t sizes[2] = {4, 3};
  horizontal_forest_scan_begin(&scan, partition, 0);
  scan_in_blocks(&scan, straddling, 7, sizes, 2);
  TEST_ASSERT_EQUAL(horizontal_forest_scan_end(&scan), -1, "a decreasing column is refused");
  TEST_ASSERT_EQUAL(scan.violation_row, 4, "the offending row is reported");
  TEST_ASSERT_EQUAL(scan.violation_previous, 2, "the previous value is reported");
  TEST_ASSERT_EQUAL(scan.violation_value, 1, "the offending value is reported");

  /* Descent inside one block, at the very first pair. */
  const int64_t early[3] = {3, 2, 5};
  const int64_t whole[1] = {3};
  horizontal_forest_scan_begin(&scan, partition, 0);
  scan_in_blocks(&scan, early, 3, whole, 1);
  TEST_ASSERT_EQUAL(horizontal_forest_scan_end(&scan), -1, "a descent at row 1 is refused");
  TEST_ASSERT(scan.violation_row == 1 && scan.violation_previous == 3 && scan.violation_value == 2,
              "the descent at row 1 is reported exactly");

  /* Equal neighbours are not a descent. */
  const int64_t flat[4] = {2, 2, 2, 2};
  horizontal_forest_scan_begin(&scan, partition, 0);
  scan_in_blocks(&scan, flat, 4, whole, 1);
  TEST_ASSERT_EQUAL(horizontal_forest_scan_end(&scan), 0, "equal values are non-decreasing");
  horizontal_partition_destroy(partition);
  return 0;
}

/**
 * @test    test_scan_of_empty_column_yields_zero_row_cuts
 * @brief   No rows: row_cuts[s][r] == 0 for every r, with or without empty blocks delivered
 */
int test_scan_of_empty_column_yields_zero_row_cuts(void) {
  for (int with_empty_block = 0; with_empty_block <= 1; with_empty_block++) {
    struct HorizontalForestPartition *partition = horizontal_partition_create(3, 1, 9);
    const int64_t cuts[4] = {0, 3, 6, 9};
    memcpy(partition->forest_cuts, cuts, sizeof(cuts));
    for (int r = 0; r < 4; r++) {
      partition->row_cuts[r] = 99;
    }

    struct HorizontalForestScan scan;
    horizontal_forest_scan_begin(&scan, partition, 0);
    if (with_empty_block) {
      horizontal_forest_scan_visit(&scan, 0, NULL, 0);
    }
    TEST_ASSERT_EQUAL(horizontal_forest_scan_end(&scan), 0, "an empty column is forest-blocked");
    for (int r = 0; r < 4; r++) {
      TEST_ASSERT_EQUAL(partition->row_cuts[r], 0, "every row cut is zero for an empty column");
    }
    horizontal_partition_destroy(partition);
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* Weights, lifecycle and aborts                                             */
/* ------------------------------------------------------------------------- */

/**
 * @test    test_accumulate_weights_counts_rows_across_blocks
 * @brief   Weights add up over successive blocks, one per streamed value
 */
int test_accumulate_weights_counts_rows_across_blocks(void) {
  int64_t weights[4] = {0, 0, 0, 0};
  const int64_t first[3] = {0, 0, 2};
  const int64_t second[2] = {2, 3};

  horizontal_partition_accumulate_weights(weights, 4, first, 3);
  horizontal_partition_accumulate_weights(weights, 4, second, 2);
  horizontal_partition_accumulate_weights(weights, 4, NULL, 0);

  TEST_ASSERT(weights[0] == 2 && weights[1] == 0 && weights[2] == 2 && weights[3] == 1,
              "each streamed value adds one to its forest");
  return 0;
}

/**
 * @test    test_partition_create_is_zeroed_sized_and_released
 * @brief   Tables have the documented shapes, start at zero and are freed by destroy
 */
int test_partition_create_is_zeroed_sized_and_released(void) {
  const size_t before = memory_category_bytes(MEM_HALOS);
  struct HorizontalForestPartition *partition = horizontal_partition_create(3, 5, 100);

  TEST_ASSERT(partition->ntask == 3 && partition->snapshot_count == 5 &&
                  partition->n_forests_total == 100,
              "the partition records its shape");
  TEST_ASSERT(memory_category_bytes(MEM_HALOS) > before, "the tables are tracked MEM_HALOS blocks");
  for (int r = 0; r <= 3; r++) {
    TEST_ASSERT_EQUAL(partition->forest_cuts[r], 0, "forest_cuts starts zeroed");
  }
  for (int i = 0; i < 5 * 4; i++) {
    TEST_ASSERT_EQUAL(partition->row_cuts[i], 0, "row_cuts starts zeroed (snapshot_count x 4)");
  }

  horizontal_partition_destroy(partition);
  TEST_ASSERT(memory_category_bytes(MEM_HALOS) == before, "destroy releases every tracked block");
  horizontal_partition_destroy(NULL); /* ignored */

  /* An empty dataset (no snapshots) still creates and destroys cleanly. */
  struct HorizontalForestPartition *empty = horizontal_partition_create(1, 0, 0);
  horizontal_partition_destroy(empty);
  TEST_ASSERT(memory_category_bytes(MEM_HALOS) == before, "an empty partition leaks nothing");
  return 0;
}

static int64_t child_value = 0;

static void child_accumulate_out_of_range(const char *unused) {
  (void)unused;
  int64_t weights[3] = {0, 0, 0};
  const int64_t values[3] = {0, 1, child_value};
  horizontal_partition_accumulate_weights(weights, 3, values, 3);
}

static void child_cut_overflowing_weights(const char *unused) {
  (void)unused;
  const int64_t weights[3] = {INT64_MAX / 2 + 1, 0, INT64_MAX / 2 + 1};
  int64_t cuts[3];
  horizontal_partition_cut_forests(weights, 3, 2, cuts);
}

static void child_cut_summing_to_int64_max(const char *unused) {
  (void)unused;
  const int64_t weights[2] = {INT64_MAX - 1, 1}; /* the sum is exactly INT64_MAX */
  int64_t cuts[3];
  horizontal_partition_cut_forests(weights, 2, 2, cuts);
  if (cuts[0] != 0 || cuts[1] != 1 || cuts[2] != 2) {
    myexit(1);
  }
}

static void child_scan_with_gap(const char *unused) {
  (void)unused;
  struct HorizontalForestPartition *partition = horizontal_partition_create(2, 1, 4);
  struct HorizontalForestScan scan;
  const int64_t values[2] = {0, 1};
  horizontal_forest_scan_begin(&scan, partition, 0);
  horizontal_forest_scan_visit(&scan, 0, values, 2);
  horizontal_forest_scan_visit(&scan, 3, values, 2); /* row 2 is missing */
}

static void child_scan_of_missing_snapshot(const char *unused) {
  (void)unused;
  struct HorizontalForestPartition *partition = horizontal_partition_create(2, 3, 4);
  struct HorizontalForestScan scan;
  horizontal_forest_scan_begin(&scan, partition, 3);
}

/**
 * @test    test_defective_inputs_abort
 * @brief   Out-of-range forest index, weight-sum overflow, streaming gap, bad snapshot
 */
int test_defective_inputs_abort(void) {
  child_value = 3;
  TEST_ASSERT(expect_fatal(NULL, child_accumulate_out_of_range, "ForestIndex 3 is outside [0, 3)",
                           NULL) == 1,
              "a ForestIndex equal to the forest count aborts");
  child_value = -1;
  TEST_ASSERT(expect_fatal(NULL, child_accumulate_out_of_range, "ForestIndex -1 is outside [0, 3)",
                           NULL) == 1,
              "a negative ForestIndex aborts");
  TEST_ASSERT(expect_fatal(NULL, child_cut_overflowing_weights, "sum beyond int64_t", NULL) == 1,
              "a weight sum beyond int64_t aborts");
  TEST_ASSERT(expect_success(NULL, child_cut_summing_to_int64_max) == 1,
              "a weight sum of exactly INT64_MAX is accepted and cut correctly");
  TEST_ASSERT(expect_fatal(NULL, child_scan_with_gap, "expected the block to start at row 2",
                           "starts at row 3") == 1,
              "a block that skips rows aborts");
  TEST_ASSERT(
      expect_fatal(NULL, child_scan_of_missing_snapshot, "snapshot 3 is outside [0, 3)", NULL) == 1,
      "a snapshot index beyond snapshot_count aborts");
  return 0;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Horizontal Forest-Block Partition\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  TEST_RUN(test_cut_forests_matches_brute_force_over_all_small_vectors);
  TEST_RUN(test_cut_forests_heavy_forest_shares_range_with_light_neighbour);
  TEST_RUN(test_cut_forests_single_rank_and_empty_dataset);
  TEST_RUN(test_zero_weight_forests_are_assigned_and_never_cut_row_order);
  TEST_RUN(test_more_ranks_than_nonzero_forests_leaves_idle_ranges);
  TEST_RUN(test_scan_row_cuts_equal_lower_bound_for_hand_built_columns);
  TEST_RUN(test_scan_row_cuts_equal_lower_bound_for_every_small_column);
  TEST_RUN(test_scan_reports_decreasing_pair_with_row_and_values);
  TEST_RUN(test_scan_of_empty_column_yields_zero_row_cuts);
  TEST_RUN(test_accumulate_weights_counts_rows_across_blocks);
  TEST_RUN(test_partition_create_is_zeroed_sized_and_released);
  TEST_RUN(test_defective_inputs_abort);

  TEST_SUMMARY();
  return TEST_RESULT();
}
