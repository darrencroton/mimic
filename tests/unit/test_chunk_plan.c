/**
 * @file    test_chunk_plan.c
 * @brief   Unit tests for pure output chunk planning helpers.
 */

#include "../framework/test_framework.h"
#include "../../src/io/tree/chunk_plan.h"
#include "../../src/util/error.h"

#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

static int assert_chunk(const struct ChunkPlan *plan, int64_t idx, int64_t start, int64_t nforests,
                        double size) {
  TEST_ASSERT(idx < plan->nchunks, "chunk index should exist");
  TEST_ASSERT_EQUAL(plan->chunks[idx].start_forest, start, "chunk start should match");
  TEST_ASSERT_EQUAL(plan->chunks[idx].nforests, nforests, "chunk length should match");
  TEST_ASSERT_DOUBLE_EQUAL(plan->chunks[idx].size, size, 1e-12, "chunk size should match");
  TEST_ASSERT_DOUBLE_EQUAL(plan->chunks[idx].cost, size, 1e-12,
                           "default chunk cost should match size");
  return TEST_PASS;
}

static double assignment_makespan(int64_t nchunks, const double *costs, int ntasks,
                                  const int *task_of_chunk) {
  double *loads = calloc((size_t)ntasks, sizeof(*loads));
  if (loads == NULL)
    return DBL_MAX;

  for (int64_t chunk = 0; chunk < nchunks; chunk++)
    loads[task_of_chunk[chunk]] += costs[chunk];

  double makespan = 0.0;
  for (int task = 0; task < ntasks; task++)
    if (loads[task] > makespan)
      makespan = loads[task];
  free(loads);
  return makespan;
}

static void brute_force_optimal(const double *costs, int nchunks, int ntasks, int chunk,
                                double *loads, double *best) {
  if (chunk == nchunks) {
    double makespan = 0.0;
    for (int task = 0; task < ntasks; task++)
      if (loads[task] > makespan)
        makespan = loads[task];
    if (makespan < *best)
      *best = makespan;
    return;
  }

  for (int task = 0; task < ntasks; task++) {
    loads[task] += costs[chunk];
    if (loads[task] < *best)
      brute_force_optimal(costs, nchunks, ntasks, chunk + 1, loads, best);
    loads[task] -= costs[chunk];
  }
}

static double optimal_makespan(const double *costs, int nchunks, int ntasks) {
  double *loads = calloc((size_t)ntasks, sizeof(*loads));
  if (loads == NULL)
    return DBL_MAX;

  double best = DBL_MAX;
  brute_force_optimal(costs, nchunks, ntasks, 0, loads, &best);
  free(loads);
  return best;
}

int test_size_budget_boundaries(void) {
  const double sizes[] = {2.0, 11.0, 3.0, 4.0, 6.0};
  struct ChunkPlan plan;

  TEST_ASSERT(chunk_plan_build_boundaries(5, sizes, 10.0, 0, &plan) == 0,
              "size-budget planning should succeed");
  TEST_ASSERT_EQUAL(plan.nchunks, 4, "oversized forest should be its own chunk");
  TEST_ASSERT(assert_chunk(&plan, 0, 0, 1, 2.0) == TEST_PASS, "first chunk should match");
  TEST_ASSERT(assert_chunk(&plan, 1, 1, 1, 11.0) == TEST_PASS, "large chunk should match");
  TEST_ASSERT(assert_chunk(&plan, 2, 2, 2, 7.0) == TEST_PASS, "middle chunk should match");
  TEST_ASSERT(assert_chunk(&plan, 3, 4, 1, 6.0) == TEST_PASS, "last chunk should match");
  chunk_plan_free(&plan);
  return TEST_PASS;
}

int test_even_chunking_and_no_empty_chunks(void) {
  const double sizes[] = {2.0, 2.0, 2.0, 2.0};
  struct ChunkPlan plan;

  TEST_ASSERT(chunk_plan_build_boundaries(4, sizes, 4.0, 0, &plan) == 0,
              "even chunking should succeed");
  TEST_ASSERT_EQUAL(plan.nchunks, 2, "four equal forests should form two chunks");
  TEST_ASSERT(assert_chunk(&plan, 0, 0, 2, 4.0) == TEST_PASS, "first even chunk should match");
  TEST_ASSERT(assert_chunk(&plan, 1, 2, 2, 4.0) == TEST_PASS, "second even chunk should match");
  for (int64_t i = 0; i < plan.nchunks; i++)
    TEST_ASSERT(plan.chunks[i].nforests > 0, "planner must never emit empty chunks");
  chunk_plan_free(&plan);

  TEST_ASSERT(chunk_plan_build_boundaries(0, NULL, 4.0, 0, &plan) == 0,
              "zero forests should be valid");
  TEST_ASSERT_EQUAL(plan.nchunks, 0, "zero forests should emit no chunks");
  chunk_plan_free(&plan);
  return TEST_PASS;
}

