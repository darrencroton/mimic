/**
 * @file    test_created_record_identity.c
 * @brief   Unit tests for the created-record identity encoder, its int64 budget
 *          predicate, and the lhalo_binary largest-forest hook through the driver scan.
 *
 * The encoder and predicate (galaxy_id.h) are pure arithmetic and are checked
 * against hand-computed values, including the exact int64 boundary. The reader
 * hook is checked on the mini-Millennium package data that ./scripts/first_run.sh
 * fetches: the test reads the eight L-Halo headers itself and compares their
 * largest TreeNHalos with the run-wide value the vertical driver's own startup
 * scan publishes. The unknown (-1) fallback is covered through the synthetic
 * reader in test_enumerated_driver.c.
 */

#include "../framework/test_framework.h"

#include "core/vertical_driver.h"
#include "error.h"
#include "galaxy_id.h"
#include "globals.h"
#include "memory.h"
#include "output/util.h"
#include "vertical/reader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const struct VerticalReader LHaloBinaryReader;

static int passed = 0, failed = 0;

/* floor(INT64_MAX / 1024) = 2^53 - 1: the largest units * rows_per_unit product that fits. */
#define MAX_HOST_KEYS (INT64_MAX / MAX_CREATED_RECORDS_PER_HOST)

#define MINI_MILLENNIUM_DIR "simulations/mini-millennium/snapshots"
#define MINI_MILLENNIUM_TREE_NAME "trees_063"
#define MINI_MILLENNIUM_FIRST_FILE 0
#define MINI_MILLENNIUM_LAST_FILE 7

/**
 * @test    test_radix_constant
 * @brief   The identity radix is 1024
 */
static int test_radix_constant(void) {
  TEST_ASSERT_EQUAL(MAX_CREATED_RECORDS_PER_HOST, 1024, "identity radix should be 1024");
  TEST_ASSERT_EQUAL(MAX_HOST_KEYS, 9007199254740991LL, "host-key bound should be 2^53 - 1");
  return TEST_PASS;
}

/**
 * @test    test_encoder_bit_patterns
 * @brief   Small inputs encode to the hand-computed -(1 + ordinal + 1024 * host_key)
 */
static int test_encoder_bit_patterns(void) {
  TEST_ASSERT_EQUAL(mimic_encode_created_galaxy_id(0, 0, 1, 0), -1LL,
                    "first record of the first host is -1");
  TEST_ASSERT_EQUAL(mimic_encode_created_galaxy_id(0, 0, 1, 1), -2LL,
                    "second ordinal of the first host is -2");
  TEST_ASSERT_EQUAL(mimic_encode_created_galaxy_id(0, 0, 1, 1023), -1024LL,
                    "last ordinal of the first host is -1024");
  TEST_ASSERT_EQUAL(mimic_encode_created_galaxy_id(0, 1, 2, 0), -1025LL,
                    "next row starts one radix further down");
  TEST_ASSERT_EQUAL(mimic_encode_created_galaxy_id(1, 0, 2, 0), -2049LL,
                    "next unit starts rows_per_unit radices further down");
  TEST_ASSERT_EQUAL(mimic_encode_created_galaxy_id(2, 3, 5, 7), -13320LL,
                    "host_key 13, ordinal 7 encodes to -(8 + 1024 * 13)");
  return TEST_PASS;
}

/**
 * @test    test_encoder_strictly_negative_and_injective
 * @brief   Every created ID over a small space is strictly negative and distinct, and the
 *          largest magnitude equals 1024 * units * rows_per_unit
 */
