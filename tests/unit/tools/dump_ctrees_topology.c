/**
 * @file    dump_ctrees_topology.c
 * @brief   Read-only reference-topology dump for a Consistent-Trees-ASCII package
 *
 * Loads every forest of a Consistent-Trees-ASCII simulation package through
 * Mimic's existing, unmodified consistent_trees_ascii reader (tree/interface.c
 * + read_ctrees_ascii.c) and dumps, per halo, the literal RawHalo link fields
 * (Descendant, FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup,
 * NextHaloInFOFgroup), translated from local per-forest array indices to the
 * stable ctrees id (MostBoundID), plus the forest's global forest number and
 * the halo's within-forest rank (its position in the per-forest
 * InputTreeHalos array, which the reader already returns in final reference
 * order after fix_flybys/fix_upid/assign_mergertree_indices have run).
 *
 * This is direct reference evidence for chain-order conformance: an external
 * consumer can compare it against another implementation's own chain
 * construction over the same source data. It performs no processing beyond
 * what the production reader already does while loading a forest: no FoF
 * grouping, no inheritance, no output. It never modifies tree_driver.c,
 * read_ctrees_ascii.c, or any other production file — every function called
 * here is an existing, unmodified public entry point (tree/interface.h,
 * core/proto.h).
 *
 * Usage: dump_ctrees_topology <run_param_file> <output_dump_path>
 * Exit codes: 0 complete dump written, 1 runtime/write failure, 2 bad usage.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "error.h"
#include "galaxy_pool.h"
#include "globals.h"
#include "memory.h"
#include "proto.h"
#include "tree/interface.h"
#include "tree/reader.h"

/**
 * @brief   Exit handler required by src/util/memory.c's fatal-allocation path.
 *
 * This harness has its own main() and does not link core/main.c (which
 * defines the production myexit() with MPI-aware messaging), so it provides
 * the same minimal contract directly: print and exit with the given code.
 */
void myexit(int signum) {
  fprintf(stderr, "dump_ctrees_topology: exiting (%d)\n", signum);
  exit(signum);
}

/* tree_driver.c gates every reader hook the same way before calling it; this
 * harness calls the same hooks directly (it has no driver to call through),
 * so it needs the same guard rather than trusting the reader table blindly. */
#define REQUIRE_READER_HOOK(reader, member)                                                        \
  do {                                                                                             \
    if ((reader)->member == NULL) {                                                                \
      FATAL_ERROR("Tree reader '%s' is missing required partition hook '%s'", (reader)->name,      \
                  #member);                                                                        \
    }                                                                                              \
  } while (0)

#define TOPOLOGY_DUMP_FORMAT_VERSION "mimic-topology-dump v1"
/* No production id is ever INT64_MIN (writer/battery/crosscheck already
 * reject a converter MostBoundID of INT64_MIN dataset-wide because its
 * magnitude overflows signed int64), so it is a safe, unambiguous NA marker
 * for "no link" here. */
#define TOPOLOGY_DUMP_NA_SENTINEL INT64_MIN

/**
 * @brief   Resolve a local per-forest halo index to its stable ctrees id.
 * @param   local_index   Index into the currently loaded InputTreeHalos array,
 *                         or a negative sentinel (-1) meaning "no link".
 * @return  The target halo's MostBoundID, or TOPOLOGY_DUMP_NA_SENTINEL.
 */
static long long topology_dump_id_of(int local_index) {
  if (local_index < 0) {
    return TOPOLOGY_DUMP_NA_SENTINEL;
  }
  return (long long)InputTreeHalos[local_index].MostBoundID;
}

/**
 * @brief   Dump every halo of the currently loaded forest (unit) to `out`.
 * @param   out             Destination stream.
 * @param   unit            Local (chunk-relative) forest index just loaded.
 * @param   forestnr_global Dense global forest number (GlobalForestOffset + unit).
 */
static void topology_dump_forest(FILE *out, int unit, long long forestnr_global) {
  for (int halonr = 0; halonr < InputTreeNHalos[unit]; halonr++) {
    const struct RawHalo *h = &InputTreeHalos[halonr];
    fprintf(out, "%lld %d %lld %d %lld %lld %lld %lld %lld\n", forestnr_global, halonr,
            (long long)h->MostBoundID, h->SnapNum, topology_dump_id_of(h->Descendant),
            topology_dump_id_of(h->FirstProgenitor), topology_dump_id_of(h->NextProgenitor),
            topology_dump_id_of(h->FirstHaloInFOFgroup),
            topology_dump_id_of(h->NextHaloInFOFgroup));
  }
}

int main(int argc, char **argv) {
  if (argc != 3) {
    fprintf(stderr, "Usage: %s <run_param_file> <output_dump_path>\n", argv[0]);
    return 2;
  }
  const char *param_file = argv[1];
  const char *dump_path = argv[2];

  /* Minimal, faithful subset of main()'s startup sequence: only what
   * read_parameter_file()/init() and the tree reader require. Deliberately
   * skips module registration, HDF5 output setup, and run_processing_driver()
   * — none of those are needed to read raw forests, and skipping them keeps
   * this harness read-only with no output side effects beyond the dump. */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);
  galaxy_pool_init(0);

  read_parameter_file(param_file);
  init();

  const struct TreeReader *reader = MimicConfig.reader;
  if (reader == NULL) {
    fprintf(stderr, "%s: no tree reader selected\n", param_file);
    return 1;
  }

  FILE *out = fopen(dump_path, "w");
  if (out == NULL) {
    fprintf(stderr, "Cannot open output dump path '%s'\n", dump_path);
    return 1;
  }
  fprintf(out, "# %s\n", TOPOLOGY_DUMP_FORMAT_VERSION);
  fprintf(out, "# forestnr rank id snapnum desc_id first_prog_id next_prog_id first_fof_id "
               "next_fof_id\n");
  fprintf(out, "# NA sentinel = %lld (no link)\n", (long long)TOPOLOGY_DUMP_NA_SENTINEL);

  REQUIRE_READER_HOOK(reader, num_partitions);
  REQUIRE_READER_HOOK(reader, partition_exists);
  REQUIRE_READER_HOOK(reader, partition_output_id);
  REQUIRE_READER_HOOK(reader, global_forest_offset);

  if (reader->prepare_run != NULL) {
    reader->prepare_run();
  }

  const int npartitions = reader->num_partitions();
  for (int partition = 0; partition < npartitions; partition++) {
    if (!reader->partition_exists(partition)) {
      continue;
    }
    const int output_id = reader->partition_output_id(partition);
    GlobalForestOffset = reader->global_forest_offset(partition);
    open_partition(output_id);
    for (int unit = 0; unit < Ntrees; unit++) {
      load_unit(unit);
      topology_dump_forest(out, unit, (long long)GlobalForestOffset + unit);
      free_unit_halos();
    }
    close_partition();
  }

  if (reader->teardown_run != NULL) {
    reader->teardown_run();
  }

  /* A silently short dump is the failure mode that matters: the consumer's
   * topology-chains check asserts the dump names every converter halo, so a
   * truncated write must surface as a non-zero exit here rather than as a
   * confusing coverage mismatch downstream. Check the stream error flag once
   * (cheaper than testing every fprintf) and the fclose flush separately,
   * since the final buffered write can only fail at close. */
  const int write_failed = ferror(out);
  if (fclose(out) != 0 || write_failed) {
    fprintf(stderr, "Failed to write dump '%s' completely (output is truncated)\n", dump_path);
    return 1;
  }
  return 0;
}
