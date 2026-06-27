/**
 * @file    test_tree_reader_counts.c
 * @brief   Unit tests for allocation-free tree-reader count callbacks.
 */

#include "../framework/test_framework.h"

#include "globals.h"
#include "tree/reader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const struct TreeReader LHaloBinaryReader;

static int passed = 0, failed = 0;

static void reset_tree_config(const char *simulation_dir) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s", simulation_dir);
  snprintf(MimicConfig.TreeName, sizeof(MimicConfig.TreeName), "%s", "tiny_trees");
  MimicConfig.TreeExtension[0] = '\0';
  MimicConfig.FirstFile = 7;
  MimicConfig.LastFile = 7;

  Ntrees = -777;
  InputTreeNHalos = NULL;
  InputTreeFirstHalo = NULL;
}

static int write_lhalo_binary_header(const char *simulation_dir, int output_id, int ntrees,
                                     int total_halos) {
  char path[512];
  snprintf(path, sizeof(path), "%s/tiny_trees.%d", simulation_dir, output_id);

  FILE *fp = fopen(path, "wb");
  if (fp == NULL) {
    return -1;
  }

  if (fwrite(&ntrees, sizeof(ntrees), 1, fp) != 1 ||
      fwrite(&total_halos, sizeof(total_halos), 1, fp) != 1) {
    fclose(fp);
    return -1;
  }

  for (int i = 0; i < ntrees; i++) {
    const int halos_this_tree = i + 1;
    if (fwrite(&halos_this_tree, sizeof(halos_this_tree), 1, fp) != 1) {
      fclose(fp);
      return -1;
    }
  }

  return fclose(fp);
}

/**
 * @test  test_lhalo_binary_count_partition_units
 * Reads only the binary header tree count and leaves open-partition globals
 * untouched.
 */
int test_lhalo_binary_count_partition_units(void) {
  char dir_template[] = "/tmp/mimic_tree_counts_XXXXXX";
  char *dir = mkdtemp(dir_template);
  TEST_ASSERT(dir != NULL, "mkdtemp should create a temp directory");

  const int output_id = 7;
  const int ntrees = 4;
  TEST_ASSERT(write_lhalo_binary_header(dir, output_id, ntrees, 10) == 0,
              "should write a tiny L-Halo binary header");
  reset_tree_config(dir);

  TEST_ASSERT(LHaloBinaryReader.count_partition_units != NULL,
              "L-Halo binary reader should expose a count callback");
  TEST_ASSERT_EQUAL(LHaloBinaryReader.count_partition_units(0), ntrees,
                    "count callback should return Ntrees from the header");
  TEST_ASSERT_EQUAL(Ntrees, -777, "count callback should not stage Ntrees globally");
  TEST_ASSERT(InputTreeNHalos == NULL, "count callback should not allocate InputTreeNHalos");
  TEST_ASSERT(InputTreeFirstHalo == NULL, "count callback should not allocate InputTreeFirstHalo");

  char path[512];
  snprintf(path, sizeof(path), "%s/tiny_trees.%d", dir, output_id);
  unlink(path);
  rmdir(dir);

  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Tree Reader Counts\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_lhalo_binary_count_partition_units);

  TEST_SUMMARY();
  return TEST_RESULT();
}