int test_forests_per_file_override(void) {
  const double sizes[] = {100.0, 1.0, 1.0, 100.0, 1.0, 1.0, 100.0};
  struct ChunkPlan plan;

  TEST_ASSERT(chunk_plan_build_boundaries(7, sizes, 2.0, 3, &plan) == 0,
              "forest-count override should ignore size budget");
  TEST_ASSERT_EQUAL(plan.nchunks, 3, "override should create fixed-count chunks");
  TEST_ASSERT(assert_chunk(&plan, 0, 0, 3, 102.0) == TEST_PASS, "first override chunk");
  TEST_ASSERT(assert_chunk(&plan, 1, 3, 3, 102.0) == TEST_PASS, "second override chunk");
  TEST_ASSERT(assert_chunk(&plan, 2, 6, 1, 100.0) == TEST_PASS, "tail override chunk");
  chunk_plan_free(&plan);
  return TEST_PASS;
}

int test_streaming_matches_full_array(void) {
  const double all_sizes[] = {1.0, 2.0, 7.0, 3.0, 2.0, 8.0};
  const double first_file[] = {1.0, 2.0};
  const double second_file[] = {7.0, 3.0, 2.0, 8.0};
  struct ChunkPlan full_plan;
  struct ChunkPlan streaming_plan;
  struct ChunkPlanBuilder builder;

  TEST_ASSERT(chunk_plan_build_boundaries(6, all_sizes, 10.0, 0, &full_plan) == 0,
              "full-array wrapper should succeed");
  TEST_ASSERT(chunk_plan_builder_init(&builder, 10.0, 0) == 0, "streaming init should succeed");
  TEST_ASSERT(chunk_plan_builder_add_file(&builder, 2, first_file) == 0,
              "first feed should succeed");
  TEST_ASSERT(chunk_plan_builder_add_file(&builder, 4, second_file) == 0,
              "second feed should succeed");
  TEST_ASSERT(chunk_plan_builder_finish(&builder, &streaming_plan) == 0,
              "streaming finish should succeed");

  TEST_ASSERT_EQUAL(streaming_plan.nchunks, full_plan.nchunks,
                    "streaming and full-array chunk counts should match");
  for (int64_t i = 0; i < full_plan.nchunks; i++) {
    TEST_ASSERT_EQUAL(streaming_plan.chunks[i].start_forest, full_plan.chunks[i].start_forest,
                      "streaming chunk start should match");
    TEST_ASSERT_EQUAL(streaming_plan.chunks[i].nforests, full_plan.chunks[i].nforests,
                      "streaming chunk length should match");
    TEST_ASSERT_DOUBLE_EQUAL(streaming_plan.chunks[i].size, full_plan.chunks[i].size, 1e-12,
                             "streaming chunk size should match");
  }

  chunk_plan_free(&full_plan);
  chunk_plan_free(&streaming_plan);
  return TEST_PASS;
}

int test_streaming_costs_are_accumulated_independently(void) {
  const double sizes[] = {6.0, 1.0, 1.0, 6.0};
  const double costs[] = {1.0, 10.0, 100.0, 1000.0};
  struct ChunkPlanBuilder builder;
  struct ChunkPlan plan;

  TEST_ASSERT(chunk_plan_builder_init(&builder, 6.0, 0) == 0, "streaming init should succeed");
  TEST_ASSERT(chunk_plan_builder_add_file_with_cost(&builder, 4, sizes, costs) == 0,
              "streaming cost feed should succeed");
  TEST_ASSERT(chunk_plan_builder_finish(&builder, &plan) == 0, "streaming finish should succeed");

  TEST_ASSERT_EQUAL(plan.nchunks, 3, "size budget should still drive boundaries");
  TEST_ASSERT_DOUBLE_EQUAL(plan.chunks[0].size, 6.0, 1e-12, "first chunk size should match");
  TEST_ASSERT_DOUBLE_EQUAL(plan.chunks[0].cost, 1.0, 1e-12, "first chunk cost should match");
  TEST_ASSERT_DOUBLE_EQUAL(plan.chunks[1].size, 2.0, 1e-12, "middle chunk size should match");
  TEST_ASSERT_DOUBLE_EQUAL(plan.chunks[1].cost, 110.0, 1e-12, "middle chunk cost should sum costs");
  TEST_ASSERT_DOUBLE_EQUAL(plan.chunks[2].size, 6.0, 1e-12, "last chunk size should match");
  TEST_ASSERT_DOUBLE_EQUAL(plan.chunks[2].cost, 1000.0, 1e-12, "last chunk cost should match");

  chunk_plan_free(&plan);
  return TEST_PASS;
}

