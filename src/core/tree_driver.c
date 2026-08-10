/**
 * @file    tree_driver.c
 * @brief   Tree-ordered partition driver for Mimic runs.
 */

#include <inttypes.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MPI
#include <mpi.h>
#endif

#ifdef HDF5
#include <hdf5.h>
#endif

#include "config.h"
#include "core/tree_driver.h"
#include "galaxy_id.h"
#include "galaxy_pool.h"
#include "globals.h"
#include "memory.h"
#include "progress.h"
#include "proto.h"
#include "run_log.h"
#include "tree/chunk_plan.h"
#include "tree/interface.h"
#include "tree/reader.h"

#include "error.h"
#include "output/hdf5.h"
#include "output/util.h"

#define MAX_PATH_BUF_SIZE (3 * MAX_STRING_LEN + 25)

/* Output paths of the partition currently being processed. Set before the
 * partition is claimed, cleared once it completes, and unlinked by bye() if the
 * program exits with a failure in between, so a crash never leaves partial
 * output files behind and never deletes completed ones. Binary output has one
 * path per requested snapshot; HDF5 output has one path per partition. */
static char current_output_paths[ABSOLUTEMAXSNAPS][MAX_PATH_BUF_SIZE + 1];
static int current_output_path_count = 0;

volatile sig_atomic_t TreeDriverGotXCPU = 0;

void tree_driver_clear_current_output_paths(void) {
  for (int i = 0; i < current_output_path_count; i++) {
    current_output_paths[i][0] = '\0';
  }
  current_output_path_count = 0;
}

void tree_driver_remove_incomplete_outputs(void) {
  for (int i = 0; i < current_output_path_count; i++) {
    if (current_output_paths[i][0] != '\0') {
      unlink(current_output_paths[i]);
    }
  }
}

static void set_current_output_paths(int output_id) {
  tree_driver_clear_current_output_paths();

#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    output_path_hdf5(current_output_paths[0], MAX_PATH_BUF_SIZE, output_id);
    current_output_path_count = 1;
    return;
  }
#endif

  for (int n = 0; n < MimicConfig.NOUT; n++) {
    output_path_binary(current_output_paths[n], MAX_PATH_BUF_SIZE, output_id, n);
  }
  current_output_path_count = MimicConfig.NOUT;
}

static int count_existing_current_outputs(void) {
  struct stat filestatus;
  int existing = 0;

  for (int i = 0; i < current_output_path_count; i++) {
    if (stat(current_output_paths[i], &filestatus) == 0) {
      existing++;
    }
  }

  return existing;
}

static void claim_current_output_paths(int output_id) {
  for (int i = 0; i < current_output_path_count; i++) {
    FILE *fd = fopen(current_output_paths[i], "w");
    if (fd == NULL) {
      FATAL_ERROR("Failed to claim output file '%s' for partition %d", current_output_paths[i],
                  output_id);
    }
    fclose(fd);
  }
}

static int effective_task_count(void) { return NTask > 0 ? NTask : 1; }

static int current_task_id(void) { return ThisTask >= 0 ? ThisTask : 0; }

static void reader_prepare_run(const struct TreeReader *reader) {
  if (reader->prepare_run != NULL) {
    reader->prepare_run();
  }
}

static void reader_teardown_run(const struct TreeReader *reader) {
  if (reader->teardown_run != NULL) {
    reader->teardown_run();
  }
}

