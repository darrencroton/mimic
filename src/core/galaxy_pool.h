#ifndef GALAXY_POOL_H
#define GALAXY_POOL_H

/**
 * @file    galaxy_pool.h
 * @brief   Per-tree galaxy storage pool
 *
 * Galaxy data (struct GalaxyData) is fixed-size, POD, and lives exactly as long
 * as the tree that owns it. Rather than one tracked mymalloc block per halo (a
 * pattern that makes the allocator's block-tracking table a hard cap on
 * halos-per-forest), galaxies are handed out from a chunked pool: large
 * contiguous chunks of GalaxyData that grow by appending new chunks and are
 * bulk-reset between trees. This keeps the number of tracked allocator blocks at
 * O(chunks) instead of O(halos) and bounds memory by the largest single tree.
 *
 * Ownership: the pool owns all galaxy memory. Nothing else frees a galaxy. The
 * tree driver resets the pool at the end of each tree (galaxy_pool_reset) and
 * destroys it once at shutdown (galaxy_pool_destroy). Chunks are never moved
 * once allocated, so pointers returned by galaxy_pool_alloc stay valid as the
 * pool grows and as halo structs (and their galaxy pointers) are copied between
 * the workspace and the output buffer.
 *
 * INVARIANT: galaxy_pool_alloc returns raw, uninitialised storage (a slot may
 * hold bytes from a prior tree). Every caller MUST fully overwrite the slot
 * before any read — either memcpy of a complete GalaxyData or init_galaxy_defaults
 * (which sets every field). This keeps slot reuse byte-identical to a fresh
 * malloc.
 */

struct GalaxyData; /* defined in generated/property_defs.h via types.h */

/* Prepare the pool with an initial chunk sized for `initial_capacity` galaxies
 * (clamped to a sensible minimum). Call once at startup. */
void galaxy_pool_init(int initial_capacity);

/* Return a stable pointer to one uninitialised GalaxyData slot, growing the pool
 * by a chunk when the current chunk is full. */
struct GalaxyData *galaxy_pool_alloc(void);

/* Reclaim every slot for reuse (retains chunks). Call at the end of each tree. */
void galaxy_pool_reset(void);

/* Free all chunks and reset state. Call once at shutdown. */
void galaxy_pool_destroy(void);

#endif /* GALAXY_POOL_H */
