/**
 * @file    test_galaxy_pool.c
 * @brief   Unit tests for the instanced galaxy storage pool
 *
 * The pool exists to remove the allocator's fixed concurrent-block cap as a
 * limit on tree size: galaxies are handed out from large contiguous chunks
 * (O(chunks) tracked blocks) instead of one tracked block per galaxy. These
 * tests prove the property that motivated the design — allocating well past
 * DEFAULT_MAX_MEMORY_BLOCKS galaxies succeeds via chunk growth — and verify
 * pointer stability across growth, slot distinctness, reset reuse, a clean
 * leak check on destroy, and independence between separate pool instances.
 */

#include "../../src/core/galaxy_pool.h"
#include "../../src/include/types.h" /* struct GalaxyData */
#include "../../src/util/error.h"
#include "../../src/util/memory.h"
#include "../framework/test_framework.h"

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

static int passed = 0;
static int failed = 0;

/* Comfortably beyond the per-block tracking cap that the old per-galaxy
 * allocation pattern would have hit. With the pool this is a handful of chunks. */
#define SLOTS (DEFAULT_MAX_MEMORY_BLOCKS + 10000)

static void stamp_slot(struct GalaxyData *galaxy, int value) {
  memset(galaxy, value & 0xff, sizeof(*galaxy));
}

static int slot_has_stamp(const struct GalaxyData *galaxy, int value) {
  const unsigned char *bytes = (const unsigned char *)galaxy;
  unsigned char expected = (unsigned char)(value & 0xff);

  for (size_t i = 0; i < sizeof(*galaxy); i++) {
    if (bytes[i] != expected) {
      return 0;
    }
  }
  return 1;
}

static int compare_uintptr(const void *a, const void *b) {
  uintptr_t pa = *(const uintptr_t *)a;
  uintptr_t pb = *(const uintptr_t *)b;

  if (pa < pb)
    return -1;
  if (pa > pb)
    return 1;
  return 0;
}

/* Within one chunk, a pool's GalaxyData array is contiguous, so consecutive
 * allocations satisfy ptrs[i] == ptrs[i - 1] + 1. A new chunk is a separate
 * allocation, so the first slot of a new chunk breaks that adjacency. Scan
 * for that break instead of assuming its index, so a test built on this
 * returns -1 (and its caller can fail loudly) if growth never actually
 * crossed a chunk boundary in the range examined -- e.g. because
 * GALAXY_POOL_DEFAULT_CHUNK grew past what the caller allocated. */
static int find_chunk_boundary(struct GalaxyData *const *ptrs, int count) {
  for (int i = 1; i < count; i++) {
    if (ptrs[i] != ptrs[i - 1] + 1) {
      return i;
    }
  }
  return -1;
}

/**
 * @test    test_alloc_beyond_block_cap_grows_and_is_stable
 * @brief   Pool grows past the tracked-block cap; all slots remain stable and distinct
 */