#define REQUIRE_READER_HOOK(reader, member)                                                        \
  do {                                                                                             \
    if ((reader)->member == NULL) {                                                                \
      FATAL_ERROR("Tree reader '%s' is missing required partition hook '%s'", (reader)->name,      \
                  #member);                                                                        \
    }                                                                                              \
  } while (0)

static int reader_partition_exists(const struct TreeReader *reader, int partition) {
  REQUIRE_READER_HOOK(reader, partition_exists);
  return reader->partition_exists(partition);
}

static int64_t reader_count_partition_units(const struct TreeReader *reader, int partition) {
  REQUIRE_READER_HOOK(reader, count_partition_units);
  const int64_t units = reader->count_partition_units(partition);
  if (units < 0) {
    const int output_id =
        reader->partition_output_id != NULL ? reader->partition_output_id(partition) : partition;
    FATAL_ERROR("Tree reader '%s' reported negative unit count %" PRId64 " for partition %d",
                reader->name, units, output_id);
  }
  return units;
}

static void log_missing_per_file_partition(const struct TreeReader *reader, int partition) {
  char tree_path[MAX_PATH_BUF_SIZE + 1];
  const int output_id = reader->partition_output_id(partition);

  if (reader->format_partition_path != NULL) {
    reader->format_partition_path(tree_path, sizeof(tree_path), output_id);
  } else {
    snprintf(tree_path, sizeof(tree_path), "%s/%s.%d%s", MimicConfig.SimulationDir,
             MimicConfig.TreeName, output_id, MimicConfig.TreeExtension);
  }
  INFO_LOG("Missing tree %s ... skipping", tree_path);
}

static int64_t *build_partition_file_offsets(const struct TreeReader *reader, const int npartitions,
                                             int64_t *total_out) {
  int64_t total_forests = 0;
  int64_t *offsets = mymalloc_cat(sizeof(*offsets) * npartitions, MEM_TREES);

  REQUIRE_READER_HOOK(reader, partition_output_id);

  for (int partition = 0; partition < npartitions; partition++) {
    const int output_id = reader->partition_output_id(partition);
    offsets[partition] = total_forests;

    if (!reader_partition_exists(reader, partition)) {
      /* Preserve skip semantics: missing files do not consume forest-id space. */
      continue;
    }

    const int64_t partition_trees = reader_count_partition_units(reader, partition);
    if (partition_trees > LLONG_MAX - total_forests) {
      FATAL_ERROR("L-Halo total forest count would overflow int64 after partition %d", output_id);
    }
    total_forests += partition_trees;
    if (!mimic_unique_galaxy_id_total_forests_valid(total_forests)) {
      FATAL_ERROR("L-Halo total forest count %" PRId64
                  " exceeds the UniqueGalaxyID encoding limit of %" PRId64,
                  total_forests, mimic_unique_galaxy_id_max_forests());
    }
  }

  if (total_out)
    *total_out = total_forests;
  return offsets;
}

/**
 * @brief   Process every unit of one partition and finalize its output.
 */
static void process_partition(int output_id, ProgressBar *ext_bar, int64_t tree_base) {
  int unit, halonr;

  FileNum = output_id;
  open_partition(output_id);
  prepare_output_files(output_id);

  ProgressBar local_bar;
  ProgressBar *bar = ext_bar;
  if (bar == NULL) {
    char label[128] = "";
#ifdef MPI
    if (NTask > 1)
      snprintf(label, sizeof(label), "task %d of %d on %s", ThisTask, NTask, ThisNode);
#endif
    progress_bar_init(&local_bar, Ntrees, label);
    bar = &local_bar;
    tree_base = 0;
  }

  for (unit = 0; unit < Ntrees; unit++) {
    if (TreeDriverGotXCPU) {
      FATAL_ERROR("Received SIGXCPU (CPU time limit) — stopping before tree %d of file %d", unit,
                  output_id);
    }

    progress_bar_update(bar, tree_base + unit);

    TreeID = unit;
    load_unit(unit);

    NumProcessedHalos = 0;

    /* One explicit view over this unit's loaded halos, so the output writers
     * below take their raw halos from the driver rather than from the global. */
    const struct HaloInputView view = {InputTreeHalos, (int64_t)InputTreeNHalos[unit]};

    for (halonr = 0; halonr < InputTreeNHalos[unit]; halonr++)
      if (HaloAux[halonr].DoneFlag == 0)
        build_halo_tree(halonr, unit, 0);

#ifdef HDF5
    if (MimicConfig.OutputFormat == output_hdf5) {
      save_halos_hdf5(output_id, unit, view);
    } else {
      save_halos(output_id, unit, view);
    }
#else
    save_halos(output_id, unit, view);
#endif
    free_unit_halos();
  }

  if (ext_bar == NULL)
    progress_bar_finish(bar);

#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    flush_hdf5_buffers(output_id);

    for (int n = 0; n < MimicConfig.NOUT; n++) {
      write_hdf5_attrs(n, output_id);
    }

    if (HDF5_current_file_id >= 0) {
      DEBUG_LOG("Closing HDF5 file (ID %lld) for partition %d", (long long)HDF5_current_file_id,
                output_id);
      H5Fclose(HDF5_current_file_id);
      HDF5_current_file_id = -1;
    }
  } else {
    finalize_halo_file(output_id);
  }
#else
  finalize_halo_file(output_id);
#endif
  close_partition();
}

/**
 * @brief   Claim and process one output partition, honoring --skip.
 */
static int claim_and_process_partition(int output_id, ProgressBar *ext_bar, int64_t tree_base) {
  set_current_output_paths(output_id);
  int existing_outputs = count_existing_current_outputs();
  if (!MimicConfig.OverwriteOutputFiles) {
    if (existing_outputs == current_output_path_count) {
      INFO_LOG("Output file %d already exists ... skipping", output_id);
      tree_driver_clear_current_output_paths();
      return 0;
    }
    if (existing_outputs > 0) {
      FATAL_ERROR("Partial output exists for partition %d (%d of %d files). Remove the partial "
                  "files or rerun without --skip.",
                  output_id, existing_outputs, current_output_path_count);
    }
  }

  claim_current_output_paths(output_id);

  process_partition(output_id, ext_bar, tree_base);

  tree_driver_clear_current_output_paths();
  return 1;
}

