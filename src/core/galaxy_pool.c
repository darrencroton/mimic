/**
 * @file    galaxy_pool.c
 * @brief   Chunked, instanced storage pool for galaxy data
 *
 * See galaxy_pool.h for the ownership model and the fully-initialise-before-read
 * invariant. Chunks are a singly linked list; each chunk holds its header and a
 * contiguous run of GalaxyData in one tracked allocation. Chunks are never moved
 * or individually freed during a run, so handed-out pointers stay valid as the
 * pool grows and across resets. Reset rewinds to the first chunk and reuses the
 * existing chunks for the next tree; destroy frees them all at shutdown.
 *
 * New-chunk sizing is geometric, not fixed: each chunk allocated to extend the
 * pool doubles the previous chunk_capacity, clamped to GALAXY_POOL_MAX_CHUNK
 * (growth is logarithmic until the cap, then linear in far larger steps). This
 * keeps the number of tracked allocator blocks per pool small — well under 200
 * at hundreds of millions of galaxies — which matters because every chunk is
 * one mymalloc_cat() block and the allocator FATALs once its block table fills
 * (see DEFAULT_MAX_MEMORY_BLOCKS in memory.h).
 *
 * This mirrors the run-persistent, grow-to-high-water scratch idiom already used
 * for the inheritance gather buffers (see ProgenitorScratch in build_model.c).
 */

#include <assert.h>

#include "galaxy_pool.h"
#include "memory.h"
#include "types.h" /* struct GalaxyData */

#define GALAXY_POOL_MIN_CHUNK 1024     /* smallest chunk we bother allocating */
#define GALAXY_POOL_DEFAULT_CHUNK 8192 /* first chunk's size when no larger hint is given */

/* Upper bound on chunk_capacity (2^22 galaxies; ~700 MB of GalaxyData at 176 B/
 * galaxy). Chunks grow geometrically to keep the tracked-block count small, but
 * an unbounded chunk would make a single mymalloc_cat() call arbitrarily large;
 * this caps that while keeping the block count well under the allocator's
 * 50,000-block table even for a ~315M-galaxy generation (well under 200 chunks
 * per pool). */
#define GALAXY_POOL_MAX_CHUNK 4194304

struct GalaxyChunk {
  struct GalaxyChunk *next;
  int capacity;
  int used;
  struct GalaxyData *data; /* points to the contiguous run just past this header */
};

struct GalaxyPool {
  struct GalaxyChunk *head;    /* first chunk */
  struct GalaxyChunk *current; /* chunk currently being filled */
  int chunk_capacity;          /* size of newly grown chunks */
  /* Cost accounting for the run memory profile. `live` counts slots handed out
   * since the last reset, so its high-water is the peak concurrent galaxy count
   * rather than a lifetime total; `slots_allocated` and `chunk_count` describe
   * what is resident. */
  int64_t live;
  int64_t live_high_water;
  int64_t slots_allocated;
  int chunk_count;
};

static struct GalaxyChunk *new_chunk(int capacity) {
  if (capacity < GALAXY_POOL_MIN_CHUNK)
    capacity = GALAXY_POOL_MIN_CHUNK;

  size_t bytes = sizeof(struct GalaxyChunk) + (size_t)capacity * sizeof(struct GalaxyData);
  struct GalaxyChunk *chunk = mymalloc_cat(bytes, MEM_GALAXIES);
  chunk->next = NULL;
  chunk->capacity = capacity;
  chunk->used = 0;
  chunk->data = (struct GalaxyData *)(chunk + 1);
  return chunk;
}

struct GalaxyPool *galaxy_pool_create(int initial_capacity) {
  struct GalaxyPool *pool = mymalloc_cat(sizeof(struct GalaxyPool), MEM_GALAXIES);

  int capacity =
      initial_capacity > GALAXY_POOL_MIN_CHUNK ? initial_capacity : GALAXY_POOL_DEFAULT_CHUNK;
  /* Clamp a caller-supplied hint the same way grown chunks are clamped, so
   * chunk_capacity never starts above the cap that galaxy_pool_alloc()
   * maintains. */
  if (capacity > GALAXY_POOL_MAX_CHUNK)
    capacity = GALAXY_POOL_MAX_CHUNK;
  pool->chunk_capacity = capacity;
  pool->live = 0;
  pool->live_high_water = 0;
  pool->slots_allocated = 0;
  pool->chunk_count = 0;
  pool->head = new_chunk(pool->chunk_capacity);
  /* Count the chunk's own capacity, not the requested one: new_chunk() raises
   * anything below GALAXY_POOL_MIN_CHUNK. */
  pool->slots_allocated += pool->head->capacity;
  pool->chunk_count++;
  pool->current = pool->head;
  return pool;
}

struct GalaxyData *galaxy_pool_alloc(struct GalaxyPool *pool) {
  assert(pool != NULL);

  if (pool->current->used >= pool->current->capacity) {
    if (pool->current->next != NULL) {
      /* Reuse a chunk left over from a previous, larger tree. */
      pool->current = pool->current->next;
      pool->current->used = 0;
    } else {
      /* Grow geometrically before allocating the new chunk: double
       * chunk_capacity, clamped to GALAXY_POOL_MAX_CHUNK. This doubling cannot
       * overflow `int` -- chunk_capacity is always <= GALAXY_POOL_MAX_CHUNK
       * (2^22) on entry, by this same clamp and by the clamp in
       * galaxy_pool_create(), so the doubled value is at most 2^23, far below
       * INT_MAX -- so no runtime overflow check is needed. */
      int new_capacity = pool->chunk_capacity * 2;
      if (new_capacity > GALAXY_POOL_MAX_CHUNK)
        new_capacity = GALAXY_POOL_MAX_CHUNK;
      pool->chunk_capacity = new_capacity;

      struct GalaxyChunk *chunk = new_chunk(pool->chunk_capacity);
      pool->slots_allocated += chunk->capacity;
      pool->chunk_count++;
      pool->current->next = chunk;
      pool->current = chunk;
    }
  }

  pool->live++;
  if (pool->live > pool->live_high_water)
    pool->live_high_water = pool->live;

  return &pool->current->data[pool->current->used++];
}

void galaxy_pool_reset(struct GalaxyPool *pool) {
  assert(pool != NULL);

  /* Rewind to the first chunk; trailing chunks are reset lazily as alloc reaches
   * them, so a single reset is O(1) regardless of how many chunks exist. */
  pool->current = pool->head;
  pool->head->used = 0;
  /* The high-water survives the reset -- it is the peak across the whole run --
   * but the live count restarts with the next processing unit. */
  pool->live = 0;
}

void galaxy_pool_stats(const struct GalaxyPool *pool, struct GalaxyPoolStats *out) {
  assert(pool != NULL);
  assert(out != NULL);

  out->galaxies_high_water = pool->live_high_water;
  out->slots_allocated = pool->slots_allocated;
  out->chunk_count = pool->chunk_count;
}

void galaxy_pool_destroy(struct GalaxyPool *pool) {
  assert(pool != NULL);

  struct GalaxyChunk *chunk = pool->head;
  while (chunk != NULL) {
    struct GalaxyChunk *next = chunk->next;
    myfree(chunk);
    chunk = next;
  }
  myfree(pool);
}
