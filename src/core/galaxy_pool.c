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
 * This mirrors the run-persistent, grow-to-high-water scratch idiom already used
 * for the inheritance gather buffers (see ProgenitorScratch in build_model.c).
 */

#include <assert.h>

#include "galaxy_pool.h"
#include "memory.h"
#include "types.h" /* struct GalaxyData */

#define GALAXY_POOL_MIN_CHUNK 1024     /* smallest chunk we bother allocating */
#define GALAXY_POOL_DEFAULT_CHUNK 8192 /* ~1.1 MB of GalaxyData per chunk */

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

  pool->chunk_capacity =
      initial_capacity > GALAXY_POOL_MIN_CHUNK ? initial_capacity : GALAXY_POOL_DEFAULT_CHUNK;
  pool->head = new_chunk(pool->chunk_capacity);
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
      struct GalaxyChunk *chunk = new_chunk(pool->chunk_capacity);
      pool->current->next = chunk;
      pool->current = chunk;
    }
  }

  return &pool->current->data[pool->current->used++];
}

void galaxy_pool_reset(struct GalaxyPool *pool) {
  assert(pool != NULL);

  /* Rewind to the first chunk; trailing chunks are reset lazily as alloc reaches
   * them, so a single reset is O(1) regardless of how many chunks exist. */
  pool->current = pool->head;
  pool->head->used = 0;
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