static void run_per_file_driver(const struct TreeReader *reader) {
  REQUIRE_READER_HOOK(reader, num_partitions);
  REQUIRE_READER_HOOK(reader, partition_output_id);

  reader_prepare_run(reader);

  const int npartitions = reader->num_partitions();
  int64_t total_trees = 0;
  int64_t *global_forest_offsets = build_partition_file_offsets(reader, npartitions, &total_trees);

#ifdef MPI
  for (int partition = ThisTask; partition < npartitions; partition += NTask) {
    const int output_id = reader->partition_output_id(partition);
    if (!reader_partition_exists(reader, partition)) {
      log_missing_per_file_partition(reader, partition);
      continue;
    }
    GlobalForestOffset = global_forest_offsets[partition];
    if (claim_and_process_partition(output_id, NULL, 0) && !progress_display_active()) {
      INFO_LOG("%sCompleted input file %d%s", mimic_color_green(), output_id, mimic_color_reset());
    }
  }
#else
  ProgressBar global_bar;
  progress_bar_init(&global_bar, total_trees, "");

  for (int partition = 0; partition < npartitions; partition++) {
    const int output_id = reader->partition_output_id(partition);
    if (!reader_partition_exists(reader, partition)) {
      log_missing_per_file_partition(reader, partition);
      continue;
    }
    GlobalForestOffset = global_forest_offsets[partition];
    claim_and_process_partition(output_id, &global_bar, global_forest_offsets[partition]);
  }
  progress_bar_finish(&global_bar);
#endif

  myfree(global_forest_offsets);
  reader_teardown_run(reader);
}

static int64_t *build_enumerated_progress_offsets(const struct TreeReader *reader, int npartitions,
                                                  int64_t *total_out) {
  int64_t total_units = 0;
  int64_t *unit_offsets = mymalloc_cat(sizeof(*unit_offsets) * npartitions, MEM_TREES);

  for (int partition = 0; partition < npartitions; partition++) {
    unit_offsets[partition] = total_units;
    if (!reader_partition_exists(reader, partition)) {
      continue;
    }
    const int64_t units = reader_count_partition_units(reader, partition);
    if (units > LLONG_MAX - total_units) {
      FATAL_ERROR("Enumerated partition unit count would overflow after partition %d", partition);
    }
    total_units += units;
  }

  if (total_out != NULL)
    *total_out = total_units;
  return unit_offsets;
}

static int *assign_enumerated_partitions(const struct TreeReader *reader, int npartitions,
                                         int ntasks) {
  int *task_of_partition = mymalloc_cat(sizeof(*task_of_partition) * npartitions, MEM_TREES);
  int existing_count = 0;

  for (int partition = 0; partition < npartitions; partition++) {
    task_of_partition[partition] = -1;
    if (reader_partition_exists(reader, partition)) {
      existing_count++;
    }
  }

  if (existing_count == 0) {
    return task_of_partition;
  }

  double *costs = mymalloc_cat(sizeof(*costs) * existing_count, MEM_TREES);
  int *existing_partitions = mymalloc_cat(sizeof(*existing_partitions) * existing_count, MEM_TREES);
  int *task_of_existing = mymalloc_cat(sizeof(*task_of_existing) * existing_count, MEM_TREES);

  int existing_index = 0;
  for (int partition = 0; partition < npartitions; partition++) {
    if (!reader_partition_exists(reader, partition)) {
      continue;
    }
    existing_partitions[existing_index] = partition;
    costs[existing_index] = reader->partition_cost(partition);
    existing_index++;
  }

  if (chunk_plan_assign_lpt(existing_count, costs, ntasks, task_of_existing) != 0) {
    FATAL_ERROR("Failed to assign %d enumerated partitions across %d tasks for reader '%s'",
                existing_count, ntasks, reader->name);
  }

  for (existing_index = 0; existing_index < existing_count; existing_index++) {
    task_of_partition[existing_partitions[existing_index]] = task_of_existing[existing_index];
  }

  myfree(task_of_existing);
  myfree(existing_partitions);
  myfree(costs);
  return task_of_partition;
}

