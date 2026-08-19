#ifndef GALAXY_POOL_H
#define GALAXY_POOL_H

/**
 * @file    galaxy_pool.h
 * @brief   Instanced galaxy storage pool
 *
 * Galaxy data (struct GalaxyData) is fixed-size, POD, and lives exactly as long
 * as the tree that owns it. Rather than one tracked mymalloc block per halo (a
 * pattern that makes the allocator's block-tracking table a hard cap on
 * halos-per-forest), galaxies are handed out from a chunked pool: large
 * contiguous chunks of GalaxyData that grow by appending new chunks and are
 * bulk-reset between trees. Each new chunk is sized geometrically -- roughly
 * double the previous one, capped at GALAXY_POOL_MAX_CHUNK in galaxy_pool.c --
 * so the tracked-block count stays small (well under 200 chunks per pool even
 * at hundreds of millions of galaxies per generation) instead of growing by
 * one block per fixed-size chunk. This keeps the number of tracked allocator
 * blocks at O(chunks) instead of O(halos) and bounds memory by the largest
 * single tree.
 *
 * A pool is an explicit instance (struct GalaxyPool), created with
 * galaxy_pool_create() and passed to every other call. Two instances never
 * share chunks or state, so allocations from one pool cannot interfere with
 * another's.
 *
 * Ownership: a pool owns all galaxy memory allocated from it. Nothing else
 * frees a galaxy. The owner resets the pool at the end of each tree
 * (galaxy_pool_reset) and destroys it once at shutdown (galaxy_pool_destroy).
 * Chunks are never moved once allocated, so pointers returned by
 * galaxy_pool_alloc stay valid as the pool grows and as halo structs (and
 * their galaxy pointers) are copied between the workspace and the output
 * buffer.
 *
 * INVARIANT: galaxy_pool_alloc returns raw, uninitialised storage (a slot may
 * hold bytes from a prior tree). Every caller MUST fully overwrite the slot
 * before any read — either memcpy of a complete GalaxyData or init_galaxy_defaults
 * (which sets every field). This keeps slot reuse byte-identical to a fresh
 * malloc.
 */

#include <stdint.h>

struct GalaxyData; /* defined in generated/property_defs.h via types.h */
struct GalaxyPool; /* opaque; defined in galaxy_pool.c */

/* What a pool has cost, for the run memory profile (src/util/run_profile.h).
 * `galaxies_high_water` is the peak number of slots handed out concurrently --
 * it is reset-aware, counting allocations since the last galaxy_pool_reset and
 * keeping the largest such run. `slots_allocated` is the sum of chunk
 * capacities, which is what stays resident; the difference between the two is
 * chunk slack. */
struct GalaxyPoolStats {
  int64_t galaxies_high_water;
  int64_t slots_allocated;
  int chunk_count;
};

/* Create a pool with an initial chunk sized for `initial_capacity` galaxies
 * (clamped to a sensible minimum and maximum). Returns a handle that must be
 * passed to every other galaxy_pool_* call and, eventually, to
 * galaxy_pool_destroy(). */
struct GalaxyPool *galaxy_pool_create(int initial_capacity);

/* Return a stable pointer to one uninitialised GalaxyData slot in `pool`,
 * growing the pool by a new, larger chunk when the current chunk is full and
 * no already-allocated chunk remains to reuse. */
struct GalaxyData *galaxy_pool_alloc(struct GalaxyPool *pool);

/* Reclaim every slot in `pool` for reuse (retains chunks). Call at the end of
 * each tree. */
void galaxy_pool_reset(struct GalaxyPool *pool);

/* Report `pool`'s cost into `*out`. Cheap and read-only, so it may be called at
 * any point; call it before galaxy_pool_destroy() if the figures are wanted. */
void galaxy_pool_stats(const struct GalaxyPool *pool, struct GalaxyPoolStats *out);

/* Free every chunk of `pool` and the pool itself. Call once at shutdown. */
void galaxy_pool_destroy(struct GalaxyPool *pool);

#endif /* GALAXY_POOL_H */
