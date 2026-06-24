/**
 * @file    test_enumerated_driver.c
 * @brief   Unit tests for the reader-enumerated tree driver path.
 */

#include "../framework/test_framework.h"

#include "core/tree_driver.h"
#include "error.h"
#include "globals.h"
#include "memory.h"
#include "output/util.h"
#include "tree/reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int passed = 0, failed = 0;

#define MAX_SYNTH_PARTITIONS 8
#define MAX_OPEN_RECORDS 16

static int synthetic_npartitions;
static int synthetic_units[MAX_SYNTH_PARTITIONS];
static int synthetic_exists[MAX_SYNTH_PARTITIONS];
static int synthetic_output_ids[MAX_SYNTH_PARTITIONS];
static int64_t synthetic_offsets[MAX_SYNTH_PARTITIONS];
static double synthetic_costs[MAX_SYNTH_PARTITIONS];
static int prepare_calls;
static int teardown_calls;
static int open_calls;
static int close_calls;
static int opened_partitions[MAX_OPEN_RECORDS];
static int64_t opened_offsets[MAX_OPEN_RECORDS];

static void reset_synthetic_state(void) {
  synthetic_npartitions = 0;
  memset(synthetic_units, 0, sizeof(synthetic_units));
  memset(synthetic_exists, 0, sizeof(synthetic_exists));
  for (int partition = 0; partition < MAX_SYNTH_PARTITIONS; partition++) {
    synthetic_output_ids[partition] = partition;
  }
  memset(synthetic_offsets, 0, sizeof(synthetic_offsets));
  memset(synthetic_costs, 0, sizeof(synthetic_costs));
  prepare_calls = 0;
  teardown_calls = 0;
  open_calls = 0;
  close_calls = 0;
  memset(opened_partitions, -1, sizeof(opened_partitions));
  memset(opened_offsets, 0, sizeof(opened_offsets));
}

static void synthetic_prepare_run(void) { prepare_calls++; }

static void synthetic_teardown_run(void) { teardown_calls++; }

static int synthetic_num_partitions(void) { return synthetic_npartitions; }

static int synthetic_partition_output_id(int partition) { return synthetic_output_ids[partition]; }

static int synthetic_partition_exists(int partition) { return synthetic_exists[partition]; }

static int64_t synthetic_count_partition_units(int partition) { return synthetic_units[partition]; }

static int64_t synthetic_global_forest_offset(int partition) {
  return synthetic_offsets[partition];
}

static double synthetic_partition_cost(int partition) { return synthetic_costs[partition]; }

static void synthetic_open_partition(int output_id) {
  if (open_calls >= MAX_OPEN_RECORDS) {
    FATAL_ERROR("Too many synthetic partition opens");
  }
  int partition = -1;
  for (int candidate = 0; candidate < synthetic_npartitions; candidate++) {
    if (synthetic_output_ids[candidate] == output_id) {
      partition = candidate;
      break;
    }
  }
  if (partition < 0) {
    FATAL_ERROR("Unknown synthetic output id %d", output_id);
  }

  opened_partitions[open_calls] = output_id;
  opened_offsets[open_calls] = GlobalForestOffset;
  open_calls++;

  Ntrees = synthetic_units[partition];
  InputTreeNHalos = mymalloc_cat(sizeof(int) * Ntrees, MEM_TREES);
  InputTreeFirstHalo = mymalloc_cat(sizeof(int) * Ntrees, MEM_TREES);
  for (int unit = 0; unit < Ntrees; unit++) {
    InputTreeNHalos[unit] = 1;
    InputTreeFirstHalo[unit] = unit;
  }
}

static void synthetic_load_unit(int unit) {
  (void)unit;
  /* The shared load_unit() wrapper allocates HaloAux after this callback. */
  InputTreeHalos = mymalloc_cat(sizeof(struct RawHalo), MEM_TREES);
}

static void synthetic_close_partition(void) { close_calls++; }

static const struct TreeReader SyntheticEnumeratedReader = {
    .name = "synthetic_enumerated",
    .file_extension = "",
    .partition_model = PARTITION_ENUMERATED,
    .processing_order = INPUT_PROCESSING_ORDER_TREE,
    .prepare_run = synthetic_prepare_run,
    .teardown_run = synthetic_teardown_run,
    .num_partitions = synthetic_num_partitions,
    .partition_output_id = synthetic_partition_output_id,
    .partition_exists = synthetic_partition_exists,
    .format_partition_path = NULL,
    .count_partition_units = synthetic_count_partition_units,
    .global_forest_offset = synthetic_global_forest_offset,
    .partition_cost = synthetic_partition_cost,
    .open_partition = synthetic_open_partition,
    .load_unit = synthetic_load_unit,
    .close_partition = synthetic_close_partition,
};