static int test_encoder_strictly_negative_and_injective(void) {
  enum { UNITS = 3, ROWS = 4, ORDINALS = 3 };
  int64_t seen[UNITS * ROWS * ORDINALS];
  int count = 0;

  for (int64_t unit = 0; unit < UNITS; unit++) {
    for (int64_t row = 0; row < ROWS; row++) {
      for (int ordinal = 0; ordinal < ORDINALS; ordinal++) {
        const int64_t id = mimic_encode_created_galaxy_id(unit, row, ROWS, ordinal);
        TEST_ASSERT(id < 0, "every created ID should be strictly negative");
        for (int i = 0; i < count; i++) {
          TEST_ASSERT(seen[i] != id, "created IDs should be distinct");
        }
        seen[count++] = id;
      }
    }
  }

  /* Largest magnitude of the boundary space: exactly INT64_MAX rounded down to 1024. */
  const int64_t units = 6361;
  const int64_t rows = 1416003655831LL; /* 6361 * 1416003655831 = 2^53 - 1 */
  const int64_t deepest =
      mimic_encode_created_galaxy_id(units - 1, rows - 1, rows, MAX_CREATED_RECORDS_PER_HOST - 1);
  TEST_ASSERT_EQUAL(deepest, -(INT64_MAX - (MAX_CREATED_RECORDS_PER_HOST - 1)),
                    "deepest ID of a just-fitting space is -(1024 * units * rows_per_unit)");
  TEST_ASSERT(deepest < 0, "deepest ID should still be negative");
  return TEST_PASS;
}

/**
 * @test    test_predicate_exact_boundary
 * @brief   A product times 1024 equal to INT64_MAX rounded down fits; the next integer up
 *          does not
 */
static int test_predicate_exact_boundary(void) {
  /* units * rows_per_unit == 2^53 - 1, so 1024 * product == INT64_MAX - 1023. */
  TEST_ASSERT(mimic_created_record_space_fits(6361, 1416003655831LL),
              "6361 * 1416003655831 * 1024 = INT64_MAX - 1023 should fit");
  TEST_ASSERT(!mimic_created_record_space_fits(6361, 1416003655832LL),
              "one more row per unit should not fit");
  TEST_ASSERT(!mimic_created_record_space_fits(6362, 1416003655831LL),
              "one more unit should not fit");

  TEST_ASSERT(mimic_created_record_space_fits(1, MAX_HOST_KEYS),
              "a single unit of 2^53 - 1 rows should fit");
  TEST_ASSERT(!mimic_created_record_space_fits(1, MAX_HOST_KEYS + 1),
              "a single unit of 2^53 rows should not fit");
  TEST_ASSERT(mimic_created_record_space_fits(MAX_HOST_KEYS, 1),
              "2^53 - 1 units of one row should fit");
  TEST_ASSERT(!mimic_created_record_space_fits(MAX_HOST_KEYS + 1, 1),
              "2^53 units of one row should not fit");
  TEST_ASSERT(!mimic_created_record_space_fits(INT64_MAX, INT64_MAX),
              "the largest operands should not fit and must not overflow");
  return TEST_PASS;
}

/**
 * @test    test_predicate_zero_and_negative
 * @brief   Zero units or zero rows fit whatever the other operand; negative inputs never fit
 */
static int test_predicate_zero_and_negative(void) {
  TEST_ASSERT(mimic_created_record_space_fits(0, 0), "an empty space should fit");
  TEST_ASSERT(mimic_created_record_space_fits(0, INT64_MAX), "zero units should fit");
  TEST_ASSERT(mimic_created_record_space_fits(INT64_MAX, 0), "zero rows should fit");
  TEST_ASSERT(!mimic_created_record_space_fits(-1, 1), "negative units should not fit");
  TEST_ASSERT(!mimic_created_record_space_fits(1, -1), "negative rows should not fit");
  return TEST_PASS;
}

/**
 * @test    test_predicate_declared_package_verdicts
 * @brief   Verdicts for the packages' declared sizes: full Millennium fits vertically and
 *          horizontally; Shin-Uchuu ASCII, whose largest forest is unknown, does not
 */
static int test_predicate_declared_package_verdicts(void) {
  /* Full Millennium, vertical: 14329882 forests, largest 514194 halos -> 7.5e15. */
  TEST_ASSERT(mimic_created_record_space_fits(14329882, 514194),
              "full Millennium vertical should fit");
  /* Full Millennium, horizontal: 64 snapshots, largest slab 18619466 rows -> 1.2e12. */
  TEST_ASSERT(mimic_created_record_space_fits(64, 18619466),
              "full Millennium horizontal should fit");
  /* Shin-Uchuu ASCII, vertical: 166547771 forests, largest unknown -> M = 2e10. */
  TEST_ASSERT(!mimic_created_record_space_fits(166547771, 20000000000LL),
              "Shin-Uchuu ASCII vertical should not fit");
  return TEST_PASS;
}

