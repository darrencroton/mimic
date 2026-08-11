/**
 * @file    test_output_buffer.c
 * @brief   Unit tests for driver-neutral output buffer marshalling
 */

#include "../../src/core/galaxy_pool.h"
#include "../../src/core/output_buffer.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"
#include "../framework/test_framework.h"

#include <stdio.h>
#include <string.h>

static int passed = 0;
static int failed = 0;

static void init_halo(struct Halo *halo, int type, int halo_nr) {
  memset(halo, 0, sizeof(*halo));
  halo->Type = type;
  halo->HaloNr = halo_nr;
  halo->SnapNum = -1;
  halo->CentralMvir = -1.0f;
}

/**
 * @test    test_copies_non_type3_and_sets_segment_fields
 * @brief   Non-Type-3 halos are copied and segment output_first/output_count are set
 */
int test_copies_non_type3_and_sets_segment_fields(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  struct Halo workspace[3];
  struct Halo output[3];
  struct OutputBuffer buffer = {output, 0, 3};
  struct OutputBufferSegment segment = {
      .source_id = 42,
      .snapshot_number = 5,
      .workspace_start = 0,
      .workspace_count = 3,
      .output_first = -1,
      .output_count = -1,
  };
  memset(output, 0, sizeof(output));
  init_halo(&workspace[0], 0, 42);
  init_halo(&workspace[1], 1, 42);
  init_halo(&workspace[2], 2, 42);
  /* CentralMvir is stamped by the driver before marshalling; the marshaller
   * must carry it through unchanged by struct copy. */
  for (int i = 0; i < 3; i++)
    workspace[i].CentralMvir = 123.5f;

  marshal_workspace_to_output_buffer(workspace, &buffer, &segment, 1);

  TEST_ASSERT(buffer.count == 3, "All non-Type-3 halos should be copied");
  TEST_ASSERT(segment.output_first == 0, "Segment output_first should use starting buffer count");
  TEST_ASSERT(segment.output_count == 3, "Segment output_count should count copied halos");
  for (int i = 0; i < 3; i++) {
    TEST_ASSERT(output[i].SnapNum == 5, "Output SnapNum should be driver supplied");
    TEST_ASSERT_DOUBLE_EQUAL(output[i].CentralMvir, 123.5, 1e-6,
                             "Output CentralMvir should be carried through by struct copy");
  }

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_skips_type3_and_clears_galaxy_pointer
 * @brief   Type-3 halos are skipped and their galaxy pointer is cleared without freeing
 */
int test_skips_type3_and_clears_galaxy_pointer(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  struct Halo workspace[2];
  struct Halo output[2];
  struct OutputBuffer buffer = {output, 0, 2};
  struct OutputBufferSegment segment = {
      .source_id = 7,
      .snapshot_number = 9,
      .workspace_start = 0,
      .workspace_count = 2,
      .output_first = -1,
      .output_count = -1,
  };
  memset(output, 0, sizeof(output));
  init_halo(&workspace[0], 3, 7);
  init_halo(&workspace[1], 1, 7);
  /* Galaxy memory is owned by the pool; the marshaller must clear (not free) the
   * Type-3 pointer and leave reclamation to the per-tree pool reset. */
  struct GalaxyPool *pool = galaxy_pool_create(0);
  workspace[0].galaxy = galaxy_pool_alloc(pool);
  memset(workspace[0].galaxy, 0, sizeof(struct GalaxyData));

  marshal_workspace_to_output_buffer(workspace, &buffer, &segment, 1);

  TEST_ASSERT(buffer.count == 1, "Type 3 halo should not be copied");
  TEST_ASSERT(segment.output_first == 0, "Segment output_first should be set");
  TEST_ASSERT(segment.output_count == 1, "Only one non-Type-3 halo should be counted");
  TEST_ASSERT(workspace[0].galaxy == NULL, "Type 3 galaxy pointer should be cleared");
  TEST_ASSERT(output[0].Type == 1, "Copied halo should be the surviving non-Type-3 entry");

  galaxy_pool_destroy(pool);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_empty_segment_records_zero_count
 * @brief   A segment with workspace_count 0 records output_count 0 and does not copy
 */
int test_empty_segment_records_zero_count(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  struct Halo workspace[1];
  struct Halo output[1];
  struct OutputBuffer buffer = {output, 0, 1};
  struct OutputBufferSegment segment = {
      .source_id = 99,
      .snapshot_number = 2,
      .workspace_start = 0,
      .workspace_count = 0,
      .output_first = -1,
      .output_count = -1,
  };
  memset(workspace, 0, sizeof(workspace));
  memset(output, 0, sizeof(output));

  marshal_workspace_to_output_buffer(workspace, &buffer, &segment, 1);

  TEST_ASSERT(buffer.count == 0, "Empty segment should not copy halos");
  TEST_ASSERT(segment.output_first == 0, "Empty segment should record current buffer count");
  TEST_ASSERT(segment.output_count == 0, "Empty segment should record zero output halos");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_multiple_segments_accumulate_into_one_buffer
 * @brief   Multiple segments append into one buffer; output_first tracks the running count
 *
 * The tree driver fills one shared buffer from several segments in sequence.
 * Each segment's output_first must pick up the running buffer count, including
 * across a Type-3 skip, so progenitor lookup (HaloAux ranges) stays correct.
 */
int test_multiple_segments_accumulate_into_one_buffer(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  struct Halo workspace[4];
  struct Halo output[4];
  struct OutputBuffer buffer = {output, 0, 4};
  struct OutputBufferSegment segments[2] = {
      {.source_id = 10,
       .snapshot_number = 3,
       .workspace_start = 0,
       .workspace_count = 2,
       .output_first = -1,
       .output_count = -1},
      {.source_id = 11,
       .snapshot_number = 3,
       .workspace_start = 2,
       .workspace_count = 2,
       .output_first = -1,
       .output_count = -1},
  };
  memset(output, 0, sizeof(output));
  /* Segment 0: one Type-3 (skipped) + one survivor. */
  init_halo(&workspace[0], 3, 10);
  init_halo(&workspace[1], 0, 10);
  /* Segment 1: two survivors. */
  init_halo(&workspace[2], 1, 11);
  init_halo(&workspace[3], 2, 11);

  marshal_workspace_to_output_buffer(workspace, &buffer, segments, 2);

  TEST_ASSERT(buffer.count == 3, "Three non-Type-3 halos across both segments");
  TEST_ASSERT(segments[0].output_first == 0, "First segment starts at buffer index 0");
  TEST_ASSERT(segments[0].output_count == 1, "First segment contributes one survivor");
  TEST_ASSERT(segments[1].output_first == 1,
              "Second segment must continue from the running buffer count");
  TEST_ASSERT(segments[1].output_count == 2, "Second segment contributes two survivors");
  TEST_ASSERT(output[0].HaloNr == 10, "Buffer slot 0 is the segment-0 survivor");
  TEST_ASSERT(output[1].HaloNr == 11 && output[2].HaloNr == 11,
              "Slots 1-2 are the segment-1 survivors");

  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_buffer_grows_when_capacity_exceeded
 * @brief   Exceeding initial capacity triggers realloc; all halos land at correct indices
 *
 * Uses a heap-backed buffer (mymalloc_cat) as required by the contract; checks
 * capacity grew, all halos landed at correct indices, and segment metadata is
 * correct across the growth boundary.
 */
int test_buffer_grows_when_capacity_exceeded(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  const int initial_capacity = 2;
  struct Halo *heap_halos = mymalloc_cat(initial_capacity * sizeof(struct Halo), MEM_HALOS);
  memset(heap_halos, 0, initial_capacity * sizeof(struct Halo));

  struct OutputBuffer buffer = {heap_halos, 0, initial_capacity};
  struct OutputBufferSegment segments[2] = {
      {.source_id = 10,
       .snapshot_number = 4,
       .workspace_start = 0,
       .workspace_count = 3,
       .output_first = -1,
       .output_count = -1},
      {.source_id = 11,
       .snapshot_number = 4,
       .workspace_start = 3,
       .workspace_count = 2,
       .output_first = -1,
       .output_count = -1},
  };

  struct Halo workspace[5];
  for (int i = 0; i < 5; i++)
    init_halo(&workspace[i], i % 2, i); /* alternating Type 0/1, none Type 3 */

  marshal_workspace_to_output_buffer(workspace, &buffer, segments, 2);

  TEST_ASSERT(buffer.count == 5, "All 5 halos should be present after growth");
  TEST_ASSERT(buffer.capacity > initial_capacity, "Buffer capacity should have grown");
  TEST_ASSERT(segments[0].output_first == 0, "First segment output_first should be 0");
  TEST_ASSERT(segments[0].output_count == 3, "First segment should contribute 3 halos");
  TEST_ASSERT(segments[1].output_first == 3,
              "Second segment output_first must reflect running count after growth");
  TEST_ASSERT(segments[1].output_count == 2, "Second segment should contribute 2 halos");
  for (int i = 0; i < 5; i++) {
    TEST_ASSERT(buffer.halos[i].SnapNum == 4, "Each output halo should carry the segment snapshot");
    TEST_ASSERT(buffer.halos[i].HaloNr == i, "Output halo order should be preserved");
  }

  myfree(buffer.halos);
  check_memory_leaks();
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Output Buffer\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  TEST_RUN(test_copies_non_type3_and_sets_segment_fields);
  TEST_RUN(test_skips_type3_and_clears_galaxy_pointer);
  TEST_RUN(test_empty_segment_records_zero_count);
  TEST_RUN(test_multiple_segments_accumulate_into_one_buffer);
  TEST_RUN(test_buffer_grows_when_capacity_exceeded);

  TEST_SUMMARY();
  return TEST_RESULT();
}
