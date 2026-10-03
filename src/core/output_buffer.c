/**
 * @file    output_buffer.c
 * @brief   Driver-neutral marshalling from processed workspaces to output state
 */

#include <assert.h>
#include <inttypes.h>

#include "constants.h"
#include "error.h"
#include "fof_workspace.h"
#include "memory.h"
#include "output_buffer.h"
#include "run_profile.h"

static void validate_segment(const struct OutputBufferSegment *segment) {
  if (segment->workspace_start < 0 || segment->workspace_count < 0) {
    FATAL_ERROR("Invalid output segment for source %" PRId64 ": start=%" PRId64 " count=%" PRId64,
                segment->source_id, segment->workspace_start, segment->workspace_count);
  }
}

/* Append one workspace row to the segment's output range, unless it is Type 3. */
static void emit_workspace_row(struct Halo *halo, struct OutputBuffer *buffer,
                               struct OutputBufferSegment *segment) {
  if (halo->Type == 3) {
    /* Type 3 halos are not emitted. The galaxy pool owns the galaxy memory
     * and reclaims it on the per-tree reset, so we only clear the pointer. */
    halo->galaxy = NULL;
    return;
  }

  if (buffer->count >= buffer->capacity) {
    /* Growth arithmetic mirrors fof_workspace_reserve() in fof_workspace.c. */
    int64_t new_capacity = (int64_t)(buffer->capacity * HALO_ARRAY_GROWTH_FACTOR);
    if (new_capacity - buffer->capacity < MIN_HALO_ARRAY_GROWTH)
      new_capacity = buffer->capacity + MIN_HALO_ARRAY_GROWTH;
    if (new_capacity > MAX_HALO_ARRAY_SIZE)
      new_capacity = MAX_HALO_ARRAY_SIZE;
    if (new_capacity <= buffer->capacity)
      FATAL_ERROR("ProcessedHalos cannot grow beyond %d elements (source %" PRId64 ")",
                  MAX_HALO_ARRAY_SIZE, segment->source_id);
    buffer->halos =
        myrealloc_cat(buffer->halos, (size_t)new_capacity * sizeof(struct Halo), MEM_HALOS);
    buffer->capacity = new_capacity;
  }

  halo->SnapNum = segment->snapshot_number;
  buffer->halos[buffer->count++] = *halo;
  segment->output_count++;
}

void marshal_workspace_to_output_buffer(const struct FoFWorkspace *ws, struct OutputBuffer *buffer,
                                        struct OutputBufferSegment *segments, int64_t nsegments) {
  assert(ws != NULL);
  assert(ws->halos != NULL);
  struct Halo *workspace = ws->halos;
  assert(buffer != NULL);
  assert(buffer->halos != NULL);
  assert(segments != NULL || nsegments == 0);

  /* Rows [base_count, count) are records created by the module pipeline; a
   * descriptor that never recorded a host has none (fof_workspace.h). */
  const int64_t ncreated = (ws->created_host != NULL) ? ws->count - ws->base_count : 0;
  if (ncreated < 0 || ncreated > ws->created_capacity) {
    FATAL_ERROR("FoF workspace created rows are inconsistent: count=%" PRId64
                ", base_count=%" PRId64 ", created_capacity=%" PRId64,
                ws->count, ws->base_count, ws->created_capacity);
  }
  int64_t created_placed = 0;

  for (int64_t s = 0; s < nsegments; s++) {
    struct OutputBufferSegment *segment = &segments[s];
    validate_segment(segment);

    segment->output_first = buffer->count;
    segment->output_count = 0;

    const int64_t end = segment->workspace_start + segment->workspace_count;
    for (int64_t p = segment->workspace_start; p < end; p++) {
      emit_workspace_row(&workspace[p], buffer, segment);
    }

    /* Then the records created on hosts in this slice, in creation order, so
     * the source halo's output range stays contiguous and next-snapshot
     * gathering inherits them with it. The scan is over every created row
     * per segment, which costs nothing when no record was created. */
    for (int64_t c = 0; c < ncreated; c++) {
      const int64_t host = ws->created_host[c];
      if (host >= segment->workspace_start && host < end) {
        emit_workspace_row(&workspace[ws->base_count + c], buffer, segment);
        created_placed++;
      }
    }
  }

  if (created_placed != ncreated) {
    FATAL_ERROR("Marshalled %" PRId64 " of %" PRId64 " created records: a created record's host "
                "lies in no output segment (or in more than one)",
                created_placed, ncreated);
  }

  /* Record the capacity this buffer actually reached, for the run memory
   * profile. Noted after the loop rather than inside the growth branch so a
   * buffer that never grew still reports the capacity it was seeded at -- that
   * capacity is resident either way. */
  run_profile_note_output_buffer(buffer->count, buffer->capacity, sizeof(struct Halo));
}
