/**
 * @file    dump_ctrees_topology.c
 * @brief   Read-only reference dumps of merger trees through Mimic's vertical readers
 *
 * Loads every forest of a Consistent-Trees-ASCII simulation package through
 * Mimic's existing, unmodified consistent_trees_ascii reader (vertical/interface.c
 * + read_ctrees_ascii.c) and dumps, per halo, the literal RawHalo link fields
 * (Descendant, FirstProgenitor, NextProgenitor, FirstHaloInFOFgroup,
 * NextHaloInFOFgroup), translated from local per-forest array indices to the
 * stable ctrees id (MostBoundID), plus the forest's global forest number and
 * the halo's within-forest rank (its position in the per-forest
 * InputTreeHalos array, which the reader already returns in final reference
 * order after fix_upid/assign_mergertree_indices have run).
 *
 * This is direct reference evidence for chain-order conformance: an external
 * consumer can compare it against another implementation's own chain
 * construction over the same source data. It performs no processing beyond
 * what the production reader already does while loading a forest: no FoF
 * grouping, no inheritance, no output. It never modifies vertical_driver.c,
 * read_ctrees_ascii.c, or any other production file — every function called
 * here is an existing, unmodified public entry point (vertical/interface.h,
 * core/proto.h).
 *
 * Usage: dump_ctrees_topology <run_param_file> <output_dump_path>
 *        dump_ctrees_topology --source-payload <run_param_file> <output_dump_path>
 * Exit codes: 0 complete dump written, 1 runtime/write failure, 2 bad usage.
 *
 * **Two modes, two separately versioned formats.** The default (two-argument)
 * invocation writes `mimic-topology-dump v1` exactly as before: MostBoundID-keyed
 * links for the enumerated Consistent-Trees readers, consumed by
 * scripts/convert/crosscheck.py. It still requires the reader's
 * global_forest_offset hook, so it still refuses the per-file L-Halo readers.
 *
 * `--source-payload` writes `mimic-source-dump v1` for the generalisation
 * acceptance comparator (scripts/convert/tests/run_generalisation_acceptance.py).
 * It works with every vertical reader, because MostBoundID is not a key there
 * (L-Halo particle identifiers are signed and repeat): each halo is identified
 * by the reader's own source-relative identity (forest number, within-forest
 * rank), each link is the target's within-forest rank plus, for the three
 * snapshot-qualified links, the target's SnapNum, and the core payload is
 * written as exact integers and IEEE-754 binary32 bit patterns, so the
 * comparison is bit-exact at the reader's own cast boundary. Forest numbers
 * follow the production driver: enumerated readers publish them through
 * global_forest_offset, and per-file readers (which have no such hook) get the
 * count-prefix offset core/vertical_driver.c builds from count_partition_units,
 * run-scoped across FirstFile..LastFile. Unlike the driver, a missing requested
 * file is fatal here: reference evidence must not silently narrow the inventory.
 * The stream ends with an `# end` trailer carrying the row and forest totals,
 * so a truncated dump is detectable by its consumer as well as by exit status.
 */

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "error.h"
#include "globals.h"
#include "memory.h"
#include "proto.h"
#include "vertical/interface.h"
#include "vertical/reader.h"

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

/* vertical_driver.c gates every reader hook the same way before calling it; this
 * harness calls the same hooks directly (it has no driver to call through),
 * so it needs the same guard rather than trusting the reader table blindly. */
