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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef MPI
#include <mpi.h>
#endif

#include "config.h"
#include "galaxy_id.h"
#include "proto.h"
#include "galaxy_pool.h"
#include "globals.h"
#include "memory.h"
#include "core/tree_driver.h"
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

static int exitfail = 1; /* Flag indicating whether program exit was due to failure */

static struct sigaction saveaction_XCPU; /* Saved signal action for SIGXCPU */

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
  TreeDriverGotXCPU = 1;
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
    tree_driver_remove_incomplete_outputs();

#ifdef MPI
    if (ThisTask == 0 && TreeDriverGotXCPU == 1)
      printf("Received XCPU, exiting. But we'll be back.\n");
#endif
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
  MimicConfig.TargetFileSize = MIMIC_DEFAULT_TARGET_FILE_SIZE;
  MimicConfig.ForestsPerFile = MIMIC_DEFAULT_FORESTS_PER_FILE;
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

/**
 * @brief   Main program entry point: init → modules → processing driver → cleanup
 */
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
    INFO_LOG("All physics modules initialised");
  }

  /* Initialize HDF5 output system if HDF5 format is selected */
#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    VERBOSE_LOG("Initializing HDF5 output system");
    calc_hdf5_props();
  }
#endif

  run_processing_driver();

#ifdef MPI
  /* Every rank must finish and close its assigned output partitions before rank 0
     re-opens them to build the master file: HDF5 takes a file lock on open, so
     an unsynchronized master write races the owning ranks and fails with
     "unable to lock file" (errno 11) on locking filesystems. */
  MPI_Barrier(MPI_COMM_WORLD);
#endif

  /* Final output phase banner and any format-specific aggregation */
#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    log_phase_banner(PHASE_OUTPUT);
    /* The master file is a single shared file of links across every output
       partition, so only rank 0 writes it (after the barrier above). The
       per-rank field-metadata arrays are freed on all ranks. */
    if (ThisTask == 0) {
      VERBOSE_LOG("Creating master HDF5 file");
      write_master_file();
    }
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
