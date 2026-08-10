/**
 * @file    snapshot_driver.c
 * @brief   Snapshot-ordered run driver.
 *
 * Deliberately not named "*hdf5.c": Makefile:272 filters that suffix out of a
 * USE-HDF5=no build, and the snapshot-ordered dispatch case must compile and
 * link in every build (the reader interface symbols it calls are always
 * present; a non-HDF5 build already rejects tree_type: snapshot_hdf5 at
 * configuration, long before this driver runs).
 *
 * This is a skeleton: it exercises the full open_run -> load/release loop
 * under the production two-generation slab rotation, then FATALs rather than
 * produce output. A snapshot-ordered run must never exit 0 until a later
 * slice adds the physics pipeline and output writers.
 */

#include <inttypes.h>

#include "error.h"
#include "globals.h"
#include "proto.h"
#include "snapshot/reader.h"

/**
 * @brief   Run a snapshot-ordered configuration through the reader lifecycle.
 *
 * Opens the configured dataset, publishes its run-scoped metadata, then walks
 * every snapshot in increasing time order holding at most two slab
 * generations live at once: snapshot N is loaded while snapshot N-1 is still
 * live, then N-1 is released. After the final snapshot the last live slab is
 * released and the run is closed, then the driver FATALs: no physics, no
 * gather, no output of any kind exists yet.
 */
void run_snapshot_driver(void) {
  const struct SnapshotReader *reader = MimicConfig.snapshot_reader;
  struct SnapshotRunInfo info;
  struct SnapshotSlab slabs[2] = {SNAPSHOT_SLAB_INIT, SNAPSHOT_SLAB_INIT};

  snapshot_reader_open_run(reader, &info);
  INFO_LOG("Opened snapshot-ordered run '%s': %" PRId64 " snapshot%s, format_version %" PRId32
           ", %" PRId64 " forest%s, max halo rank in forest %" PRId64,
           reader->name, info.snapshot_count, info.snapshot_count == 1 ? "" : "s",
           info.format_version, info.n_forests_total, info.n_forests_total == 1 ? "" : "s",
           info.max_halo_rank_in_forest);

  for (int64_t snapnum = 0; snapnum < info.snapshot_count; snapnum++) {
    const int slot = (int)(snapnum % 2);

    snapshot_reader_load_slab(reader, snapnum, &slabs[slot]);

    const int live_slabs = (snapnum > 0) ? 2 : 1;
    DEBUG_LOG("Loaded snapshot %" PRId64 " (%" PRId64 " halos); %d slab%s live", snapnum,
              slabs[slot].nhalos, live_slabs, live_slabs == 1 ? "" : "s");

    if (snapnum > 0) {
      const int prev_slot = (int)((snapnum - 1) % 2);
      snapshot_reader_release_slab(reader, &slabs[prev_slot]);
    }
  }

  if (info.snapshot_count > 0) {
    const int last_slot = (int)((info.snapshot_count - 1) % 2);
    snapshot_reader_release_slab(reader, &slabs[last_slot]);
  }

  snapshot_reader_close_run(reader);

  FATAL_ERROR("The snapshot-ordered driver has validated and loaded every snapshot but cannot "
              "yet produce output");
}