#define REQUIRE_READER_HOOK(reader, member)                                                        \
  do {                                                                                             \
    if ((reader)->member == NULL) {                                                                \
      FATAL_ERROR("Vertical reader '%s' is missing required partition hook '%s'", (reader)->name,  \
                  #member);                                                                        \
    }                                                                                              \
  } while (0)

#define TOPOLOGY_DUMP_FORMAT_VERSION "mimic-topology-dump v1"
#define SOURCE_DUMP_FORMAT_VERSION "mimic-source-dump v1"
#define SOURCE_DUMP_MODE_FLAG "--source-payload"

/* Float payload is dumped as its exact binary32 bit pattern. */
_Static_assert(sizeof(float) == sizeof(uint32_t), "source dump assumes 32-bit float");

enum DumpMode {
  DUMP_MODE_TOPOLOGY_V1 = 0, /* default: mimic-topology-dump v1, MostBoundID-keyed */
  DUMP_MODE_SOURCE_V1 = 1,   /* --source-payload: mimic-source-dump v1, rank-keyed */
};
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

/** @brief The exact IEEE-754 binary32 bit pattern of `value`. */
static uint32_t source_dump_float_bits(float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  return bits;
}

/**
 * @brief   Snapshot of a link's target, or -1 when the link is not a valid local index.
 * @param   local_index  Stored link value: a within-forest index, or -1 for "no link".
 * @param   nhalos       Halo count of the loaded forest.
 *
 * The stored link value itself is always dumped verbatim, so an out-of-forest
 * value (which a reader may not reject) reaches the comparator unchanged and
 * fails there; only the target lookup, which would read outside the forest, is
 * suppressed.
 */
static int source_dump_target_snap(int local_index, int nhalos) {
  if (local_index < 0 || local_index >= nhalos) {
    return -1;
  }
  return InputTreeHalos[local_index].SnapNum;
}

/**
 * @brief   Dump every halo of the loaded forest as `mimic-source-dump v1` rows.
 * @param   out             Destination stream.
 * @param   unit            Unit index within the open partition.
 * @param   output_id       The partition's output id (the file number for per-file readers).
 * @param   forestnr_global Run-scoped forest number of this unit.
 * @return  Number of rows written.
 */
static int64_t source_dump_forest(FILE *out, int unit, int output_id, int64_t forestnr_global) {
  const int nhalos = InputTreeNHalos[unit];
  for (int halonr = 0; halonr < nhalos; halonr++) {
    const struct RawHalo *h = &InputTreeHalos[halonr];
    fprintf(out, "%" PRId64 " %d %d %d %d %d %d %d %d %d %d %d %d %d %lld", forestnr_global, halonr,
            output_id, unit, h->SnapNum, h->Descendant,
            source_dump_target_snap(h->Descendant, nhalos), h->FirstProgenitor,
            source_dump_target_snap(h->FirstProgenitor, nhalos), h->NextProgenitor,
            source_dump_target_snap(h->NextProgenitor, nhalos), h->FirstHaloInFOFgroup,
            h->NextHaloInFOFgroup, h->Len, (long long)h->MostBoundID);
    const float payload[] = {h->M_Crit200, h->Pos[0],  h->Pos[1],  h->Pos[2],
                             h->Vel[0],    h->Vel[1],  h->Vel[2],  h->Spin[0],
                             h->Spin[1],   h->Spin[2], h->VelDisp, h->Vmax};
    for (size_t i = 0; i < sizeof(payload) / sizeof(payload[0]); i++) {
      fprintf(out, " %08" PRIx32, source_dump_float_bits(payload[i]));
    }
    fputc('\n', out);
  }
  return (int64_t)nhalos;
}

/** @brief Write the fixed `mimic-source-dump v1` header. */
static void source_dump_header(FILE *out, const struct VerticalReader *reader) {
  fprintf(out, "# %s\n", SOURCE_DUMP_FORMAT_VERSION);
  fprintf(out, "# reader %s partition_model %s\n", reader->name,
          reader->partition_model == PARTITION_PER_FILE ? "per_file" : "enumerated");
  fprintf(out, "# columns forest_index rank partition unit snapnum descendant descendant_snap "
               "first_progenitor first_progenitor_snap next_progenitor next_progenitor_snap "
               "first_fof next_fof len most_bound_id m_crit200 pos_x pos_y pos_z vel_x vel_y "
               "vel_z spin_x spin_y spin_z vel_disp vmax\n");
  fprintf(out, "# links are within-forest ranks, -1 = no link; *_snap is the target's snapnum, "
               "-1 = no link; m_crit200..vmax are binary32 bit patterns in hex\n");
}

int main(int argc, char **argv) {
  enum DumpMode mode = DUMP_MODE_TOPOLOGY_V1;
  if (argc == 4 && strcmp(argv[1], SOURCE_DUMP_MODE_FLAG) == 0) {
    mode = DUMP_MODE_SOURCE_V1;
    argv++;
    argc--;
  }
  if (argc != 3) {
    fprintf(stderr, "Usage: %s [%s] <run_param_file> <output_dump_path>\n", argv[0],
            SOURCE_DUMP_MODE_FLAG);
    return 2;
  }
  const char *param_file = argv[1];
  const char *dump_path = argv[2];

  /* Minimal, faithful subset of main()'s startup sequence: only what
   * read_parameter_file()/init() and the vertical reader require. Deliberately
   * skips module registration, HDF5 output setup, and run_processing_driver()
   * — none of those are needed to read raw forests, and skipping them keeps
   * this harness read-only with no output side effects beyond the dump. */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);

  read_parameter_file(param_file);
  init();

  const struct VerticalReader *reader = MimicConfig.vertical_reader;
  if (reader == NULL) {
    fprintf(stderr, "%s: no vertical reader selected\n", param_file);
    return 1;
  }

  FILE *out = fopen(dump_path, "w");
  if (out == NULL) {
    fprintf(stderr, "Cannot open output dump path '%s'\n", dump_path);
    return 1;
  }

  /* Only the source mode derives per-file forest numbers itself (the
   * count-prefix offsets core/vertical_driver.c builds); v1 keeps its original
   * requirement of the enumerated readers' global_forest_offset hook. */
  const int per_file_offsets =
      mode == DUMP_MODE_SOURCE_V1 && reader->partition_model == PARTITION_PER_FILE;
  if (mode == DUMP_MODE_SOURCE_V1) {
    source_dump_header(out, reader);
  } else {
    fprintf(out, "# %s\n", TOPOLOGY_DUMP_FORMAT_VERSION);
    fprintf(out, "# forestnr rank id snapnum desc_id first_prog_id next_prog_id first_fof_id "
                 "next_fof_id\n");
    fprintf(out, "# NA sentinel = %lld (no link)\n", (long long)TOPOLOGY_DUMP_NA_SENTINEL);
  }

  REQUIRE_READER_HOOK(reader, num_partitions);
  REQUIRE_READER_HOOK(reader, partition_exists);
  REQUIRE_READER_HOOK(reader, partition_output_id);
  if (per_file_offsets) {
    REQUIRE_READER_HOOK(reader, count_partition_units);
  } else {
    REQUIRE_READER_HOOK(reader, global_forest_offset);
  }

  if (reader->prepare_run != NULL) {
    reader->prepare_run();
  }

  int64_t per_file_offset = 0;
  int64_t rows_written = 0;
  int64_t forests_written = 0;
  const int npartitions = reader->num_partitions();
  for (int partition = 0; partition < npartitions; partition++) {
    const int output_id = reader->partition_output_id(partition);
    if (!reader->partition_exists(partition)) {
      if (per_file_offsets) {
        FATAL_ERROR("Requested input file %d is missing; a source dump never narrows the "
                    "configured FirstFile..LastFile inventory",
                    output_id);
      }
      continue;
    }
    int64_t expected_units = -1;
    if (per_file_offsets) {
      expected_units = reader->count_partition_units(partition);
      if (expected_units < 0 || expected_units > INT64_MAX - per_file_offset) {
        FATAL_ERROR("Input file %d reports an invalid unit count %" PRId64
                    " at forest offset %" PRId64,
                    output_id, expected_units, per_file_offset);
      }
      GlobalForestOffset = per_file_offset;
    } else {
      GlobalForestOffset = reader->global_forest_offset(partition);
    }
    open_partition(output_id);
    if (per_file_offsets && (int64_t)Ntrees != expected_units) {
      FATAL_ERROR("Input file %d opened with %d units but counted %" PRId64, output_id, Ntrees,
                  expected_units);
    }
    for (int unit = 0; unit < Ntrees; unit++) {
      load_unit(unit);
      if (mode == DUMP_MODE_SOURCE_V1) {
        rows_written += source_dump_forest(out, unit, output_id, GlobalForestOffset + unit);
      } else {
        topology_dump_forest(out, unit, (long long)GlobalForestOffset + unit);
      }
      free_unit_halos(NULL);
    }
    forests_written += Ntrees;
    close_partition();
    if (per_file_offsets) {
      per_file_offset += expected_units;
    }
  }

  if (reader->teardown_run != NULL) {
    reader->teardown_run();
  }

  if (mode == DUMP_MODE_SOURCE_V1) {
    fprintf(out, "# end rows %" PRId64 " forests %" PRId64 "\n", rows_written, forests_written);
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
