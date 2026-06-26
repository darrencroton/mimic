/**
 * @file    tree/chunk_plan.c
 * @brief   Pure output chunk-planning helpers.
 */

#include "tree/chunk_plan.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static int chunk_plan_append(struct ChunkPlan *plan, int64_t start_forest, int64_t nforests,
                             double size, double cost) {
  if (nforests <= 0)
    return -1;

  if (plan->nchunks == plan->capacity) {
    if (plan->capacity > INT64_MAX / 2)
      return -1;
    int64_t new_capacity = plan->capacity == 0 ? 16 : plan->capacity * 2;
    if ((uint64_t)new_capacity > (uint64_t)(SIZE_MAX / sizeof(struct ChunkPlanRange)))
      return -1;

    struct ChunkPlanRange *new_chunks =
        realloc(plan->chunks, (size_t)new_capacity * sizeof(*new_chunks));
    if (new_chunks == NULL)
      return -1;

    plan->chunks = new_chunks;
    plan->capacity = new_capacity;
  }

  plan->chunks[plan->nchunks].start_forest = start_forest;
  plan->chunks[plan->nchunks].nforests = nforests;
  plan->chunks[plan->nchunks].size = size;
  plan->chunks[plan->nchunks].cost = cost;
  plan->nchunks++;
  return 0;
}

static int chunk_plan_emit_current(struct ChunkPlanBuilder *builder) {
  if (builder->current_nforests == 0)
    return 0;

  if (chunk_plan_append(&builder->plan, builder->current_start, builder->current_nforests,
                        builder->current_size, builder->current_cost) != 0)
    return -1;

  builder->current_nforests = 0;
  builder->current_size = 0.0;
  builder->current_cost = 0.0;
  return 0;
}

int chunk_plan_builder_init(struct ChunkPlanBuilder *builder, double size_budget,
                            int64_t forests_per_file_override) {
  if (builder == NULL)
    return -1;
  if (forests_per_file_override < 0)
    return -1;
  if (forests_per_file_override == 0 && (!isfinite(size_budget) || size_budget <= 0.0))
    return -1;

  memset(builder, 0, sizeof(*builder));
  builder->size_budget = size_budget;
  builder->forests_per_file_override = forests_per_file_override;
  return 0;
}

int chunk_plan_builder_add_file(struct ChunkPlanBuilder *builder, int64_t nforests,
                                const double *size_per_forest) {
  return chunk_plan_builder_add_file_with_cost(builder, nforests, size_per_forest, NULL);
}

int chunk_plan_builder_add_file_with_cost(struct ChunkPlanBuilder *builder, int64_t nforests,
                                          const double *size_per_forest,
                                          const double *cost_per_forest) {
  if (builder == NULL || nforests < 0)
    return -1;
  if (nforests > 0 && size_per_forest == NULL)
    return -1;

  for (int64_t i = 0; i < nforests; i++) {
    const double forest_size = size_per_forest[i];
    const double forest_cost = cost_per_forest != NULL ? cost_per_forest[i] : forest_size;
    if (!isfinite(forest_size) || forest_size < 0.0)
      return -1;
    if (!isfinite(forest_cost) || forest_cost < 0.0)
      return -1;

    if (builder->forests_per_file_override > 0) {
      if (builder->current_nforests == builder->forests_per_file_override &&
          chunk_plan_emit_current(builder) != 0)
        return -1;
    } else if (builder->current_nforests > 0 &&
               builder->current_size + forest_size > builder->size_budget) {
      if (chunk_plan_emit_current(builder) != 0)
        return -1;
    }

    if (builder->current_nforests == 0)
      builder->current_start = builder->next_forest;
    builder->current_nforests++;
    builder->current_size += forest_size;
    builder->current_cost += forest_cost;
    builder->next_forest++;

    if (builder->forests_per_file_override == 0 && forest_size > builder->size_budget &&
        chunk_plan_emit_current(builder) != 0)
      return -1;
  }

  return 0;
}

int chunk_plan_builder_finish(struct ChunkPlanBuilder *builder, struct ChunkPlan *plan) {
  if (builder == NULL || plan == NULL)
    return -1;
  if (chunk_plan_emit_current(builder) != 0)
    return -1;

  *plan = builder->plan;
  memset(&builder->plan, 0, sizeof(builder->plan));
  return 0;
}

int chunk_plan_build_boundaries(int64_t nforests, const double *size_per_forest, double size_budget,
                                int64_t forests_per_file_override, struct ChunkPlan *plan) {
  struct ChunkPlanBuilder builder;
  if (plan == NULL)
    return -1;
  memset(plan, 0, sizeof(*plan));

  if (chunk_plan_builder_init(&builder, size_budget, forests_per_file_override) != 0)
    return -1;
  if (chunk_plan_builder_add_file(&builder, nforests, size_per_forest) != 0) {
    chunk_plan_free(&builder.plan);
    return -1;
  }
  if (chunk_plan_builder_finish(&builder, plan) != 0) {
    chunk_plan_free(&builder.plan);
    return -1;
  }

  return 0;
}

void chunk_plan_free(struct ChunkPlan *plan) {
  if (plan == NULL)
    return;
  free(plan->chunks);
  memset(plan, 0, sizeof(*plan));
}

struct ChunkCostOrder {
  int64_t chunk_id;
  double cost;
};

static int compare_chunk_cost_desc(const void *lhs, const void *rhs) {
  const struct ChunkCostOrder *a = lhs;
  const struct ChunkCostOrder *b = rhs;

  if (a->cost > b->cost)
    return -1;
  if (a->cost < b->cost)
    return 1;
  if (a->chunk_id < b->chunk_id)
    return -1;
  if (a->chunk_id > b->chunk_id)
    return 1;
  return 0;
}

int chunk_plan_assign_lpt(int64_t nchunks, const double *cost_per_chunk, int ntasks,
                          int *task_of_chunk) {
  if (nchunks < 0 || ntasks < 1)
    return -1;
  if (nchunks > 0 && (cost_per_chunk == NULL || task_of_chunk == NULL))
    return -1;

  struct ChunkCostOrder *order = NULL;
  double *task_load = NULL;
  if (nchunks > 0) {
    if ((uint64_t)nchunks > (uint64_t)(SIZE_MAX / sizeof(*order)) ||
        (size_t)ntasks > SIZE_MAX / sizeof(*task_load))
      return -1;
    order = malloc((size_t)nchunks * sizeof(*order));
    task_load = calloc((size_t)ntasks, sizeof(*task_load));
    if (order == NULL || task_load == NULL) {
      free(order);
      free(task_load);
      return -1;
    }
  }

  for (int64_t i = 0; i < nchunks; i++) {
    if (!isfinite(cost_per_chunk[i]) || cost_per_chunk[i] < 0.0) {
      free(order);
      free(task_load);
      return -1;
    }
    order[i].chunk_id = i;
    order[i].cost = cost_per_chunk[i];
  }

  qsort(order, (size_t)nchunks, sizeof(*order), compare_chunk_cost_desc);

  for (int64_t i = 0; i < nchunks; i++) {
    int best_task = 0;
    for (int task = 1; task < ntasks; task++) {
      if (task_load[task] < task_load[best_task])
        best_task = task;
    }

    task_of_chunk[order[i].chunk_id] = best_task;
    task_load[best_task] += order[i].cost;
  }

  free(order);
  free(task_load);
  return 0;
}
