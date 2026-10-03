/**
 * @file    fof_workspace.c
 * @brief   Growth and release of the FoF workspace descriptor
 *
 * The one row-growth function both drivers' workspaces go through, and the
 * grower of the created-host map the record-creation commit fills (see
 * fof_workspace.h for the descriptor's ownership contract). Kept in its own
 * translation unit so every harness that links a driver's workspace lifecycle
 * (the unit-test runner and the topology dump tool) links the same body.
 */

#include <inttypes.h>
#include <string.h>

#include "constants.h"
#include "error.h"
#include "fof_workspace.h"
#include "memory.h"

void fof_workspace_reserve(struct FoFWorkspace *ws, int64_t required) {
  /* Refuse an over-cap request up front, before the loop reallocates to the cap. */
  if (required > MAX_HALO_ARRAY_SIZE) {
    FATAL_ERROR("FoF workspace requires %" PRId64 " halos but maximum allowed size is %d", required,
                MAX_HALO_ARRAY_SIZE);
  }

  while (required > ws->capacity) {
    const int64_t old_size = ws->capacity;
    int64_t new_size = (int64_t)(ws->capacity * HALO_ARRAY_GROWTH_FACTOR);

    if (new_size - ws->capacity < MIN_HALO_ARRAY_GROWTH)
      new_size = ws->capacity + MIN_HALO_ARRAY_GROWTH;

    if (new_size > MAX_HALO_ARRAY_SIZE)
      new_size = MAX_HALO_ARRAY_SIZE;

    if (new_size <= ws->capacity) {
      FATAL_ERROR("FoF workspace requires %" PRId64 " halos but maximum allowed size is %d",
                  required, MAX_HALO_ARRAY_SIZE);
    }

    INFO_LOG("Growing FoF workspace from %" PRId64 " to %" PRId64 " elements", old_size, new_size);

    ws->halos = myrealloc_cat(ws->halos, (size_t)new_size * sizeof(struct Halo), MEM_HALOS);
    ws->capacity = new_size;
    memset(&ws->halos[old_size], 0, (size_t)(new_size - old_size) * sizeof(struct Halo));
  }
}

void fof_workspace_reserve_created(struct FoFWorkspace *ws, int64_t required) {
  if (required > MAX_HALO_ARRAY_SIZE) {
    FATAL_ERROR("FoF workspace requires %" PRId64 " created records but maximum allowed size is %d",
                required, MAX_HALO_ARRAY_SIZE);
  }

  if (required <= ws->created_capacity) {
    return;
  }

  int64_t new_size = (int64_t)(ws->created_capacity * HALO_ARRAY_GROWTH_FACTOR);
  if (new_size - ws->created_capacity < MIN_HALO_ARRAY_GROWTH)
    new_size = ws->created_capacity + MIN_HALO_ARRAY_GROWTH;
  if (new_size < required)
    new_size = required;
  if (new_size > MAX_HALO_ARRAY_SIZE)
    new_size = MAX_HALO_ARRAY_SIZE;

  ws->created_host =
      myrealloc_cat(ws->created_host, (size_t)new_size * sizeof(*ws->created_host), MEM_HALOS);
  ws->created_capacity = new_size;
}

void fof_workspace_destroy(struct FoFWorkspace *ws) {
  myfree(ws->halos);
  myfree(ws->created_host);
  memset(ws, 0, sizeof(*ws));
}