int test_streaming_accepts_int64_forest_indices(void) {
  const double sizes[] = {1.0, 1.0};
  struct ChunkPlanBuilder builder;
  struct ChunkPlan plan;
  const int64_t mocked_start = (int64_t)INT_MAX + 7;

  TEST_ASSERT(chunk_plan_builder_init(&builder, 10.0, 0) == 0, "streaming init should succeed");
  builder.next_forest = mocked_start;
  TEST_ASSERT(chunk_plan_builder_add_file(&builder, 2, sizes) == 0,
              "mocked over-INT_MAX feed should succeed");
  TEST_ASSERT(chunk_plan_builder_finish(&builder, &plan) == 0, "streaming finish should succeed");

  TEST_ASSERT_EQUAL(plan.nchunks, 1, "mocked high-index forests should form one chunk");
  TEST_ASSERT_EQUAL(plan.chunks[0].start_forest, mocked_start,
                    "chunk start should preserve int64 forest index");
  TEST_ASSERT_EQUAL(plan.chunks[0].nforests, 2, "chunk length should match");

  chunk_plan_free(&plan);
  return TEST_PASS;
}

int test_lpt_single_task_and_idle_tasks(void) {
  const double costs[] = {5.0, 4.0};
  int task_of_chunk[2] = {-1, -1};
  int task_counts[4] = {0};

  TEST_ASSERT(chunk_plan_assign_lpt(2, costs, 1, task_of_chunk) == 0,
              "single-task assignment should succeed");
  TEST_ASSERT_EQUAL(task_of_chunk[0], 0, "first chunk should go to task zero");
  TEST_ASSERT_EQUAL(task_of_chunk[1], 0, "second chunk should go to task zero");

  TEST_ASSERT(chunk_plan_assign_lpt(2, costs, 4, task_of_chunk) == 0,
              "assignment with idle tasks should succeed");
  for (int64_t chunk = 0; chunk < 2; chunk++)
    task_counts[task_of_chunk[chunk]]++;
  TEST_ASSERT_EQUAL(task_counts[0], 1, "task zero should receive one chunk");
  TEST_ASSERT_EQUAL(task_counts[1], 1, "task one should receive one chunk");
  TEST_ASSERT_EQUAL(task_counts[2], 0, "task two should be idle");
  TEST_ASSERT_EQUAL(task_counts[3], 0, "task three should be idle");
  return TEST_PASS;
}

int test_lpt_tie_break_is_stable(void) {
  const double costs[] = {1.0, 1.0, 1.0, 1.0};
  int first[4] = {-1, -1, -1, -1};
  int second[4] = {-1, -1, -1, -1};

  TEST_ASSERT(chunk_plan_assign_lpt(4, costs, 2, first) == 0, "first assignment should succeed");
  TEST_ASSERT(chunk_plan_assign_lpt(4, costs, 2, second) == 0, "second assignment should succeed");
  for (int64_t chunk = 0; chunk < 4; chunk++)
    TEST_ASSERT_EQUAL(first[chunk], second[chunk], "assignments should be deterministic");

  TEST_ASSERT_EQUAL(first[0], 0, "lowest tied chunk should choose lowest loaded task");
  TEST_ASSERT_EQUAL(first[1], 1, "second tied chunk should choose the next task");
  TEST_ASSERT_EQUAL(first[2], 0, "third tied chunk should return to task zero");
  TEST_ASSERT_EQUAL(first[3], 1, "fourth tied chunk should return to task one");
  return TEST_PASS;
}

int test_lpt_makespan_bound_on_spiky_distribution(void) {
  const double costs[] = {20.0, 19.0, 13.0, 12.0, 11.0, 8.0, 7.0, 6.0, 4.0};
  int task_of_chunk[9] = {-1, -1, -1, -1, -1, -1, -1, -1, -1};

  TEST_ASSERT(chunk_plan_assign_lpt(9, costs, 3, task_of_chunk) == 0,
              "spiky assignment should succeed");
  const double lpt_makespan = assignment_makespan(9, costs, 3, task_of_chunk);
  const double optimal = optimal_makespan(costs, 9, 3);

  TEST_ASSERT(lpt_makespan <= (4.0 / 3.0) * optimal + 1e-12,
              "LPT makespan should be within 4/3 of optimal");
  return TEST_PASS;
}

