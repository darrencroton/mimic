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

/*
 * Order the created rows for emission: grouped by the segment whose slice holds
 * their host, in creation order within each segment (a stable counting sort,
 * linear in rows, segments and created rows). On return (*placement)[0,
 * ncreated) holds created-row offsets (row - base_count) in emission order and
 * segment s owns (*placement)[(*bucket)[s], (*bucket)[s + 1]). Both are
 * MEM_HALOS allocations the caller frees. A segment reaching into the created
 * rows, two segments sharing a row, or a host in no segment is fatal, before
 * anything is emitted.
 */
static void order_created_rows(const struct FoFWorkspace *ws,
                               const struct OutputBufferSegment *segments, int64_t nsegments,
                               int64_t ncreated, int64_t **placement, int64_t **bucket) {
  int64_t *segment_of_row =
      mymalloc_cat((size_t)ws->base_count * sizeof(*segment_of_row), MEM_HALOS);
  for (int64_t r = 0; r < ws->base_count; r++) {
    segment_of_row[r] = -1;
  }
  for (int64_t s = 0; s < nsegments; s++) {
    const int64_t end = segments[s].workspace_start + segments[s].workspace_count;
    if (end > ws->base_count) {
      FATAL_ERROR("Output segment for source %" PRId64 " ends at row %" PRId64
                  ", inside the created records starting at %" PRId64,
                  segments[s].source_id, end, ws->base_count);
    }
    for (int64_t r = segments[s].workspace_start; r < end; r++) {
      if (segment_of_row[r] >= 0) {
        FATAL_ERROR("Output segments for sources %" PRId64 " and %" PRId64
                    " share workspace row %" PRId64,
                    segments[segment_of_row[r]].source_id, segments[s].source_id, r);
      }
      segment_of_row[r] = s;
    }
  }

  int64_t *counts = mymalloc_cat((size_t)(nsegments + 1) * sizeof(*counts), MEM_HALOS);
  int64_t *segment_of_created =
      mymalloc_cat((size_t)ncreated * sizeof(*segment_of_created), MEM_HALOS);
  for (int64_t s = 0; s <= nsegments; s++) {
    counts[s] = 0;
  }
  int64_t placed = 0;
  for (int64_t c = 0; c < ncreated; c++) {
    const int64_t host = ws->created_host[c];
    const int64_t s = (host >= 0 && host < ws->base_count) ? segment_of_row[host] : -1;
    segment_of_created[c] = s;
    if (s >= 0) {
      counts[s + 1]++;
      placed++;
    }
  }
  if (placed != ncreated) {
    FATAL_ERROR("Marshalled %" PRId64 " of %" PRId64 " created records: a created record's host "
                "lies in no output segment",
                placed, ncreated);
  }

  /* counts becomes the bucket starts; segment_of_row is reused as each
   * segment's fill cursor (nsegments may exceed base_count, so it is resized). */
  for (int64_t s = 0; s < nsegments; s++) {
    counts[s + 1] += counts[s];
  }
  int64_t *cursor =
      myrealloc_cat(segment_of_row, (size_t)(nsegments + 1) * sizeof(*cursor), MEM_HALOS);
  for (int64_t s = 0; s < nsegments; s++) {
    cursor[s] = counts[s];
  }
  int64_t *order = mymalloc_cat((size_t)ncreated * sizeof(*order), MEM_HALOS);
  for (int64_t c = 0; c < ncreated; c++) {
    order[cursor[segment_of_created[c]]++] = c;
  }

  myfree(cursor);
  myfree(segment_of_created);
  *placement = order;
  *bucket = counts;
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

  /* Validated up front so a bad segment is reported before it is mapped. */
  for (int64_t s = 0; s < nsegments; s++) {
    validate_segment(&segments[s]);
  }

  int64_t *placement = NULL;
  int64_t *bucket = NULL;
  if (ncreated > 0) {
    order_created_rows(ws, segments, nsegments, ncreated, &placement, &bucket);
  }

  for (int64_t s = 0; s < nsegments; s++) {
    struct OutputBufferSegment *segment = &segments[s];

    segment->output_first = buffer->count;
    segment->output_count = 0;

    const int64_t end = segment->workspace_start + segment->workspace_count;
    for (int64_t p = segment->workspace_start; p < end; p++) {
      emit_workspace_row(&workspace[p], buffer, segment);
    }

    /* Then the records created on hosts in this slice, in creation order, so
     * the source halo's output range stays contiguous and next-snapshot
     * gathering inherits them with it. */
    if (ncreated > 0) {
      for (int64_t i = bucket[s]; i < bucket[s + 1]; i++) {
        emit_workspace_row(&workspace[ws->base_count + placement[i]], buffer, segment);
      }
    }
  }

  myfree(placement);
  myfree(bucket);

  /* Record the capacity this buffer actually reached, for the run memory
   * profile. Noted after the loop rather than inside the growth branch so a
   * buffer that never grew still reports the capacity it was seeded at -- that
   * capacity is resident either way. */
  run_profile_note_output_buffer(buffer->count, buffer->capacity, sizeof(struct Halo));
}