static void run_enumerated_driver(const struct TreeReader *reader) {
  REQUIRE_READER_HOOK(reader, num_partitions);
  REQUIRE_READER_HOOK(reader, partition_output_id);
  REQUIRE_READER_HOOK(reader, partition_exists);
  REQUIRE_READER_HOOK(reader, count_partition_units);
  REQUIRE_READER_HOOK(reader, global_forest_offset);
  REQUIRE_READER_HOOK(reader, partition_cost);

  reader_prepare_run(reader);

  const int npartitions = reader->num_partitions();
  if (npartitions < 0) {
    FATAL_ERROR("Tree reader '%s' reported negative partition count %d", reader->name, npartitions);
  }

  {
    const int nfiles = MimicConfig.LastFile - MimicConfig.FirstFile + 1;
#ifdef MPI
    const int ntasks = effective_task_count();
    if (ntasks > 1) {
      INFO_LOG("Processing %d input file%s (first_file=%d, last_file=%d) → %d output file%s across "
               "%d tasks",
               nfiles, nfiles == 1 ? "" : "s", MimicConfig.FirstFile, MimicConfig.LastFile,
               npartitions, npartitions == 1 ? "" : "s", ntasks);
    } else
#endif
    {
      INFO_LOG("Processing %d input file%s (first_file=%d, last_file=%d) → %d output file%s",
               nfiles, nfiles == 1 ? "" : "s", MimicConfig.FirstFile, MimicConfig.LastFile,
               npartitions, npartitions == 1 ? "" : "s");
    }
  }

  const int ntasks = effective_task_count();
  const int this_task = current_task_id();
  if (this_task >= ntasks) {
    FATAL_ERROR("Task id %d is outside task count %d", this_task, ntasks);
  }

  int64_t total_units = 0;
  int64_t *unit_offsets = build_enumerated_progress_offsets(reader, npartitions, &total_units);
  int *task_of_partition = assign_enumerated_partitions(reader, npartitions, ntasks);

#ifdef MPI
  for (int partition = 0; partition < npartitions; partition++) {
    if (task_of_partition[partition] != this_task || !reader_partition_exists(reader, partition)) {
      continue;
    }
    const int output_id = reader->partition_output_id(partition);
    GlobalForestOffset = reader->global_forest_offset(partition);
    if (claim_and_process_partition(output_id, NULL, 0) && !progress_display_active()) {
      INFO_LOG("%sCompleted output file %d%s", mimic_color_green(), output_id, mimic_color_reset());
    }
  }
#else
  ProgressBar global_bar;
  progress_bar_init(&global_bar, total_units, "");

  for (int partition = 0; partition < npartitions; partition++) {
    if (task_of_partition[partition] != this_task || !reader_partition_exists(reader, partition)) {
      continue;
    }
    const int output_id = reader->partition_output_id(partition);
    GlobalForestOffset = reader->global_forest_offset(partition);
    claim_and_process_partition(output_id, &global_bar, unit_offsets[partition]);
  }
  progress_bar_finish(&global_bar);
#endif

  myfree(task_of_partition);
  myfree(unit_offsets);
  reader_teardown_run(reader);
}

/**
 * @brief   Run the tree-ordered processing driver.
 */
void run_tree_driver(void) {
  log_phase_banner(PHASE_TREE_PROCESSING);
  enable_debug_log_rate_limiting();
  const struct TreeReader *reader = MimicConfig.reader;

  if (reader->partition_model != PARTITION_ENUMERATED) {
    const int nfiles = MimicConfig.LastFile - MimicConfig.FirstFile + 1;
#ifdef MPI
    const int ntasks = effective_task_count();
    if (ntasks > 1) {
      INFO_LOG("Processing %d input file%s (first_file=%d, last_file=%d) → %d output file%s across "
               "%d tasks",
               nfiles, nfiles == 1 ? "" : "s", MimicConfig.FirstFile, MimicConfig.LastFile, nfiles,
               nfiles == 1 ? "" : "s", ntasks);
    } else
#endif
    {
      INFO_LOG("Processing %d input file%s (first_file=%d, last_file=%d) → %d output file%s",
               nfiles, nfiles == 1 ? "" : "s", MimicConfig.FirstFile, MimicConfig.LastFile, nfiles,
               nfiles == 1 ? "" : "s");
    }
  }

  switch (reader->partition_model) {
  case PARTITION_PER_FILE:
    run_per_file_driver(reader);
    break;
  case PARTITION_ENUMERATED:
    run_enumerated_driver(reader);
    break;
  default:
    FATAL_ERROR("Unknown partition model %d for tree reader '%s'", reader->partition_model,
                reader->name);
  }

  disable_debug_log_rate_limiting();
}

/**
 * @brief   Dispatch to the processing driver selected by input.processing_order.
 */
void run_processing_driver(void) {
  switch ((enum InputProcessingOrder)MimicConfig.ProcessingOrder) {
  case INPUT_PROCESSING_ORDER_TREE:
    run_tree_driver();
    return;
  case INPUT_PROCESSING_ORDER_SNAPSHOT:
    run_snapshot_driver();
    return;
  }

  FATAL_ERROR("Unknown input.processing_order '%s'",
              input_processing_order_name((enum InputProcessingOrder)MimicConfig.ProcessingOrder));
}