int test_invalid_inputs_and_zero_chunk_assignment(void) {
  const double valid_sizes[] = {1.0};
  const double invalid_sizes[] = {-1.0};
  const double nonfinite_sizes[] = {NAN};
  const double valid_costs[] = {1.0};
  const double invalid_costs[] = {-1.0};
  const double nonfinite_costs[] = {INFINITY};
  struct ChunkPlanBuilder builder;
  struct ChunkPlan plan;
  int task_of_chunk = -1;

  TEST_ASSERT(chunk_plan_builder_init(NULL, 1.0, 0) != 0,
              "builder init should reject NULL builder");
  TEST_ASSERT(chunk_plan_builder_init(&builder, 0.0, 0) != 0,
              "builder init should reject non-positive budget without override");
  TEST_ASSERT(chunk_plan_builder_init(&builder, 1.0, -1) != 0,
              "builder init should reject negative forest-count override");

  TEST_ASSERT(chunk_plan_build_boundaries(0, NULL, 1.0, 0, NULL) != 0,
              "full-array wrapper should reject NULL output plan");
  TEST_ASSERT(chunk_plan_build_boundaries(-1, valid_sizes, 1.0, 0, &plan) != 0,
              "full-array wrapper should reject negative forest count");
  TEST_ASSERT(chunk_plan_build_boundaries(1, NULL, 1.0, 0, &plan) != 0,
              "full-array wrapper should reject NULL non-empty size array");
  TEST_ASSERT(chunk_plan_build_boundaries(1, invalid_sizes, 1.0, 0, &plan) != 0,
              "full-array wrapper should reject negative forest sizes");
  TEST_ASSERT(chunk_plan_build_boundaries(1, nonfinite_sizes, 1.0, 0, &plan) != 0,
              "full-array wrapper should reject non-finite forest sizes");
  TEST_ASSERT(chunk_plan_builder_init(&builder, 1.0, 0) == 0,
              "builder init should succeed for invalid-cost checks");
  TEST_ASSERT(chunk_plan_builder_add_file_with_cost(&builder, 1, valid_sizes, invalid_costs) != 0,
              "streaming builder should reject negative forest costs");
  chunk_plan_free(&builder.plan);
  TEST_ASSERT(chunk_plan_builder_init(&builder, 1.0, 0) == 0,
              "builder init should succeed for non-finite-cost checks");
  TEST_ASSERT(chunk_plan_builder_add_file_with_cost(&builder, 1, valid_sizes, nonfinite_costs) != 0,
              "streaming builder should reject non-finite forest costs");
  chunk_plan_free(&builder.plan);

  TEST_ASSERT(chunk_plan_assign_lpt(0, NULL, 1, NULL) == 0,
              "zero-chunk LPT assignment should be valid");
  TEST_ASSERT(chunk_plan_assign_lpt(-1, valid_costs, 1, &task_of_chunk) != 0,
              "LPT assignment should reject negative chunk count");
  TEST_ASSERT(chunk_plan_assign_lpt(1, NULL, 1, &task_of_chunk) != 0,
              "LPT assignment should reject NULL non-empty cost array");
  TEST_ASSERT(chunk_plan_assign_lpt(1, valid_costs, 1, NULL) != 0,
              "LPT assignment should reject NULL non-empty output array");
  TEST_ASSERT(chunk_plan_assign_lpt(1, valid_costs, 0, &task_of_chunk) != 0,
              "LPT assignment should reject zero tasks");
  TEST_ASSERT(chunk_plan_assign_lpt(1, invalid_costs, 1, &task_of_chunk) != 0,
              "LPT assignment should reject negative chunk costs");

  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Chunk Plan\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  TEST_RUN(test_size_budget_boundaries);
  TEST_RUN(test_even_chunking_and_no_empty_chunks);
  TEST_RUN(test_forests_per_file_override);
  TEST_RUN(test_streaming_matches_full_array);
  TEST_RUN(test_streaming_costs_are_accumulated_independently);
  TEST_RUN(test_streaming_accepts_int64_forest_indices);
  TEST_RUN(test_lpt_single_task_and_idle_tasks);
  TEST_RUN(test_lpt_tie_break_is_stable);
  TEST_RUN(test_lpt_makespan_bound_on_spiky_distribution);
  TEST_RUN(test_invalid_inputs_and_zero_chunk_assignment);

  TEST_SUMMARY();
  return TEST_RESULT();
}
