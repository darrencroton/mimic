#ifndef CORE_OUTPUT_BUFFER_H
#define CORE_OUTPUT_BUFFER_H

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

void marshal_workspace_to_output_buffer(struct Halo *workspace, struct OutputBuffer *buffer,
                                        struct OutputBufferSegment *segments, int nsegments);

#endif /* CORE_OUTPUT_BUFFER_H */
