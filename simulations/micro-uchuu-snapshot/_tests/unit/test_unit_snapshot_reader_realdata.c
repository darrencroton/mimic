/**
 * @file    test_unit_snapshot_reader_realdata.c
 * @brief   Opt-in unit test of the snapshot_hdf5 reader against the full dataset.
 *
 * Opens the complete 50-snapshot micro-Uchuu conversion through the snapshot
 * reader interface, so every validation open_run performs -- structure, header
 * values, snapshot-list agreement, and invariant 5's bounded data scans over
 * 22.5 million halos -- runs against production-layout data rather than the
 * committed fixture.
 *
 * Opt-in by construction: the dataset is machine-local, reached through the
 * gitignored `simulations/micro-uchuu-snapshot/snapshots` symlink. When that path
 * does not resolve the test skips and names the path, so it stays correct on a
 * machine without the data. See that package's README.md for the conversion
 * commands and the symlink instruction.
 *
 * This is a C unit test rather than a Python integration test because Phase 4b
 * adds no runtime caller: no ./mimic run can reach the reader until the
 * snapshot-ordered driver exists.
 */

#include "../../../../tests/framework/test_framework.h"

#include "../../../../src/include/constants.h"
#include "../../../../src/include/proto.h"
#include "../../../../src/include/types.h"
#include "../../../../src/io/snapshot/reader.h"
#include "../../../../src/util/error.h"
#include "../../../../src/util/memory.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

extern struct MimicConfig MimicConfig;

/* ---------------------------------------------------------------------------
 * Dataset facts
 *
 * Recorded by the 2026-08-03 conversion gate and reproduced by the producer
 * validation battery and the topology cross-check. A mismatch here means the
 * local dataset is not the one this package documents, which is worth failing
 * over rather than passing quietly.
 * ------------------------------------------------------------------------- */

#define REALDATA_SNAPSHOTS 50
#define REALDATA_TOTAL_HALOS INT64_C(22580924)
#define REALDATA_N_FORESTS_TOTAL INT64_C(440651)
#define REALDATA_MAX_RANK INT64_C(350074)
#define REALDATA_FORMAT_VERSION 1

static const char *package_path(const char *leaf) {
  static char path[MAX_STRING_LEN];
  snprintf(path, sizeof(path), "simulations/%s/%s", MIMIC_COMPILED_SIMULATION, leaf);
  return path;
}

/**
 * @test  test_open_run_against_full_dataset
 * open_run validates the whole converted dataset and reports its halo counts.
 */
int test_open_run_against_full_dataset(void) {
  static char skip_reason[2 * MAX_STRING_LEN];

  char data_dir[MAX_STRING_LEN];
  char a_list[MAX_STRING_LEN];
  snprintf(data_dir, sizeof(data_dir), "%s", package_path("snapshots"));
  snprintf(a_list, sizeof(a_list), "%s", package_path("micro-uchuu.a_list"));

  if (access(data_dir, R_OK) != 0) {
    snprintf(skip_reason, sizeof(skip_reason),
             "'%s' does not resolve; link it to the converted dataset (see that package's "
             "README.md)",
             data_dir);
    return TEST_SKIP_WITH(skip_reason);
  }

  memset(&MimicConfig, 0, sizeof(MimicConfig));
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s", data_dir);
  snprintf(MimicConfig.FileWithSnapList, sizeof(MimicConfig.FileWithSnapList), "%s", a_list);
  read_snap_list();
  MimicConfig.MAXSNAPS = MimicConfig.Snaplistlen;
  MimicConfig.UniqueGalaxyIDMultiplier = (int64_t)TREE_MUL_FAC;

  TEST_ASSERT_EQUAL(MimicConfig.Snaplistlen, REALDATA_SNAPSHOTS,
                    "the package snapshot list should hold fifty entries");

  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  TEST_ASSERT(reader != NULL, "snapshot_hdf5 should be registered");

  struct SnapshotRunInfo info;
  snapshot_reader_open_run(reader, &info);

  TEST_ASSERT_EQUAL(info.snapshot_count, REALDATA_SNAPSHOTS,
                    "run info should publish fifty snapshots");
  TEST_ASSERT_EQUAL(info.format_version, REALDATA_FORMAT_VERSION,
                    "run info should publish the on-disk format version");
  TEST_ASSERT_EQUAL(info.n_forests_total, REALDATA_N_FORESTS_TOTAL,
                    "run info should publish the recorded forest count");
  TEST_ASSERT_EQUAL(info.max_halo_rank_in_forest, REALDATA_MAX_RANK,
                    "run info should publish the recorded maximum halo rank");
  TEST_ASSERT(snapshot_identity_bounds_valid(&info, MimicConfig.UniqueGalaxyIDMultiplier) != 0,
              "the dataset bounds should be encodable with the default multiplier");

  int64_t total = 0;
  printf("  per-snapshot halo counts:\n");
  for (int64_t snap = 0; snap < info.snapshot_count; snap++) {
    const int64_t count = snapshot_reader_halo_count(reader, snap);
    TEST_ASSERT(count >= 0, "every snapshot should report a non-negative halo count");
    printf("    snapshot %03" PRId64 ": %10" PRId64 " halos\n", snap, count);
    total += count;
  }
  printf("    total      : %10" PRId64 " halos\n", total);

  TEST_ASSERT_EQUAL(total, REALDATA_TOTAL_HALOS,
                    "the dataset should hold the recorded number of halos");

  snapshot_reader_close_run(reader);
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Snapshot Reader (full dataset, opt-in)\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_open_run_against_full_dataset);

  TEST_SUMMARY();
  return TEST_RESULT();
}
