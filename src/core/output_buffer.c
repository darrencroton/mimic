/**
 * @file    output_buffer.c
 * @brief   Driver-neutral marshalling from processed workspaces to output state
 */

#include <assert.h>

#include "error.h"
#include "memory.h"
#include "output_buffer.h"

static void validate_segment(const struct OutputBufferSegment *segment) {
  if (segment->workspace_start < 0 || segment->workspace_count < 0) {
    FATAL_ERROR("Invalid output segment for source %d: start=%d count=%d", segment->source_id,
                segment->workspace_start, segment->workspace_count);
  }
}

void marshal_workspace_to_output_buffer(struct Halo *workspace, struct OutputBuffer *buffer,
                                        struct OutputBufferSegment *segments, int nsegments) {
  assert(workspace != NULL);
  assert(buffer != NULL);
  assert(buffer->halos != NULL);
  assert(segments != NULL || nsegments == 0);

  for (int s = 0; s < nsegments; s++) {
    struct OutputBufferSegment *segment = &segments[s];
    validate_segment(segment);

    segment->output_first = buffer->count;
    segment->output_count = 0;

    const int end = segment->workspace_start + segment->workspace_count;
    for (int p = segment->workspace_start; p < end; p++) {
      if (workspace[p].Type == 3) {
        if (workspace[p].galaxy != NULL) {
          myfree(workspace[p].galaxy);
          workspace[p].galaxy = NULL;
        }
        continue;
      }

      if (buffer->count >= buffer->capacity) {
        FATAL_ERROR("Output buffer capacity exceeded while marshalling source "
                    "%d (%d >= %d)",
                    segment->source_id, buffer->count, buffer->capacity);
      }

      workspace[p].SnapNum = segment->snapshot_number;
      buffer->halos[buffer->count++] = workspace[p];
      segment->output_count++;
    }
  }
}