static void configure_driver_defaults(void) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  MimicConfig.reader = &SyntheticEnumeratedReader;
  MimicConfig.ProcessingOrder = INPUT_PROCESSING_ORDER_TREE;
  MimicConfig.OverwriteOutputFiles = 1;
  MimicConfig.OutputFormat = output_binary;
  snprintf(MimicConfig.OutputFileBaseName, sizeof(MimicConfig.OutputFileBaseName), "%s",
           "synthetic");
  ThisTask = 0;
  NTask = 1;
  GlobalForestOffset = 0;
  TreeDriverGotXCPU = 0;
  tree_driver_clear_current_output_paths();
}

static void set_partition(int partition, int units, double cost, int64_t offset) {
  synthetic_exists[partition] = 1;
  synthetic_units[partition] = units;
  synthetic_costs[partition] = cost;
  synthetic_offsets[partition] = offset;
}

static void set_partition_output_id(int partition, int output_id) {
  synthetic_output_ids[partition] = output_id;
}

static int create_temp_output_dir(char *dir_template) {
  char *dir = mkdtemp(dir_template);
  TEST_ASSERT(dir != NULL, "mkdtemp should create an output directory");
  snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", dir);
  return TEST_PASS;
}

static void remove_skip_fixture(const char *dir) {
  char path[512];
  output_path_binary(path, sizeof(path), 0, 0);
  unlink(path);
  rmdir(dir);
}

static int test_output_claim_forward_path_creates_and_clears_file(void) {
  char dir_template[] = "/tmp/mimic_enumerated_driver_forward_XXXXXX";
  char output_path[512];

  configure_driver_defaults();
  reset_synthetic_state();
  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS,
              "temporary output directory should be configured");

  synthetic_npartitions = 1;
  set_partition(0, 1, 1.0, 400);
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = 0;
  MimicConfig.ZZ[0] = 0.0;

  output_path_binary(output_path, sizeof(output_path), 0, 0);
  TEST_ASSERT(access(output_path, F_OK) != 0, "output file should not exist before processing");

  run_tree_driver();

  TEST_ASSERT_EQUAL(prepare_calls, 1, "forward run should prepare reader state once");
  TEST_ASSERT_EQUAL(teardown_calls, 1, "forward run should tear reader state down once");
  TEST_ASSERT_EQUAL(open_calls, 1, "forward run should open the partition");
  TEST_ASSERT_EQUAL(close_calls, 1, "forward run should close the partition");
  TEST_ASSERT(access(output_path, F_OK) == 0, "forward run should create the output file");
  tree_driver_remove_incomplete_outputs();
  TEST_ASSERT(access(output_path, F_OK) == 0,
              "completed output should not remain registered for failure cleanup");

  unlink(output_path);
  rmdir(dir_template);
  return TEST_PASS;
}

static int test_lpt_assignment_processes_current_task_in_ascending_order(void) {
  configure_driver_defaults();
  reset_synthetic_state();

  synthetic_npartitions = 5;
  set_partition(0, 1, 10.0, 100);
  set_partition(1, 1, 1.0, 101);
  set_partition(2, 1, 8.0, 102);
  set_partition(3, 1, 1.0, 103);
  set_partition(4, 1, 6.0, 104);
  ThisTask = 1;
  NTask = 2;

  run_tree_driver();

  TEST_ASSERT_EQUAL(prepare_calls, 1, "prepare_run should be called once");
  TEST_ASSERT_EQUAL(teardown_calls, 1, "teardown_run should be called once");
  TEST_ASSERT_EQUAL(open_calls, 2, "task 1 should receive two LPT-assigned partitions");
  TEST_ASSERT_EQUAL(close_calls, 2, "each opened partition should close");
  TEST_ASSERT_EQUAL(opened_partitions[0], 2, "task 1 should process partition 2 first");
  TEST_ASSERT_EQUAL(opened_partitions[1], 4, "task 1 should process partition 4 second");
  TEST_ASSERT(opened_partitions[0] < opened_partitions[1],
              "assigned partitions should be processed in ascending id order");
  TEST_ASSERT_EQUAL(opened_offsets[0], 102, "driver should publish partition 2 forest offset");
  TEST_ASSERT_EQUAL(opened_offsets[1], 104, "driver should publish partition 4 forest offset");

  return TEST_PASS;
}

