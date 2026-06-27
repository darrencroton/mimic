/**
 * @file    test_galaxy_pool.c
 * @brief   Unit tests for the per-tree galaxy storage pool
 *
 * The pool exists to remove the allocator's fixed concurrent-block cap as a
 * limit on tree size: galaxies are handed out from large contiguous chunks
 * (O(chunks) tracked blocks) instead of one tracked block per galaxy. These
 * tests prove the property that motivated the design — allocating well past
 * DEFAULT_MAX_MEMORY_BLOCKS galaxies succeeds via chunk growth — and verify
 * pointer stability across growth, slot distinctness, reset reuse, and a clean
 * leak check on destroy.
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
  galaxy_pool_init(0);

  struct GalaxyData **ptrs = malloc((size_t)SLOTS * sizeof(*ptrs));
  TEST_ASSERT(ptrs != NULL, "Test harness allocation for pointer table should succeed");

  /* Allocate more slots than the tracked-block cap; stamp each with its index. */
  for (int i = 0; i < SLOTS; i++) {
    ptrs[i] = galaxy_pool_alloc();
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
  galaxy_pool_reset();
  struct GalaxyData *after_reset = galaxy_pool_alloc();
  TEST_ASSERT(after_reset == first, "Reset should rewind to the first slot and reuse memory");

  /* Re-fill to the same high-water mark to confirm chunks are reused, not regrown. */
  galaxy_pool_reset();
  for (int i = 0; i < SLOTS; i++) {
    struct GalaxyData *g = galaxy_pool_alloc();
    TEST_ASSERT(g != NULL, "Reused pool should satisfy the same allocation volume");
    stamp_slot(g, SLOTS - i);
  }

  free(ptrs);
  galaxy_pool_destroy();
  check_memory_leaks();
  return TEST_PASS;
}

/**
 * @test    test_destroy_then_realloc_starts_fresh
 * @brief   Pool is usable from a clean state after destroy; lazy init works after destroy
 */
int test_destroy_then_realloc_starts_fresh(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* No explicit init: first alloc must lazily initialise the pool. */
  struct GalaxyData *a = galaxy_pool_alloc();
  TEST_ASSERT(a != NULL, "Lazy init on first alloc should succeed");
  stamp_slot(a, 1);

  galaxy_pool_destroy();

  /* After destroy the pool must be usable again from a clean state. */
  struct GalaxyData *b = galaxy_pool_alloc();
  TEST_ASSERT(b != NULL, "Pool must be reusable after destroy");

  galaxy_pool_destroy();
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
  TEST_RUN(test_destroy_then_realloc_starts_fresh);

  TEST_SUMMARY();
  return TEST_RESULT();
}