/* ------------------------------------------------------------------------- */
/* lhalo_binary hook on the mini-Millennium package data                      */
/* ------------------------------------------------------------------------- */

static void mini_millennium_tree_path(char *buf, size_t size, int filenr) {
  snprintf(buf, size, "%s/%s.%d", MINI_MILLENNIUM_DIR, MINI_MILLENNIUM_TREE_NAME, filenr);
}

static int mini_millennium_present(void) {
  char path[512];
  for (int filenr = MINI_MILLENNIUM_FIRST_FILE; filenr <= MINI_MILLENNIUM_LAST_FILE; filenr++) {
    mini_millennium_tree_path(path, sizeof(path), filenr);
    if (access(path, R_OK) != 0) {
      return 0;
    }
  }
  return 1;
}

/* Largest TreeNHalos in one L-Halo binary header, read independently of the reader:
 * int Ntrees, int totNHalos, int TreeNHalos[Ntrees]. Returns -1 on a read failure. */
static int64_t header_max_tree_nhalos(int filenr) {
  char path[512];
  int ntrees, tot_nhalos;
  mini_millennium_tree_path(path, sizeof(path), filenr);

  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }
  int64_t max_halos = -1;
  if (fread(&ntrees, sizeof(int), 1, fp) == 1 && fread(&tot_nhalos, sizeof(int), 1, fp) == 1 &&
      ntrees >= 0) {
    max_halos = 0;
    for (int i = 0; i < ntrees; i++) {
      int nhalos;
      if (fread(&nhalos, sizeof(int), 1, fp) != 1) {
        max_halos = -1;
        break;
      }
      if (nhalos > max_halos) {
        max_halos = nhalos;
      }
    }
  }
  fclose(fp);
  return max_halos;
}

/** @brief The header's Ntrees (the file's forest count), or -1 when unreadable */
static int64_t header_ntrees(int filenr) {
  char path[512];
  int ntrees;
  mini_millennium_tree_path(path, sizeof(path), filenr);

  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }
  const int64_t result = (fread(&ntrees, sizeof(int), 1, fp) == 1 && ntrees >= 0) ? ntrees : -1;
  fclose(fp);
  return result;
}

/* Stand-ins for the per-partition hooks. The test pre-creates every partition's
 * output and runs with --skip semantics, so the driver runs its real startup scan
 * over the real headers and then skips every partition: none of these may run. */
static void scan_only_open_partition(int output_id) {
  FATAL_ERROR("scan-only reader should never open partition %d", output_id);
}

static void scan_only_load_unit(int unit) {
  FATAL_ERROR("scan-only reader should never load unit %d", unit);
}

static void scan_only_close_partition(void) {
  FATAL_ERROR("scan-only reader should never close a partition");
}

static void configure_mini_millennium(const struct VerticalReader *reader) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.vertical_reader = reader;
  MimicConfig.ProcessingOrder = INPUT_PROCESSING_ORDER_VERTICAL;
  MimicConfig.UniqueGalaxyIDMultiplier = TREE_MUL_FAC;
  MimicConfig.FirstFile = MINI_MILLENNIUM_FIRST_FILE;
  MimicConfig.LastFile = MINI_MILLENNIUM_LAST_FILE;
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s", MINI_MILLENNIUM_DIR);
  snprintf(MimicConfig.TreeName, sizeof(MimicConfig.TreeName), "%s", MINI_MILLENNIUM_TREE_NAME);
  MimicConfig.TreeExtension[0] = '\0';
  ThisTask = 0;
  NTask = 1;
  GlobalForestOffset = 0;
  VerticalDriverGotXCPU = 0;
  vertical_driver_clear_current_output_paths();
}

/**
 * @test    test_lhalo_binary_hook_on_mini_millennium
 * @brief   Per file, the hook equals the largest TreeNHalos of that header; run-wide, the
 *          driver's startup scan publishes the largest over all eight headers and, as units,
 *          the sum of their Ntrees
 */
