#ifndef CORE_OUTPUT_BUFFER_H
#define CORE_OUTPUT_BUFFER_H

/**
 * @file    output_buffer.h
 * @brief   Driver-neutral workspace-to-output-buffer marshalling
 *
 * marshal_workspace_to_output_buffer() transfers surviving workspace halos
 * (excluding Type 3) into the driver-owned output buffer, growing it when
 * needed, and fills each segment's output_first/output_count fields.
 * output_buffer_seed_capacity() is the seed rule both drivers size it by.
 */

#include <stdint.h>

#include "types.h"

struct FoFWorkspace; /* fof_workspace.h */

struct OutputBuffer {
  struct Halo *halos;
  int64_t count;
  int64_t capacity;
};

/* One descendant subhalo's slice of the FoF workspace. source_id is the input
 * halo index the slice came from and the workspace fields are offsets into the
 * workspace, so all are int64_t like the drivers' own halo indices. */
struct OutputBufferSegment {
  int64_t source_id;
  int snapshot_number;
  int64_t workspace_start;
  int64_t workspace_count;
  int64_t output_first;
  int64_t output_count;
};

/*
 * Copy surviving workspace halos into the output buffer, skipping Type 3 entries.
 * Each segment's output_first and output_count fields are filled in. Segments
 * address rows of ws->halos; the descriptor itself is not modified (a Type 3
 * row's galaxy pointer and every emitted row's SnapNum are written in place).
 *
 * Per segment, in segment order, the slice's surviving rows are emitted first,
 * then the surviving records created during the pipeline whose host lies in
 * that slice (ws->created_host), in creation order; output_count counts both.
 * A source halo's output range therefore stays contiguous, and FirstHalo/NHalos
 * and next-snapshot gathering need no knowledge of created records. Placement
 * is linear in rows, segments and created rows, using call-local MEM_HALOS
 * scratch only when records were created. A created record whose host lies in
 * no segment, a segment reaching into the created rows, or two segments
 * sharing a row is fatal.
 *
 * The buffer may be grown by myrealloc_cat when count reaches capacity.
 * CONTRACT: buffer->halos must be a heap allocation tracked by mymalloc_cat or
 * myrealloc_cat — stack arrays will FATAL on overflow. After calling this
 * function, callers that mirror buffer->halos and buffer->capacity in external
 * globals (e.g. ProcessedHalos / MaxProcessedHalos) must sync those back from
 * the returned struct fields.
 */
void marshal_workspace_to_output_buffer(const struct FoFWorkspace *ws, struct OutputBuffer *buffer,
                                        struct OutputBufferSegment *segments, int64_t nsegments);

/*
 * The output buffer's seed capacity for a unit of `nhalos` input halos (a tree
 * under the vertical driver, a snapshot slab under the horizontal driver): the
 * halos plus OUTPUT_SEED_HEADROOM of them, the headroom floored at
 * MIN_HALO_ARRAY_GROWTH, clamped at MAX_HALO_ARRAY_SIZE unless `nhalos` is
 * already past it, when only the minimum growth increment is added. A buffer
 * whose output outgrows the seed grows in marshal_workspace_to_output_buffer().
 * Returns 0 on int64_t overflow, which only `nhalos` within
 * MIN_HALO_ARRAY_GROWTH of INT64_MAX can reach; the caller treats it as fatal.
 *
 * Inline in the header so a harness that links a driver's loader without
 * output_buffer.c still links the rule.
 */
static inline int output_buffer_seed_capacity(int64_t nhalos, int64_t *capacity) {
  if (nhalos > MAX_HALO_ARRAY_SIZE) {
    if (MIN_HALO_ARRAY_GROWTH > INT64_MAX - nhalos) {
      return 0;
    }
    *capacity = nhalos + MIN_HALO_ARRAY_GROWTH;
    return 1;
  }

  int64_t seed_headroom = (int64_t)((double)nhalos * OUTPUT_SEED_HEADROOM);
  if (seed_headroom < MIN_HALO_ARRAY_GROWTH) {
    seed_headroom = MIN_HALO_ARRAY_GROWTH;
  }
  int64_t seed_capacity = nhalos + seed_headroom;
  if (seed_capacity > MAX_HALO_ARRAY_SIZE) {
    seed_capacity = MAX_HALO_ARRAY_SIZE;
  }
  *capacity = seed_capacity;
  return 1;
}

#endif /* CORE_OUTPUT_BUFFER_H */
