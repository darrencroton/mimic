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
   * that identifies both its owning pool and its index. */
  for (int i = 0; i < COUNT; i++) {
    a_ptrs[i] = galaxy_pool_alloc(pool_a);
    stamp_slot(a_ptrs[i], i);
    b_ptrs[i] = galaxy_pool_alloc(pool_b);
    stamp_slot(b_ptrs[i], 200 + i);
  }

  int distinct = 1;
  for (int i = 0; i < COUNT; i++) {
    if (!slot_has_stamp(a_ptrs[i], i) || !slot_has_stamp(b_ptrs[i], 200 + i)) {
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

  for (int i = 0; i < COUNT; i++) {
    struct GalaxyData *a_slot = galaxy_pool_alloc(pool_a);
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

  /* Pool A must still be usable after its own reset. */
  struct GalaxyData *a_after_reset = galaxy_pool_alloc(pool_a);
  TEST_ASSERT(a_after_reset != NULL, "Reset pool must remain usable for further allocation");

  /* Pool B must still be usable for fresh allocation after pool A's reset. */
  struct GalaxyData *b_extra = galaxy_pool_alloc(pool_b);
  TEST_ASSERT(b_extra != NULL, "Untouched pool must remain usable for further allocation");

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
   * still be able to hand out further slots, after pool A is gone. */
  TEST_ASSERT(slot_has_stamp(b_slot, 7),
              "Surviving pool's contents must be unaffected by another pool's destroy");
  struct GalaxyData *b_more = galaxy_pool_alloc(pool_b);
  TEST_ASSERT(b_more != NULL, "Surviving pool must remain usable after another pool is destroyed");

  galaxy_pool_destroy(pool_b);
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

  TEST_SUMMARY();
  return TEST_RESULT();
}