static int test_driver_opens_reader_output_ids(void) {
  configure_driver_defaults();
  reset_synthetic_state();

  synthetic_npartitions = 2;
  set_partition(0, 1, 5.0, 150);
  set_partition_output_id(0, 10);
  set_partition(1, 1, 4.0, 151);
  set_partition_output_id(1, 12);

  run_tree_driver();

  TEST_ASSERT_EQUAL(open_calls, 2, "driver should open both existing partitions");
  TEST_ASSERT_EQUAL(opened_partitions[0], 10, "driver should open partition 0's output id");
  TEST_ASSERT_EQUAL(opened_partitions[1], 12, "driver should open partition 1's output id");
  TEST_ASSERT_EQUAL(opened_offsets[0], 150, "driver should publish partition 0 forest offset");
  TEST_ASSERT_EQUAL(opened_offsets[1], 151, "driver should publish partition 1 forest offset");

  return TEST_PASS;
}

static int test_missing_partition_has_zero_lpt_cost(void) {
  configure_driver_defaults();
  reset_synthetic_state();

  synthetic_npartitions = 3;
  set_partition(0, 1, 10.0, 500);
  synthetic_exists[1] = 0;
  synthetic_units[1] = 1;
  synthetic_costs[1] = 1000.0;
  synthetic_offsets[1] = 501;
  set_partition(2, 1, 1.0, 502);
  ThisTask = 1;
  NTask = 2;

  run_tree_driver();

  TEST_ASSERT_EQUAL(open_calls, 1, "missing partition cost should not skew assignment onto task 1");
  TEST_ASSERT_EQUAL(opened_partitions[0], 2,
                    "task 1 should receive the low-cost existing partition");
  TEST_ASSERT_EQUAL(opened_offsets[0], 502,
                    "driver should publish the existing partition forest offset");

  return TEST_PASS;
}

static int test_idle_rank_runs_lifecycle_without_opening_partition(void) {
  configure_driver_defaults();
  reset_synthetic_state();

  synthetic_npartitions = 2;
  set_partition(0, 1, 1.0, 200);
  set_partition(1, 1, 1.0, 201);
  ThisTask = 3;
  NTask = 4;

  run_tree_driver();

  TEST_ASSERT_EQUAL(prepare_calls, 1, "idle task should still prepare reader run state once");
  TEST_ASSERT_EQUAL(teardown_calls, 1, "idle task should still tear reader run state down once");
  TEST_ASSERT_EQUAL(open_calls, 0, "idle task should not open any partition");
  TEST_ASSERT_EQUAL(close_calls, 0, "idle task should not close an unopened partition");

  return TEST_PASS;
}

static int test_skip_existing_output_preserves_lifecycle(void) {
  char dir_template[] = "/tmp/mimic_enumerated_driver_XXXXXX";
  char output_path[512];

  configure_driver_defaults();
  reset_synthetic_state();
  TEST_ASSERT(create_temp_output_dir(dir_template) == TEST_PASS,
              "temporary output directory should be configured");

  synthetic_npartitions = 1;
  set_partition(0, 1, 1.0, 300);
  MimicConfig.NOUT = 1;
  MimicConfig.ListOutputSnaps[0] = 0;
  MimicConfig.ZZ[0] = 0.0;
  MimicConfig.OverwriteOutputFiles = 0;

  output_path_binary(output_path, sizeof(output_path), 0, 0);
  FILE *fd = fopen(output_path, "w");
  TEST_ASSERT(fd != NULL, "pre-existing output file should be creatable");
  fclose(fd);

  run_tree_driver();

  TEST_ASSERT_EQUAL(prepare_calls, 1, "skip run should prepare reader state once");
  TEST_ASSERT_EQUAL(teardown_calls, 1, "skip run should tear reader state down once");
  TEST_ASSERT_EQUAL(open_calls, 0, "skipped partition should not be opened");
  TEST_ASSERT_EQUAL(close_calls, 0, "skipped partition should not be closed");

  remove_skip_fixture(dir_template);
  return TEST_PASS;
}

int main(void) {
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
  init_memory_system(0);

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Enumerated Tree Driver\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  TEST_RUN(test_lpt_assignment_processes_current_task_in_ascending_order);
  TEST_RUN(test_driver_opens_reader_output_ids);
  TEST_RUN(test_missing_partition_has_zero_lpt_cost);
  TEST_RUN(test_idle_rank_runs_lifecycle_without_opening_partition);
  TEST_RUN(test_skip_existing_output_preserves_lifecycle);
  TEST_RUN(test_output_claim_forward_path_creates_and_clears_file);

  TEST_SUMMARY();

  check_memory_leaks();
  cleanup_memory_system();
  return TEST_RESULT();
}
