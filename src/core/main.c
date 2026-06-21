/**
 * @file    main.c
 * @brief   Main entry point for the Mimic physics-agnostic galaxy evolution framework
 *
 * This file contains the main program flow for Mimic, handling initialization,
 * file processing, and the halo tracking loop. It coordinates the overall
 * execution of the framework, including:
 * - Parameter file reading and initialization
 * - Command-line argument processing
 * - Error handling setup
 * - Tree file loading and traversal
 * - Halo tracking through merger trees
 * - Output file generation
 *
 * Key functions:
 * - main(): Program entry point and core execution loop
 * - termination_handler(): Handles CPU time limit signals
 * - bye(): Performs cleanup on program exit
 */

#include <math.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef MPI
#include <mpi.h>
#endif

#include "config.h"
#include "proto.h"
#include "galaxy_pool.h"
#include "globals.h"
#include "tree/interface.h"
#include "tree/reader.h"
#include "run_log.h"
#include "progress.h"

#include "output/hdf5.h"
#include "output/python_example.h"
#include "output/util.h"
#include "version.h"
#include "io.h"
#include "generated/property_defs.h"

/* Module system (physics-agnostic) */
#include "module_registry.h"

#define MAX_PATH_BUF_SIZE (3 * MAX_STRING_LEN + 25)

/* Output paths of the partition currently being processed. Set before the
 * partition is claimed, cleared once it completes, and unlinked by bye() if the
 * program exits with a failure in between, so a crash never leaves partial
 * output files behind and never deletes completed ones. Binary output has one
 * path per requested snapshot; HDF5 output has one path per partition. */
static char current_output_paths[ABSOLUTEMAXSNAPS][MAX_PATH_BUF_SIZE + 1];
static int current_output_path_count = 0;
static int exitfail = 1; /* Flag indicating whether program exit was due to failure */

static struct sigaction saveaction_XCPU;  /* Saved signal action for SIGXCPU */
static volatile sig_atomic_t gotXCPU = 0; /* Flag indicating whether SIGXCPU was received */

/**
 * @brief   Signal handler for CPU time limit exceeded (SIGXCPU)
 *
 * @param   signum    Signal number that triggered the handler
 *
 * This function sets a flag when the CPU time limit is exceeded and
 * passes control to any previously registered handler if one exists.
 * This allows for graceful termination when running on systems with
 * CPU time limits (e.g., batch systems).
 */

void termination_handler(int signum) {
  gotXCPU = 1;
  /* Call the previous handler first while our handler is still active */
  if (saveaction_XCPU.sa_handler != NULL)
    (*saveaction_XCPU.sa_handler)(signum);
  /* Then restore the previous handler */
  sigaction(SIGXCPU, &saveaction_XCPU, NULL);
}

/**
 * @brief   Removes a command-line argument and shifts remaining arguments
 *
 * @param   argv    Argument vector
 * @param   argc    Pointer to argument count
 * @param   index   Index of argument to remove
 * @return  Adjusted index for loop continuation (index - 1)
 *
 * This helper function removes a command-line argument at the specified index
 * by shifting all subsequent arguments left, decrementing the argument count,
 * and returning the adjusted index for the calling loop.
 */
static int remove_arg(char **argv, int *argc, int index) {
  for (int k = index; k < *argc - 1; k++) {
    argv[k] = argv[k + 1];
  }
  (*argc)--;
  return index - 1;
}

/**
 * @brief   Exit handler for controlled program termination
 *
 * @param   signum    Exit code to be passed to the OS
 *
 * This function prints a termination message and exits the program.
 * Different messages are displayed in MPI versus serial mode.
 */

void myexit(int signum) {
#ifdef MPI
  printf("Task: %d\tnode: %s\tis exiting\n\n\n", ThisTask, ThisNode);
#else
  printf("We're exiting\n");
#endif
  exit(signum);
}

/**
 * @brief   Cleanup function registered with atexit()
 *
 * This function performs cleanup operations before the program terminates,
 * including MPI finalization in parallel mode and temporary file removal.
 * It is automatically called when the program exits.
 */

