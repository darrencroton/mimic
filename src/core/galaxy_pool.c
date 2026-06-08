/**
 * @file    galaxy_pool.c
 * @brief   Chunked, per-tree storage pool for galaxy data
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

static struct GalaxyChunk *PoolHead = NULL;               /* first chunk (NULL until initialised) */
static struct GalaxyChunk *PoolCurrent = NULL;            /* chunk currently being filled */
static int PoolChunkCapacity = GALAXY_POOL_DEFAULT_CHUNK; /* size of newly grown chunks */

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

void galaxy_pool_init(int initial_capacity) {
  if (PoolHead != NULL)
    return; /* already initialised */

  PoolChunkCapacity =
      initial_capacity > GALAXY_POOL_MIN_CHUNK ? initial_capacity : GALAXY_POOL_DEFAULT_CHUNK;
  PoolHead = new_chunk(PoolChunkCapacity);
  PoolCurrent = PoolHead;
}

struct GalaxyData *galaxy_pool_alloc(void) {
  if (PoolCurrent == NULL)
    galaxy_pool_init(0); /* robust against a skipped explicit init */

  if (PoolCurrent->used >= PoolCurrent->capacity) {
    if (PoolCurrent->next != NULL) {
      /* Reuse a chunk left over from a previous, larger tree. */
      PoolCurrent = PoolCurrent->next;
      PoolCurrent->used = 0;
    } else {
      struct GalaxyChunk *chunk = new_chunk(PoolChunkCapacity);
      PoolCurrent->next = chunk;
      PoolCurrent = chunk;
    }
  }

  return &PoolCurrent->data[PoolCurrent->used++];
}

void galaxy_pool_reset(void) {
  /* Rewind to the first chunk; trailing chunks are reset lazily as alloc reaches
   * them, so a single reset is O(1) regardless of how many chunks exist. */
  PoolCurrent = PoolHead;
  if (PoolHead != NULL)
    PoolHead->used = 0;
}

void galaxy_pool_destroy(void) {
  struct GalaxyChunk *chunk = PoolHead;
  while (chunk != NULL) {
    struct GalaxyChunk *next = chunk->next;
    myfree(chunk);
    chunk = next;
  }
  PoolHead = NULL;
  PoolCurrent = NULL;
  PoolChunkCapacity = GALAXY_POOL_DEFAULT_CHUNK;
}
