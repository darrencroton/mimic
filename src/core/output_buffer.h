#ifndef CORE_OUTPUT_BUFFER_H
#define CORE_OUTPUT_BUFFER_H

/**
 * @file    output_buffer.h
 * @brief   Driver-neutral workspace-to-output-buffer marshalling
 *
 * marshal_workspace_to_output_buffer() transfers surviving workspace halos
 * (excluding Type 3) into the driver-owned output buffer, growing it when
 * needed, and fills each segment's output_first/output_count fields.
 */

#include "types.h"

struct OutputBuffer {
  struct Halo *halos;
  int count;
  int capacity;
};

struct OutputBufferSegment {
  int source_id;
  int snapshot_number;
  int workspace_start;
  int workspace_count;
  int output_first;
  int output_count;
};

/*
 * Copy surviving workspace halos into the output buffer, skipping Type 3 entries.
 * Each segment's output_first and output_count fields are filled in.
 *
 * The buffer may be grown by myrealloc_cat when count reaches capacity.
 * CONTRACT: buffer->halos must be a heap allocation tracked by mymalloc_cat or
 * myrealloc_cat — stack arrays will FATAL on overflow. After calling this
 * function, callers that mirror buffer->halos and buffer->capacity in external
 * globals (e.g. ProcessedHalos / MaxProcessedHalos) must sync those back from
 * the returned struct fields.
 */
void marshal_workspace_to_output_buffer(struct Halo *workspace, struct OutputBuffer *buffer,
                                        struct OutputBufferSegment *segments, int nsegments);

#endif /* CORE_OUTPUT_BUFFER_H */