void bye() {
#ifdef MPI
  MPI_Finalize();
  free(ThisNode);
#endif

  if (exitfail) {
    /* Remove in-progress output files so a failed run leaves no partial output */
    for (int i = 0; i < current_output_path_count; i++) {
      if (current_output_paths[i][0] != '\0') {
        unlink(current_output_paths[i]);
      }
    }

#ifdef MPI
    if (ThisTask == 0 && gotXCPU == 1)
      printf("Received XCPU, exiting. But we'll be back.\n");
#endif
  }
}

static void clear_current_output_paths(void) {
  for (int i = 0; i < current_output_path_count; i++) {
    current_output_paths[i][0] = '\0';
  }
  current_output_path_count = 0;
}

static void set_current_output_paths(int output_id) {
  clear_current_output_paths();

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

/**
 * @brief   Extracts filename from a path
 *
 * @param   path        Full path to file
 * @return  Pointer to filename portion of path
 */
const char *get_filename_from_path(const char *path) {
  const char *filename = strrchr(path, '/');
  if (filename) {
    return filename + 1;
  }
  return path;
}

/**
 * @brief   Copy a referenced config file into the run metadata directory.
 *
 * Empty paths are skipped silently. A copy failure is logged as a warning
 * rather than aborting the run, so a provenance gap is visible without losing
 * the (already written) output.
 *
 * @param   metadata_dir  Destination metadata directory
 * @param   src_path      Source file path (may be empty)
 */
static void copy_to_metadata(const char *metadata_dir, const char *src_path) {
  char dest_path[MAX_STRING_LEN + 50]; // Extra space for "/" and filename

  if (src_path == NULL || src_path[0] == '\0') {
    return;
  }

  snprintf(dest_path, sizeof(dest_path), "%s/%s", metadata_dir, get_filename_from_path(src_path));
  if (copy_file(src_path, dest_path) != 0) {
    WARNING_LOG("Failed to copy '%s' to metadata directory '%s'", src_path, metadata_dir);
  }
}

/**
 * @brief   Write the exact output schema used by this executable.
 *
 * Binary output stores raw HaloOutput records, so downstream readers need the
 * field offsets and total record size from the compiler that built this run.
 * HDF5 output is self-describing, but the same schema file keeps run metadata
 * consistent across output formats.
 */
static void write_output_schema_metadata(const char *metadata_dir) {
  char schema_path[MAX_STRING_LEN + 64];
  snprintf(schema_path, sizeof(schema_path), "%s/output_schema.json", metadata_dir);

  FILE *schema_file = fopen(schema_path, "w");
  if (schema_file == NULL) {
    WARNING_LOG("Failed to create output schema metadata file: %s", schema_path);
    return;
  }

#include "../include/generated/output_schema_writer.inc"

  fclose(schema_file);
  VERBOSE_LOG("Output schema metadata saved to %s", schema_path);
}

/**
 * @brief   Main program entry point
 *
 * @param   argc      Number of command-line arguments
 * @param   argv      Array of command-line argument strings
 * @return  Exit status code (0 for success)
 *
 * This function implements the main program flow:
 * 1. Initialize MPI if compiled with MPI support
 * 2. Process command-line arguments
 * 3. Set up signal handling for CPU time limits
 * 4. Initialize the error handling system
 * 5. Read parameter file and initialize simulation
 * 6. Process merger tree files in parallel (MPI) or serially
 * 7. For each tree:
 *    a. Load the merger tree
 *    b. Construct objects by walking the tree
 *    c. Save the resulting objects
 * 8. Perform cleanup and exit
 */

/**
 * @brief   Parse command-line flags, leaving only the parameter file in argv
 *
 * Handles -h/--help (prints usage and exits), verbosity flags, --skip, and
 * --compress. Also installs the runtime defaults the flags may override.
 *
 * @param   argc  Pointer to argument count (updated as flags are consumed)
 * @param   argv  Argument vector (flags are removed in place)
 * @return  The selected log level
 */
static LogLevel parse_cli(int *argc, char **argv) {
  LogLevel log_level = LOG_LEVEL_INFO;

  /* Set default values */
  MimicConfig.OverwriteOutputFiles = 1;
  MimicConfig.HDF5CompressionLevel = 0; // Off by default; enabled via --compress
  MimicConfig.MaxTreeDepth = 500;       // Typical trees: 50-100 levels
  /* Consistent-Trees forest distribution defaults (only the ctrees readers use
     them): split forests uniformly; the exponent applies to the power schemes. */
  MimicConfig.ForestDistributionScheme = 0; // uniform_in_forests
  MimicConfig.Exponent_Forest_Dist_Scheme = 0.7;

  for (int i = 1; i < *argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      /* Initialize error handling early for proper message formatting */
      initialize_error_handling(log_level, NULL);

      /* Display help and exit */
      printf("\nMimic - Physics-Agnostic Galaxy Evolution Framework\n");
      printf("Usage: mimic [options] <parameterfile>\n\n");
      printf("Options:\n");
      printf("  -h, --help       Display this help message and exit\n");
      printf("  -v, --verbose    Add context (timestamp, file:line) to messages\n");
      printf("  -d, --debug      Enable debug output with context (most verbose)\n");
      printf("  -q, --quiet      Show only warnings and errors (least verbose)\n");
      printf("  --skip           Skip existing output files instead of "
             "overwriting\n");
      printf("  --compress       Compress HDF5 galaxy output with gzip "
             "(off by default)\n\n");
      exit(0);
    } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
      // Enable verbose formatting (adds timestamp, file:line context)
      set_verbose_format(1);
      i = remove_arg(argv, argc, i);
    } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--debug") == 0) {
      // Enable debug level logging with timestamp/file:line context prefix
      log_level = LOG_LEVEL_DEBUG;
      set_verbose_format(1);
      set_verbose_prefix(1);
      i = remove_arg(argv, argc, i);
    } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
      log_level = LOG_LEVEL_WARNING;
      i = remove_arg(argv, argc, i);
    } else if (strcmp(argv[i], "--skip") == 0) {
      MimicConfig.OverwriteOutputFiles = 0;
      i = remove_arg(argv, argc, i);
    } else if (strcmp(argv[i], "--compress") == 0) {
      /* On/off: HDF5's table API applies a fixed gzip level, so there is no
       * level to expose here. Any nonzero value enables compression. */
      MimicConfig.HDF5CompressionLevel = 1;
      i = remove_arg(argv, argc, i);
    }
  }

  return log_level;
}