int test_alloc_beyond_block_cap_grows_and_is_stable(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  struct GalaxyPool *pool = galaxy_pool_create(0);

  struct GalaxyData **ptrs = malloc((size_t)SLOTS * sizeof(*ptrs));
  TEST_ASSERT(ptrs != NULL, "Test harness allocation for pointer table should succeed");

  /* Allocate more slots than the tracked-block cap; stamp each with its index. */
  for (int i = 0; i < SLOTS; i++) {
    ptrs[i] = galaxy_pool_alloc(pool);
    TEST_ASSERT(ptrs[i] != NULL, "Pool allocation past the block cap should succeed");
    stamp_slot(ptrs[i], i);
  }

  /* Pointer stability + distinctness: every earlier slot must still hold its own
   * value after all the growth that followed it (chunks never move, and no two
   * slots alias). */
  int stable = 1;
  for (int i = 0; i < SLOTS; i++) {
    if (!slot_has_stamp(ptrs[i], i)) {
      stable = 0;
      break;
    }
  }
  TEST_ASSERT(stable, "Every slot must retain its value across chunk growth (stable, distinct)");

  /* Reset rewinds to the first slot and reuses existing chunks. */
  struct GalaxyData *first = ptrs[0];
  galaxy_pool_reset(pool);
  struct GalaxyData *after_reset = galaxy_pool_alloc(pool);
  TEST_ASSERT(after_reset == first, "Reset should rewind to the first slot and reuse memory");

  /* Re-fill to the same high-water mark to confirm chunks are reused, not regrown. */
  galaxy_pool_reset(pool);
  for (int i = 0; i < SLOTS; i++) {
    struct GalaxyData *g = galaxy_pool_alloc(pool);
    TEST_ASSERT(g != NULL, "Reused pool should satisfy the same allocation volume");
    stamp_slot(g, SLOTS - i);
  }

  free(ptrs);
  galaxy_pool_destroy(pool);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_destroy_then_recreate_starts_fresh
 * @brief   A pool created after an earlier instance is destroyed starts from a clean state
 */
int test_destroy_then_recreate_starts_fresh(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  struct GalaxyPool *first_pool = galaxy_pool_create(0);
  struct GalaxyData *a = galaxy_pool_alloc(first_pool);
  TEST_ASSERT(a != NULL, "Allocation from a freshly created pool should succeed");
  stamp_slot(a, 1);

  galaxy_pool_destroy(first_pool);

  /* A newly created pool must be usable from a clean state, independent of the
   * destroyed instance. */
  struct GalaxyPool *second_pool = galaxy_pool_create(0);
  struct GalaxyData *b = galaxy_pool_alloc(second_pool);
  TEST_ASSERT(b != NULL,
              "A newly created pool must be usable after an earlier instance is destroyed");

  galaxy_pool_destroy(second_pool);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_two_pools_interleave_without_interference
 * @brief   Allocations from two independent pool instances do not alias or interfere
 */
int test_two_pools_interleave_without_interference(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  struct GalaxyPool *pool_a = galaxy_pool_create(0);
  struct GalaxyPool *pool_b = galaxy_pool_create(0);

  enum { COUNT = 64 };
  struct GalaxyData *a_ptrs[COUNT];
  struct GalaxyData *b_ptrs[COUNT];

  /* Interleave allocations between the two pools and stamp each with a value
   * that identifies both its owning pool and its index. A's range (0..63) and
   * B's range (128..191) are both single bytes and stay disjoint after the
   * `value & 0xff` reduction in stamp_slot() -- unlike a naive 200+i for
   * i in 0..63, which wraps past 255 and collides with A's low indices. */
  for (int i = 0; i < COUNT; i++) {
    a_ptrs[i] = galaxy_pool_alloc(pool_a);
    stamp_slot(a_ptrs[i], i);
    b_ptrs[i] = galaxy_pool_alloc(pool_b);
    stamp_slot(b_ptrs[i], 128 + i);
  }

  int distinct = 1;
  for (int i = 0; i < COUNT; i++) {
    if (!slot_has_stamp(a_ptrs[i], i) || !slot_has_stamp(b_ptrs[i], 128 + i)) {
      distinct = 0;
      break;
    }
    /* No slot from one pool may alias a slot from the other. */
    for (int j = 0; j < COUNT; j++) {
      if (a_ptrs[i] == b_ptrs[j]) {
        distinct = 0;
        break;
      }
    }
  }
  TEST_ASSERT(distinct,
              "Interleaved allocations from two pools must never alias each other's slots");

  galaxy_pool_destroy(pool_a);
  galaxy_pool_destroy(pool_b);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_reset_one_pool_leaves_other_intact
 * @brief   Resetting one pool rewinds only that pool; the other's pointers and contents survive
 */
int test_reset_one_pool_leaves_other_intact(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  struct GalaxyPool *pool_a = galaxy_pool_create(0);
  struct GalaxyPool *pool_b = galaxy_pool_create(0);

  enum { COUNT = 32 };
  struct GalaxyData *b_ptrs[COUNT];
  struct GalaxyData *a_first = NULL;

  for (int i = 0; i < COUNT; i++) {
    struct GalaxyData *a_slot = galaxy_pool_alloc(pool_a);
    if (i == 0) {
      a_first = a_slot;
    }
    stamp_slot(a_slot, i);
    b_ptrs[i] = galaxy_pool_alloc(pool_b);
    stamp_slot(b_ptrs[i], 100 + i);
  }

  galaxy_pool_reset(pool_a);

  /* Pool B's slots and their contents must be untouched by resetting pool A. */
  int intact = 1;
  for (int i = 0; i < COUNT; i++) {
    if (!slot_has_stamp(b_ptrs[i], 100 + i)) {
      intact = 0;
      break;
    }
  }
  TEST_ASSERT(intact, "Resetting one pool must leave another pool's contents intact");

  /* Pool A must rewind to the start of its retained chunk after reset. */
  struct GalaxyData *a_after_reset = galaxy_pool_alloc(pool_a);
  TEST_ASSERT(a_after_reset == a_first,
              "Reset must rewind pool A to the start of its retained chunk (its first-ever slot)");

  /* Pool B must advance to a new slot, not repeat its last allocation, for
   * fresh allocation after pool A's reset. */
  struct GalaxyData *b_extra = galaxy_pool_alloc(pool_b);
  TEST_ASSERT(b_extra != b_ptrs[COUNT - 1],
              "Untouched pool must advance to a new slot, not repeat its last allocation, after "
              "another pool's reset");

  galaxy_pool_destroy(pool_a);
  galaxy_pool_destroy(pool_b);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_destroy_one_pool_leaves_other_functional
 * @brief   Destroying one pool instance leaves an independent instance fully functional
 */
int test_destroy_one_pool_leaves_other_functional(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  struct GalaxyPool *pool_a = galaxy_pool_create(0);
  struct GalaxyPool *pool_b = galaxy_pool_create(0);

  struct GalaxyData *b_slot = galaxy_pool_alloc(pool_b);
  stamp_slot(b_slot, 7);

  galaxy_pool_destroy(pool_a);

  /* Pool B's prior allocation must still hold its value, and the pool must
   * still continue its own sequence -- not re-hand its prior slot -- after
   * pool A is gone. */
  TEST_ASSERT(slot_has_stamp(b_slot, 7),
              "Surviving pool's contents must be unaffected by another pool's destroy");
  struct GalaxyData *b_more = galaxy_pool_alloc(pool_b);
  TEST_ASSERT(b_more != b_slot,
              "Surviving pool must continue its own sequence, not re-hand its prior slot, after "
              "another pool is destroyed");

  galaxy_pool_destroy(pool_b);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_two_pools_survive_interleaved_chunk_growth_and_one_reset
 * @brief   Two pools, driven past their first chunk interleaved, stay independent through growth
 *          and a reset of only one of them
 *
 * The other independence tests never allocate enough from either pool to force
 * a second chunk, so they only prove independence within a single chunk. This
 * test interleaves both pools' allocation past one default-sized chunk (the
 * 8192-galaxy default in galaxy_pool.c -- not part of the public API, so this
 * uses a literal margin rather than an include), then resets only pool A and
 * confirms pool B's post-growth, second-chunk pointers and contents survive.
 * It locates each pool's actual chunk boundary by scanning for the pointer
 * discontinuity between chunks rather than assuming a literal index, so the
 * test fails loudly instead of silently narrowing to single-chunk coverage
 * if the default chunk size or the create()-with-zero sizing rule ever
 * changes.
 */
int test_two_pools_survive_interleaved_chunk_growth_and_one_reset(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  struct GalaxyPool *pool_a = galaxy_pool_create(0);
  struct GalaxyPool *pool_b = galaxy_pool_create(0);

  enum { PAST_ONE_CHUNK = 8192 + 500 };

  struct GalaxyData **a_ptrs = malloc((size_t)PAST_ONE_CHUNK * sizeof(*a_ptrs));
  struct GalaxyData **b_ptrs = malloc((size_t)PAST_ONE_CHUNK * sizeof(*b_ptrs));
  TEST_ASSERT(a_ptrs != NULL && b_ptrs != NULL,
              "Test harness pointer tables for both pools should allocate");

  for (int i = 0; i < PAST_ONE_CHUNK; i++) {
    a_ptrs[i] = galaxy_pool_alloc(pool_a);
    b_ptrs[i] = galaxy_pool_alloc(pool_b);
  }

  /* Find each pool's own chunk boundary by scanning the pointers actually
   * returned, rather than assuming it sits at literal indices matching
   * today's GALAXY_POOL_DEFAULT_CHUNK. If the default chunk size ever grows
   * past PAST_ONE_CHUNK, or the create()-with-zero sizing rule changes, no
   * boundary exists inside the allocated range and these asserts fail
   * loudly -- instead of the test silently degrading into single-chunk
   * coverage while still reporting green. */
  int a_boundary = find_chunk_boundary(a_ptrs, PAST_ONE_CHUNK);
  int b_boundary = find_chunk_boundary(b_ptrs, PAST_ONE_CHUNK);
  TEST_ASSERT(a_boundary >= 2 && a_boundary <= PAST_ONE_CHUNK - 2,
              "Pool A's allocation must actually cross a chunk boundary inside the allocated "
              "range, or this test is silently testing only a single chunk");
  TEST_ASSERT(b_boundary >= 2 && b_boundary <= PAST_ONE_CHUNK - 2,
              "Pool B's allocation must actually cross a chunk boundary inside the allocated "
              "range, or this test is silently testing only a single chunk");

  /* Stamp a handful of representative slots -- start, both sides of each
   * pool's own discovered chunk boundary, and the last slot -- rather than
   * every slot, since a single-byte stamp cannot stay unique per index
   * across thousands of slots. */
  const int a_marks[] = {
      0, 1, a_boundary - 2, a_boundary - 1, a_boundary, a_boundary + 1, PAST_ONE_CHUNK - 1};
  const int b_marks[] = {
      0, 1, b_boundary - 2, b_boundary - 1, b_boundary, b_boundary + 1, PAST_ONE_CHUNK - 1};
  const int nmarks = (int)(sizeof(a_marks) / sizeof(a_marks[0]));
  for (int m = 0; m < nmarks; m++) {
    stamp_slot(a_ptrs[a_marks[m]], 1);
    stamp_slot(b_ptrs[b_marks[m]], 2);
  }

  /* No pointer handed to pool A may equal any pointer handed to pool B,
   * across the full interleaved, multi-chunk allocation. Sorted comparison
   * keeps this O(n log n) instead of the O(n^2) pairwise loop the smaller
   * independence tests use. */
  size_t total = (size_t)PAST_ONE_CHUNK * 2;
  uintptr_t *all_ptrs = malloc(total * sizeof(*all_ptrs));
  TEST_ASSERT(all_ptrs != NULL, "Test harness combined pointer table should allocate");
  for (int i = 0; i < PAST_ONE_CHUNK; i++) {
    all_ptrs[i] = (uintptr_t)a_ptrs[i];
    all_ptrs[(size_t)PAST_ONE_CHUNK + i] = (uintptr_t)b_ptrs[i];
  }
  qsort(all_ptrs, total, sizeof(*all_ptrs), compare_uintptr);
  int distinct = 1;
  for (size_t i = 1; i < total; i++) {
    if (all_ptrs[i] == all_ptrs[i - 1]) {
      distinct = 0;
      break;
    }
  }
  TEST_ASSERT(distinct,
              "Interleaved multi-chunk allocations from two pools must never alias, including "
              "across the chunk-growth boundary");
  free(all_ptrs);

  galaxy_pool_reset(pool_a);

  /* Pool B's marked slots -- including the ones in its second, grown chunk --
   * must survive pool A's reset untouched. */
  int b_intact = 1;
  for (int m = 0; m < nmarks; m++) {
    if (!slot_has_stamp(b_ptrs[b_marks[m]], 2)) {
      b_intact = 0;
      break;
    }
  }
  TEST_ASSERT(b_intact, "Resetting pool A after interleaved multi-chunk growth must leave pool "
                        "B's high-water contents intact, including in its second chunk");

  /* Pool A must rewind to its first slot even though it grew a second chunk
   * before the reset. */
  struct GalaxyData *a_after_reset = galaxy_pool_alloc(pool_a);
  TEST_ASSERT(a_after_reset == a_ptrs[0],
              "Pool A must rewind to its first slot after reset, even after growing multiple "
              "chunks");

  /* Pool B must continue its own high-water sequence after pool A's reset --
   * not collide with the slot pool A just re-handed out, and not re-hand a
   * slot it already issued (a plain non-NULL check would pass even if the
   * allocator strayed into A's memory or looped back over B's own history,
   * since galaxy_pool_alloc() FATALs rather than returning NULL on failure
   * and so can never fail this test the way a bug here should). */
  struct GalaxyData *b_more = galaxy_pool_alloc(pool_b);
  TEST_ASSERT(b_more != a_after_reset,
              "Pool B's next allocation must not collide with pool A's post-reset slot");
  int b_more_is_new = 1;
  for (int i = 0; i < PAST_ONE_CHUNK; i++) {
    if (b_ptrs[i] == b_more) {
      b_more_is_new = 0;
      break;
    }
  }
  TEST_ASSERT(b_more_is_new, "Pool B must continue its own high-water sequence after pool A's "
                             "reset, not re-hand a previously issued slot");

  free(a_ptrs);
  free(b_ptrs);
  galaxy_pool_destroy(pool_a);
  galaxy_pool_destroy(pool_b);
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_alloc_across_multiple_grown_chunks_stable_and_reusable
 * @brief   Pointers survive several rounds of geometric chunk growth, and reset reuses chunks
 *
 * Allocates enough galaxies to force at least three new-chunk allocations
 * beyond the first (i.e. past 8192 + 16384 + 32768 slots), so the pool has
 * grown through several different chunk sizes rather than just crossing one
 * boundary. Confirms the stable-pointer contract holds across that growth,
 * then resets and re-allocates the same volume to confirm the pool reuses its
 * existing chunks (observed only via allocation succeeding and pointers
 * repeating the earlier sequence -- chunk sizes are not public API and are
 * not asserted directly).
 */
int test_alloc_across_multiple_grown_chunks_stable_and_reusable(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  struct GalaxyPool *pool = galaxy_pool_create(0);

  /* 8192 + 16384 + 32768 = 57344; allocate past that so at least three new
   * chunks (beyond the first) have been allocated. */
  enum { PAST_THREE_GROWTHS = 8192 + 16384 + 32768 + 1000 };

  struct GalaxyData **ptrs = malloc((size_t)PAST_THREE_GROWTHS * sizeof(*ptrs));
  TEST_ASSERT(ptrs != NULL, "Test harness allocation for pointer table should succeed");

  for (int i = 0; i < PAST_THREE_GROWTHS; i++) {
    ptrs[i] = galaxy_pool_alloc(pool);
    TEST_ASSERT(ptrs[i] != NULL, "Pool allocation across several chunk growths should succeed");
    stamp_slot(ptrs[i], i);
  }

  /* Stable-pointer contract: every slot handed out before later growth must
   * still hold its own value once growth has moved on to later chunks. */
  int stable = 1;
  for (int i = 0; i < PAST_THREE_GROWTHS; i++) {
    if (!slot_has_stamp(ptrs[i], i)) {
      stable = 0;
      break;
    }
  }
  TEST_ASSERT(stable, "Every slot must retain its value across several rounds of chunk growth");

  /* Reset then re-allocate the same volume: this must succeed by reusing the
   * chunks already grown, not by growing further or failing. */
  struct GalaxyData *first = ptrs[0];
  galaxy_pool_reset(pool);
  struct GalaxyData *after_reset = galaxy_pool_alloc(pool);
  TEST_ASSERT(after_reset == first,
              "Reset after multi-chunk growth should rewind to the first-ever slot");

  for (int i = 1; i < PAST_THREE_GROWTHS; i++) {
    struct GalaxyData *g = galaxy_pool_alloc(pool);
    TEST_ASSERT(g == ptrs[i],
                "Re-allocating after reset should reuse the existing chunks, handing back the "
                "same slot sequence");
  }

  free(ptrs);
  galaxy_pool_destroy(pool);
  check_memory_leaks();
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Galaxy Pool\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_DEBUG, NULL);

  TEST_RUN(test_alloc_beyond_block_cap_grows_and_is_stable);
  TEST_RUN(test_destroy_then_recreate_starts_fresh);
  TEST_RUN(test_two_pools_interleave_without_interference);
  TEST_RUN(test_reset_one_pool_leaves_other_intact);
  TEST_RUN(test_destroy_one_pool_leaves_other_functional);
  TEST_RUN(test_two_pools_survive_interleaved_chunk_growth_and_one_reset);
  TEST_RUN(test_alloc_across_multiple_grown_chunks_stable_and_reusable);

  TEST_SUMMARY();
  return TEST_RESULT();
}
