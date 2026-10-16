/**
 * @file    dump_ctrees_topology.c
 * @brief   Read-only reference dumps of merger trees through Mimic's vertical readers
 *
 * Loads every forest of a simulation package through Mimic's existing,
 * unmodified vertical reader (vertical/interface.c and the selected reader) and
 * dumps, per halo, the literal RawHalo link fields and core payload, exactly as
 * the reader returns them once its own fix-ups and link assignment have run.
 *
 * This is direct reference evidence: an external consumer can compare it
 * against another implementation's reading of the same source data. It
 * performs no processing beyond what the production reader already does while
 * loading a forest: no FoF grouping, no inheritance, no output. It never
 * modifies vertical_driver.c, a reader, or any other production file — every
 * function called here is an existing, unmodified public entry point
 * (vertical/interface.h, core/proto.h).
 *
 * Usage: dump_ctrees_topology --source-payload <run_param_file> <output_dump_path>
 * Exit codes: 0 complete dump written, 1 runtime/write failure, 2 bad usage.
 *
 * The tool writes one format, `mimic-source-dump v1`, for the generalisation
 * acceptance comparator (convert/mimic-convert/tests/run_generalisation_acceptance.py);
 * the `--source-payload` flag names it and is required.
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
 * so a truncated dump is detectable by its consumer as well as by exit status,
 * and a dump that fails (exit 1) after it was opened is removed, so no
 * partial reference is left on disk.
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

/* The dump being written, or NULL: set once the dump is opened
 * and cleared once it is complete, so every failure in between can remove it. */
static FILE *partial_dump_stream = NULL;
static const char *partial_dump_path = NULL;

/**
 * @brief   Close and remove an unfinished source dump, if one is registered.
 *
 * Idempotent; reports (but survives) a failed removal, since it runs on paths
 * that are already exiting with status 1.
 */
static void discard_partial_dump(void) {
  if (partial_dump_stream != NULL) {
    fclose(partial_dump_stream);
    partial_dump_stream = NULL;
  }
  if (partial_dump_path != NULL) {
    if (remove(partial_dump_path) != 0) {
      fprintf(stderr, "dump_ctrees_topology: could not remove partial dump '%s'\n",
              partial_dump_path);
    }
    partial_dump_path = NULL;
  }
}

/**
 * @brief   Exit handler required by src/util/memory.c's fatal-allocation path.
 *
 * This harness has its own main() and does not link core/main.c (which
 * defines the production myexit() with MPI-aware messaging), so it provides
 * the same minimal contract directly: remove an unfinished dump, print
 * and exit with the given code. Every FATAL_ERROR reaches here.
 */
void myexit(int signum) {
  discard_partial_dump();
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

#define SOURCE_DUMP_FORMAT_VERSION "mimic-source-dump v1"
#define SOURCE_DUMP_MODE_FLAG "--source-payload"

/* Float payload is dumped as its exact binary32 bit pattern. */
_Static_assert(sizeof(float) == sizeof(uint32_t), "source dump assumes 32-bit float");

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
  if (argc != 4 || strcmp(argv[1], SOURCE_DUMP_MODE_FLAG) != 0) {
    fprintf(stderr, "Usage: %s %s <run_param_file> <output_dump_path>\n", argv[0],
            SOURCE_DUMP_MODE_FLAG);
    return 2;
  }
  const char *param_file = argv[2];
  const char *dump_path = argv[3];

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

  /* Per-file readers have no global_forest_offset hook, so their forest
   * numbers are the count-prefix offsets core/vertical_driver.c builds. */
  const int per_file_offsets = reader->partition_model == PARTITION_PER_FILE;
  partial_dump_stream = out;
  partial_dump_path = dump_path;
  source_dump_header(out, reader);

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
      rows_written += source_dump_forest(out, unit, output_id, GlobalForestOffset + unit);
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

  fprintf(out, "# end rows %" PRId64 " forests %" PRId64 "\n", rows_written, forests_written);

  /* A silently short dump is the failure mode that matters: the consumer
   * compares the dump row for row, so a truncated write must surface as a
   * non-zero exit here rather than as a confusing mismatch downstream. Check
   * the stream error flag once (cheaper than testing every fprintf) and the
   * fclose flush separately, since the final buffered write can only fail at
   * close. */
  const int write_failed = ferror(out);
  partial_dump_stream = NULL; /* closed just below, whatever the outcome */
  if (fclose(out) != 0 || write_failed) {
    fprintf(stderr, "Failed to write dump '%s' completely (partial dump removed)\n", dump_path);
    discard_partial_dump();
    return 1;
  }
  partial_dump_path = NULL; /* complete: keep it */
  return 0;
}