/**
 * @brief   Process every unit of one partition and finalize its output
 *
 * Opens the partition, creates its output files, runs the depth-first tree
 * driver over every unit, writes the processed halos, and finalizes the
 * format-specific output. For the L-Halo readers a partition is one input file
 * (named by its output id) and a unit is one tree.
 */
static void process_partition(int output_id) {
  int unit, halonr;

  /* Open the partition and create its output files */
  FileNum = output_id;
  open_partition(output_id);
  prepare_output_files(output_id);

  /* Live progress bar over the trees in this file (falls back to periodic log
   * lines when output is redirected or running multi-rank under MPI). */
  ProgressBar progress;
  progress_bar_init(&progress, Ntrees, output_id);

  for (unit = 0; unit < Ntrees; unit++) {
    /* Stop cleanly if the CPU time limit signal was received (batch systems) */
    if (gotXCPU) {
      FATAL_ERROR("Received SIGXCPU (CPU time limit) — stopping before tree %d of file %d", unit,
                  output_id);
    }

    progress_bar_update(&progress, unit);

    /* Set the current unit ID and load the unit */
    TreeID = unit;
    load_unit(unit);

    /* Reset the per-unit output-buffer count */
    NumProcessedHalos = 0;

    /* Construct objects for each unprocessed halo in the unit */
    for (halonr = 0; halonr < InputTreeNHalos[unit]; halonr++)
      if (HaloAux[halonr].DoneFlag == 0)
        build_halo_tree(halonr, unit, output_id, 0);

    /* Save the processed halos (format depends on OutputFormat parameter) */
#ifdef HDF5
    if (MimicConfig.OutputFormat == output_hdf5) {
      save_halos_hdf5(output_id, unit);
    } else {
      save_halos(output_id, unit);
    }
#else
    save_halos(output_id, unit);
#endif
    free_unit_halos();
  }

  progress_bar_finish(&progress);

  /* Finalize output files (format depends on OutputFormat parameter) */
#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    /* Flush any buffered halos accumulated across units for this partition */
    flush_hdf5_buffers(output_id);

    /* Write metadata attributes for each output snapshot */
    for (int n = 0; n < MimicConfig.NOUT; n++) {
      write_hdf5_attrs(n, output_id);
    }

    /* Close the HDF5 file */
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
 * @brief   Claim and process one output partition, honoring --skip
 *
 * @param   output_id  Output id of the partition (filenr for per-file readers,
 *                      task id for per-task readers)
 * @return  1 if the partition was processed, 0 if it was skipped because its
 *          output already exists.
 *
 * A partial output set (some but not all of a partition's files exist) is fatal
 * under --skip: it usually marks an interrupted run that should not be silently
 * completed or overwritten in part.
 */
static int claim_and_process_partition(int output_id) {
  set_current_output_paths(output_id);
  int existing_outputs = count_existing_current_outputs();
  if (!MimicConfig.OverwriteOutputFiles) {
    if (existing_outputs == current_output_path_count) {
      INFO_LOG("Output for partition %d already exists ... skipping", output_id);
      clear_current_output_paths();
      return 0;
    }
    if (existing_outputs > 0) {
      FATAL_ERROR("Partial output exists for partition %d (%d of %d files). Remove the partial "
                  "files or rerun without --skip.",
                  output_id, existing_outputs, current_output_path_count);
    }
  }

  /* Create output files to mark that this partition is being processed */
  claim_current_output_paths(output_id);

  process_partition(output_id);

  /* This partition's output is complete; nothing to unlink on later failure */
  clear_current_output_paths();
  return 1;
}

/**
 * @brief   Run the existing tree-ordered processing driver.
 */
static void run_tree_driver(void) {
  FILE *fd;
  char tree_path[MAX_PATH_BUF_SIZE + 1];

  /* Main loop to process input partitions (one per input file for L-Halo) */
  log_phase_banner(PHASE_TREE_PROCESSING);
  /* Enable rate limiting for DEBUG_LOG during tree processing to prevent
   * runaway output from loops over thousands of trees/halos */
  enable_debug_log_rate_limiting();
  const struct TreeReader *reader = MimicConfig.reader;
  if (reader->partition_model == PARTITION_PER_TASK) {
    /* Each task owns exactly one output partition (its forest chunk); the reader
     * performs the per-task forest split inside open_partition keyed on the
     * ThisTask/NTask globals. There is no per-file stride and no per-file
     * existence check -- the ctrees index files are validated when opened. */
    const int output_id = ThisTask; /* 0 in serial builds */
    if (claim_and_process_partition(output_id) && !progress_display_active()) {
      /* In live mode the progress bar already shows completion in place. */
      INFO_LOG("%sCompleted task chunk %d%s", mimic_color_green(), output_id, mimic_color_reset());
    }
  } else {
    /* PARTITION_PER_FILE: one partition per input file, strided across tasks. */
    const int npartitions = reader->num_partitions();
#ifdef MPI
    /* In MPI mode, distribute partitions across processors using stride of NTask */
    for (int partition = ThisTask; partition < npartitions; partition += NTask)
#else
    /* In serial mode, process all partitions sequentially */
    for (int partition = 0; partition < npartitions; partition++)
#endif
    {
      const int output_id = reader->partition_output_id(partition);

      /* Construct tree filename and check if it exists */
      if (reader->format_partition_path != NULL) {
        reader->format_partition_path(tree_path, sizeof(tree_path), output_id);
      } else {
        snprintf(tree_path, MAX_PATH_BUF_SIZE, "%s/%s.%d%s", MimicConfig.SimulationDir,
                 MimicConfig.TreeName, output_id, MimicConfig.TreeExtension);
      }
      if (!(fd = fopen(tree_path, "r"))) {
        INFO_LOG("Missing tree %s ... skipping", tree_path);
        continue; // tree file does not exist, move along
      } else
        fclose(fd);

      /* Check if output already exists (avoid reprocessing unless overwrite is
       * set) and process this partition. */
      if (claim_and_process_partition(output_id) && !progress_display_active()) {
        /* In live mode the progress bar already shows completion in place. */
        INFO_LOG("%sCompleted file %d%s", mimic_color_green(), output_id, mimic_color_reset());
      }
    }
  }

  /* Disable rate limiting for DEBUG_LOG after tree processing completes */
  disable_debug_log_rate_limiting();
}

/**
 * @brief   Dispatch to the processing driver selected by input.processing_order.
 */
static void run_processing_driver(void) {
  switch ((enum InputProcessingOrder)MimicConfig.ProcessingOrder) {
  case INPUT_PROCESSING_ORDER_TREE:
    run_tree_driver();
    return;
  case INPUT_PROCESSING_ORDER_SNAPSHOT:
    FATAL_ERROR("The snapshot-ordered driver is not implemented yet");
  }

  FATAL_ERROR("Unknown input.processing_order '%s'",
              input_processing_order_name((enum InputProcessingOrder)MimicConfig.ProcessingOrder));
}

/**
 * @brief   Snapshot the run configuration and provenance next to the output
 *
 * Copies the run file and every referenced package file to the output metadata
 * directory so it is a self-contained, reproducible snapshot of the run. The
 * run YAML only references the model and simulation packages by path, so the
 * simulation config and snapshot list are captured here too. Property metadata
 * is represented by output_schema.json and HDF5 FieldMetadata; full source
 * provenance comes from version metadata plus the recorded git revision.
 */
static void write_run_metadata(const char *param_file) {
  char metadata_dir[MAX_STRING_LEN + 15]; // +15 for "/metadata" and null terminator

  // Create metadata directory if it doesn't exist
  snprintf(metadata_dir, sizeof(metadata_dir), "%s/metadata", MimicConfig.OutputDir);
  if (ensure_directory_exists(metadata_dir) != 0) {
    WARNING_LOG("Failed to create metadata directory '%s'", metadata_dir);
  }

  copy_to_metadata(metadata_dir, param_file);
  copy_to_metadata(metadata_dir, MimicConfig.FileWithSnapList);
  copy_to_metadata(metadata_dir, MimicConfig.SimulationConfigPath);
  /* PlottingProfilePath is intentionally not copied: the profile is only
   * needed by mimic-plot.py, which reads it from the run YAML directly. */
  write_output_schema_metadata(metadata_dir);
  write_python_example(MimicConfig.OutputDir);
  VERBOSE_LOG("Run configuration and referenced package files copied to %s", metadata_dir);

  // Create version metadata file
  if (create_version_metadata(MimicConfig.OutputDir, param_file) != 0) {
    WARNING_LOG("Failed to create version metadata file");
  }
}

int main(int argc, char **argv) {
  struct sigaction current_XCPU;

#ifdef MPI
  /* Initialize MPI environment */
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &ThisTask); /* Get this processor's task ID */
  MPI_Comm_size(MPI_COMM_WORLD, &NTask);    /* Get total number of processors */

  /* Get the name of this processor's node */
  ThisNode = malloc(MPI_MAX_PROCESSOR_NAME * sizeof(char));
  MPI_Get_processor_name(ThisNode, &nodeNameLen);
  if (nodeNameLen >= MPI_MAX_PROCESSOR_NAME) {
    FATAL_ERROR("MPI node name string too long (%d >= %d)", nodeNameLen, MPI_MAX_PROCESSOR_NAME);
  }
#endif

  /* Parse command-line flags (verbosity, --skip, --compress, help) */
  LogLevel log_level = parse_cli(&argc, argv);

  /* Ensure we have exactly one parameter file specified */
  if (argc != 2) {
    FATAL_ERROR("Incorrect usage! Please use: mimic [options] <parameterfile>\n"
                "For help, use: mimic --help");
  }

  /* Register exit handler for cleanup */
  atexit(bye);

  /* Set up signal handling for CPU time limits */
  sigaction(SIGXCPU, NULL, &saveaction_XCPU);
  current_XCPU = saveaction_XCPU;
  current_XCPU.sa_handler = termination_handler;
  sigaction(SIGXCPU, &current_XCPU, NULL);

  /* Configure color usage for run banners before any logging */
  extern int MimicLogUseColor;
  MimicLogUseColor = isatty(STDOUT_FILENO) ? 1 : 0;

  /* Print run header */
  log_run_header(argv[1], log_level);

  /* Initialize error handling system first to set log level */
  /* (must be done before any INFO_LOG/FATAL_ERROR calls) */
  initialize_error_handling(log_level, NULL);

  /* Now log the configuration phase banner */
  log_phase_banner(PHASE_CONFIG);

  /* Initialize memory management system (will log at correct level) */
  init_memory_system(0); /* Use default block limit */

  /* Prepare the per-tree galaxy storage pool (grows to the largest tree). */
  galaxy_pool_init(0);

  /* Log startup information */
  DEBUG_LOG("Starting Mimic with verbosity level: %s", get_log_level_name(log_level));
  VERBOSE_LOG("Mimic physics-agnostic galaxy evolution framework starting up");

  /* Log detailed command line arguments at debug level */
  DEBUG_LOG("Command line argument count: %d", argc);
  for (int j = 0; j < argc; j++) {
    DEBUG_LOG("Argument %d: %s", j, argv[j]);
  }

  /* Read parameter file and initialize simulation */
  read_parameter_file(argv[1]);
  init();
  VERBOSE_LOG("Simulation directory : %s", MimicConfig.SimulationDir);
  VERBOSE_LOG("Output directory     : %s", MimicConfig.OutputDir);
  VERBOSE_LOG("Tree file range      : %d .. %d", MimicConfig.FirstFile, MimicConfig.LastFile);
#ifdef HDF5
  VERBOSE_LOG("Output format        : %s",
              MimicConfig.OutputFormat == output_hdf5 ? "HDF5" : "Binary");
#else
  VERBOSE_LOG("Output format        : Binary");
#endif
  VERBOSE_LOG("Snapshots requested  : %d", MimicConfig.NOUT);
  if (!get_verbose_format()) {
    INFO_LOG("Mimic configuration completed");
  }

  if (ensure_directory_exists(MimicConfig.OutputDir) != 0) {
    FATAL_ERROR("Failed to create output directory '%s'", MimicConfig.OutputDir);
  }

  /* Register and initialize galaxy physics modules */
  log_phase_banner(PHASE_MODULE_PIPELINE);
  VERBOSE_LOG("Initializing galaxy physics module system");
  register_all_modules(); /* Physics-agnostic: core doesn't know which modules
                             exist */
  if (module_system_init() != 0) {
    ERROR_LOG("Module system initialization failed");
    myexit(1);
  }
  if (!get_verbose_format() && module_system_pipeline_count() > 0) {
    INFO_LOG("All modules initialised");
  }

  /* Initialize HDF5 output system if HDF5 format is selected */
#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    VERBOSE_LOG("Initializing HDF5 output system");
    calc_hdf5_props();
  }
#endif

  run_processing_driver();

  /* Final output phase banner and any format-specific aggregation */
#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    log_phase_banner(PHASE_OUTPUT);
    VERBOSE_LOG("Creating master HDF5 file");
    write_master_file();
    free_hdf5_ids();
  } else
