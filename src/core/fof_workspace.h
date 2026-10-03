#ifndef CORE_FOF_WORKSPACE_H
#define CORE_FOF_WORKSPACE_H

/**
 * @file    fof_workspace.h
 * @brief   The FoF workspace descriptor both drivers hand to physics and marshalling
 *
 * A FoF workspace is the contiguous struct Halo array one FoF group is
 * assembled into by inheritance, evolved in place by the module pipeline, and
 * marshalled from into the driver's output buffer. struct FoFWorkspace carries
 * that array together with what the code operating on it needs to know about
 * where its rows come from: the galaxy pool their galaxies are allocated from
 * and the created-record identity space the driver published for the unit
 * being processed.
 *
 * Ownership: each driver owns exactly one descriptor for the whole of its
 * processing (the vertical driver's is sized per unit by load_unit() and
 * released by free_unit_halos(); the horizontal driver's lives in its
 * per-run state). The descriptor owns `halos` (a tracked MEM_HALOS
 * allocation); it borrows `pool`, whose owner resets and destroys it.
 * fof_workspace_reserve() is the only function that grows `halos`, so any
 * pointer into the array must be re-read from the descriptor after a call
 * that may reserve.
 */

#include <stdint.h>

#include "types.h" /* struct Halo, struct RecordIdentitySpace */

struct GalaxyPool; /* opaque; defined in galaxy_pool.c */

struct FoFWorkspace {
  struct Halo *halos;                  /* [capacity]; rows [0, count) are live */
  int64_t count;                       /* rows assembled for the current FoF group */
  int64_t capacity;                    /* rows allocated in `halos` */
  struct GalaxyPool *pool;             /* borrowed: where this workspace's galaxies live */
  struct RecordIdentitySpace identity; /* the driver's published space for this unit */
};

/**
 * @brief   Grow the workspace until it holds at least @p required rows
 *
 * @param   ws         Workspace to grow
 * @param   required   Number of rows the caller is about to fill
 *
 * Grows by HALO_ARRAY_GROWTH_FACTOR, by at least MIN_HALO_ARRAY_GROWTH rows,
 * up to MAX_HALO_ARRAY_SIZE, through myrealloc_cat in MEM_HALOS, and zeroes
 * every new row so its galaxy pointer starts NULL. A request above
 * MAX_HALO_ARRAY_SIZE is fatal. Does nothing when the capacity already
 * suffices. `count`, `pool` and `identity` are left unchanged.
 */
void fof_workspace_reserve(struct FoFWorkspace *ws, int64_t required);

/**
 * @brief   Release the workspace's rows and empty the descriptor
 *
 * @param   ws   Workspace to release
 *
 * Frees `halos` (the final, possibly grown, allocation) and zeroes the whole
 * descriptor, dropping the borrowed pool and the identity space; the pool
 * itself is untouched. Safe on a descriptor that holds no allocation.
 */
void fof_workspace_destroy(struct FoFWorkspace *ws);

#endif /* CORE_FOF_WORKSPACE_H */
