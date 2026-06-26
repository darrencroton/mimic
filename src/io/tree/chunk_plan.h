/**
 * @file    tree/chunk_plan.h
 * @brief   Pure helpers for output chunk boundaries and task assignment.
 *
 * Chunk boundaries are independent of MPI task count: forest ranges are
 * contiguous, global, and never empty. Task assignment is a separate LPT pass
 * over the emitted chunks.
 */

#ifndef IO_TREE_CHUNK_PLAN_H
#define IO_TREE_CHUNK_PLAN_H

#include <stdint.h>

struct ChunkPlanRange {
  int64_t start_forest;
  int64_t nforests;
  double size;
  double cost;
};

struct ChunkPlan {
  struct ChunkPlanRange *chunks;
  int64_t nchunks;
  int64_t capacity;
};

struct ChunkPlanBuilder {
  struct ChunkPlan plan;
  double size_budget;
  int64_t forests_per_file_override;
  int64_t next_forest;
  int64_t current_start;
  int64_t current_nforests;
  double current_size;
  double current_cost;
};

int chunk_plan_builder_init(struct ChunkPlanBuilder *builder, double size_budget,
                            int64_t forests_per_file_override);
/* If add_file or finish fails after partial input, call chunk_plan_free(&builder->plan). */
int chunk_plan_builder_add_file(struct ChunkPlanBuilder *builder, int64_t nforests,
                                const double *size_per_forest);
int chunk_plan_builder_add_file_with_cost(struct ChunkPlanBuilder *builder, int64_t nforests,
                                          const double *size_per_forest,
                                          const double *cost_per_forest);
int chunk_plan_builder_finish(struct ChunkPlanBuilder *builder, struct ChunkPlan *plan);
int chunk_plan_build_boundaries(int64_t nforests, const double *size_per_forest, double size_budget,
                                int64_t forests_per_file_override, struct ChunkPlan *plan);
void chunk_plan_free(struct ChunkPlan *plan);

int chunk_plan_assign_lpt(int64_t nchunks, const double *cost_per_chunk, int ntasks,
                          int *task_of_chunk);

#endif /* IO_TREE_CHUNK_PLAN_H */