static int test_lhalo_binary_hook_on_mini_millennium(void) {
  if (!mini_millennium_present()) {
    return TEST_SKIP_WITH("mini-Millennium tree files simulations/mini-millennium/snapshots/"
                          "trees_063.0-7 are absent; run ./scripts/first_run.sh to fetch them");
  }

  TEST_ASSERT(LHaloBinaryReader.max_partition_unit_halos != NULL,
              "lhalo_binary should provide the largest-unit hook");

  configure_mini_millennium(&LHaloBinaryReader);
  int64_t expected_run_max = 0;
  int64_t expected_units = 0;
  for (int filenr = MINI_MILLENNIUM_FIRST_FILE; filenr <= MINI_MILLENNIUM_LAST_FILE; filenr++) {
    const int64_t ntrees = header_ntrees(filenr);
    TEST_ASSERT(ntrees > 0, "test should read a positive Ntrees from the header");
    expected_units += ntrees;
    const int64_t header_max = header_max_tree_nhalos(filenr);
    TEST_ASSERT(header_max > 0, "test should read a positive TreeNHalos maximum from the header");
    const int partition = filenr - MINI_MILLENNIUM_FIRST_FILE;
    TEST_ASSERT_EQUAL(LHaloBinaryReader.max_partition_unit_halos(partition), header_max,
                      "hook should equal the file header's largest TreeNHalos");
    if (header_max > expected_run_max) {
      expected_run_max = header_max;
    }
  }

  /* The driver's own scan: real enumeration, counts and largest-unit hooks over the
   * real files. Every output already exists and --skip is in force, so no partition
   * is opened and no tree is processed. */
  struct VerticalReader scan_reader = LHaloBinaryReader;
  scan_reader.open_partition = scan_only_open_partition;
  scan_reader.load_unit = scan_only_load_unit;
  scan_reader.close_partition = scan_only_close_partition;

  char dir_template[] = "/tmp/mimic_created_identity_XXXXXX";
  char *dir = mkdtemp(dir_template);
  TEST_ASSERT(dir != NULL, "mkdtemp should create an output directory");

  configure_mini_millennium(&scan_reader);
  snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", dir);
  snprintf(MimicConfig.OutputFileBaseName, sizeof(MimicConfig.OutputFileBaseName), "%s",
           "identity");
  MimicConfig.OverwriteOutputFiles = 0;
  MimicConfig.OutputFormat = output_binary;
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = 0;
  MimicConfig.ZZ[0] = 0.0;

  char output_path[512];
  for (int filenr = MINI_MILLENNIUM_FIRST_FILE; filenr <= MINI_MILLENNIUM_LAST_FILE; filenr++) {
    output_path_binary(output_path, sizeof(output_path), filenr, 0);
    FILE *fd = fopen(output_path, "w");
    TEST_ASSERT(fd != NULL, "pre-existing output file should be creatable");
    fclose(fd);
  }

  run_vertical_driver();

  const struct RecordIdentitySpace space = vertical_driver_record_identity_space();
  TEST_ASSERT_EQUAL(space.rows_per_unit, expected_run_max,
                    "run-wide rows_per_unit should be the largest TreeNHalos of the eight headers");
  TEST_ASSERT_EQUAL(space.units, expected_units,
                    "units should be the run's forest count, the sum of the eight headers' Ntrees");
  TEST_ASSERT(space.fits, "the mini-Millennium identity space should fit int64");
  TEST_ASSERT_EQUAL(space.unit, -1, "a run that processed no unit publishes no unit");

  for (int filenr = MINI_MILLENNIUM_FIRST_FILE; filenr <= MINI_MILLENNIUM_LAST_FILE; filenr++) {
    output_path_binary(output_path, sizeof(output_path), filenr, 0);
    unlink(output_path);
  }
  rmdir(dir);
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Created-Record Identity\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_radix_constant);
  TEST_RUN(test_encoder_bit_patterns);
  TEST_RUN(test_encoder_strictly_negative_and_injective);
  TEST_RUN(test_predicate_exact_boundary);
  TEST_RUN(test_predicate_zero_and_negative);
  TEST_RUN(test_predicate_declared_package_verdicts);
  TEST_RUN(test_lhalo_binary_hook_on_mini_millennium);

  TEST_SUMMARY();

  check_memory_leaks();
  cleanup_memory_system();
  return TEST_RESULT();
}