#endif
  {
    log_phase_banner(PHASE_OUTPUT);
    VERBOSE_LOG("Finalizing binary output files");
  }

  /* Report memory usage before cleanup (verbose/debug only) */
  if (get_verbose_format()) {
    INFO_LOG("Memory usage at completion:");
    print_memory_brief();
  }

  /* Clean up allocated memory */

  /* Free Age array using its original (unoffset) allocation pointer */
  myfree(Age_base);

  /* Cleanup galaxy physics modules */
  VERBOSE_LOG("Cleaning up galaxy physics module system");
  module_system_cleanup();

  /* Release the run-persistent inheritance gather scratch buffer */
  free_tree_driver_scratch();

  /* Release the galaxy pool before the leak check so its chunks are accounted
   * for and not reported as leaks. */
  galaxy_pool_destroy();

  /* Check for memory leaks and clean up memory system */
  check_memory_leaks();
  cleanup_memory_system();

  /* Snapshot the run configuration and provenance next to the output */
  write_run_metadata(argv[1]);
  if (!get_verbose_format()) {
    INFO_LOG("Output written to %s/", MimicConfig.OutputDir);
  }

  /* Set exit status to success */
  log_phase_banner(PHASE_SHUTDOWN);
  INFO_LOG("Mimic completed successfully");
  exitfail = 0;
  return 0;
}
