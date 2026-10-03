#ifndef CORE_FOF_WORKSPACE_H
#define CORE_FOF_WORKSPACE_H

/**
 * @file    fof_workspace.h
 * @brief   The FoF workspace descriptor both drivers hand to physics and marshalling
 *
 * fof_workspace_reserve() is the only function that grows a workspace's rows,
 * so any pointer into them must be re-read from the descriptor after a call
 * that may reserve. Within the module pipeline that happens only between
 * module callbacks, when the dispatcher commits records created by
 * module_create_record() (module_interface.h).
 */

#include <stdint.h>

#include "types.h" /* struct Halo, struct RecordIdentitySpace */

struct GalaxyPool; /* opaque; defined in galaxy_pool.c */

/**
 * @brief   One FoF group's processing rows and what they belong to
 *
 * The contiguous struct Halo array a FoF group is assembled into by
 * inheritance, evolved in place by the module pipeline, and marshalled from
 * into the driver's output buffer, together with where its galaxies live and
 * how records created in it are identified.
 *
 * - `halos` is owned: a tracked MEM_HALOS allocation, grown only by
 *   fof_workspace_reserve() and freed by fof_workspace_destroy().
 * - `pool` is borrowed: the galaxy pool the rows' galaxies come from; its
 *   owner resets and destroys it, never the workspace.
 * - `identity` is a per-unit snapshot of the driver's published
 *   created-record identity space, copied in by the driver for the unit
 *   (vertical) or snapshot (horizontal) being processed.
 * - Rows [0, count) are live, and 0 <= count <= capacity always.
 * - `base_count` is the row count when the module pipeline started for the
 *   current FoF group; process_halo_evolution() sets it (a harness that calls
 *   execute_module_pipeline() directly sets it itself). Rows [base_count, count)
 *   are records created during the pipeline by module_create_record()
 *   (module_interface.h), appended by the dispatcher in creation order.
 * - `created_host` is owned: a tracked MEM_HALOS allocation of
 *   `created_capacity` entries, grown only by fof_workspace_reserve_created()
 *   and freed by fof_workspace_destroy(). created_host[row - base_count] is the
 *   workspace index of created row `row`'s host, which is always below
 *   base_count. It stays NULL until the first record is created, and a
 *   descriptor whose created_host is NULL holds no created rows whatever
 *   base_count says (hand-built descriptors leave both zeroed).
 *
 * Each driver owns exactly one descriptor: the vertical driver's rows are
 * sized per unit by load_unit() and released by free_unit_halos(); the
 * horizontal driver's live in its per-run state.
 */
struct FoFWorkspace {
  struct Halo *halos;                  /* [capacity]; rows [0, count) are live */
  int64_t count;                       /* rows assembled for the current FoF group */
  int64_t capacity;                    /* rows allocated in `halos` */
  struct GalaxyPool *pool;             /* borrowed: where this workspace's galaxies live */
  struct RecordIdentitySpace identity; /* the driver's published space for this unit */
  int64_t base_count;                  /* rows present when the module pipeline started */
  int64_t *created_host;               /* [created_capacity]; host of row base_count + i */
  int64_t created_capacity;            /* entries allocated in `created_host` */
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
 * @brief   Grow the created-host map until it holds at least @p required entries
 *
 * @param   ws         Workspace whose created_host map to grow
 * @param   required   Number of created rows the caller is about to record
 *
 * Same policy as fof_workspace_reserve() (growth factor, minimum increment,
 * MAX_HALO_ARRAY_SIZE ceiling, MEM_HALOS); new entries are not initialised,
 * since the caller fills every entry it records. Does nothing when the
 * capacity already suffices.
 */
void fof_workspace_reserve_created(struct FoFWorkspace *ws, int64_t required);

/**
 * @brief   Release the workspace's rows and empty the descriptor
 *
 * @param   ws   Workspace to release
 *
 * Frees `halos` and `created_host` (the final, possibly grown, allocations)
 * and zeroes the whole descriptor, dropping the borrowed pool and the identity
 * space; the pool itself is untouched. Safe on a descriptor that holds no
 * allocation.
 */
void fof_workspace_destroy(struct FoFWorkspace *ws);

#endif /* CORE_FOF_WORKSPACE_H */
