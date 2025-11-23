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

#include <assert.h>
#include <math.h>
#include <signal.h>
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
#include "globals.h"
#include "tree/interface.h"

#include "output/hdf5.h"
#include "version.h"
#include "io.h"

/* Module system (physics-agnostic) */
#include "module_registry.h"

#define MAX_BUFZ0_SIZE (3 * MAX_STRING_LEN + 25)
static char
    bufz0[MAX_BUFZ0_SIZE + 1]; /* 3 strings + max 19 bytes for a number */
static int exitfail =
    1; /* Flag indicating whether program exit was due to failure */

static struct sigaction saveaction_XCPU; /* Saved signal action for SIGXCPU */
static volatile sig_atomic_t gotXCPU =
    0; /* Flag indicating whether SIGXCPU was received */

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
  sigaction(SIGXCPU, &saveaction_XCPU, NULL);
  if (saveaction_XCPU.sa_handler != NULL)
    (*saveaction_XCPU.sa_handler)(signum);
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
  printf("We're exiting\n\n\n");
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
    unlink(bufz0); /* Remove temporary output file if we're exiting due to
                      failure */

#ifdef MPI
    if (ThisTask == 0 && gotXCPU == 1)
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

int main(int argc, char **argv) {
  int filenr, treenr, halonr;
  struct sigaction current_XCPU;

  struct stat filestatus;
  FILE *fd;

#ifdef MPI
  /* Initialize MPI environment */
  MPI_Init(&argc, &argv);
  MPI_Comm_rank(MPI_COMM_WORLD, &ThisTask); /* Get this processor's task ID */
  MPI_Comm_size(MPI_COMM_WORLD, &NTask);    /* Get total number of processors */

  /* Get the name of this processor's node */
  ThisNode = malloc(MPI_MAX_PROCESSOR_NAME * sizeof(char));
  MPI_Get_processor_name(ThisNode, &nodeNameLen);
  if (nodeNameLen >= MPI_MAX_PROCESSOR_NAME) {
    FATAL_ERROR("MPI node name string too long (%d >= %d)", nodeNameLen,
                MPI_MAX_PROCESSOR_NAME);
  }
#endif

  /* Set default logging level */
  LogLevel log_level = LOG_LEVEL_INFO;

  /* Set default values */
  MimicConfig.OverwriteOutputFiles = 1;
  MimicConfig.MaxTreeDepth = 500; // Typical trees: 50-100 levels

  /* Parse command-line arguments for special flags like help, verbosity */
  int i;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
      /* Initialize error handling early for proper message formatting */
      initialize_error_handling(log_level, NULL);

      /* Display help and exit */
      INFO_LOG("Mimic Help");
      printf("\nMimic - Physics-Agnostic Galaxy Evolution Framework\n");
      printf("Usage: mimic [options] <parameterfile>\n\n");
      printf("Options:\n");
      printf("  -h, --help       Display this help message and exit\n");
      printf("  -v, --verbose    Show debug messages (most verbose)\n");
      printf(
          "  -q, --quiet      Show only warnings and errors (least verbose)\n");
      printf("  --skip           Skip existing output files instead of "
             "overwriting\n\n");
      exit(0);
    } else if (strcmp(argv[i], "-v") == 0 ||
               strcmp(argv[i], "--verbose") == 0) {
      log_level = LOG_LEVEL_DEBUG;
      i = remove_arg(argv, &argc, i);
    } else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
      log_level = LOG_LEVEL_WARNING;
      i = remove_arg(argv, &argc, i);
    } else if (strcmp(argv[i], "--skip") == 0) {
      MimicConfig.OverwriteOutputFiles = 0;
      i = remove_arg(argv, &argc, i);
    }
  }

  /* Initialize error handling system with the log level determined from command
   * line (must be done before any FATAL_ERROR calls) */
  initialize_error_handling(log_level, NULL);

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

  /* Initialize memory management system */
  init_memory_system(0); /* Use default block limit */

  /* Log startup information */
  DEBUG_LOG("Starting Mimic with verbosity level: %s",
            get_log_level_name(log_level));
  INFO_LOG("Mimic physics-agnostic galaxy evolution framework starting up");

  /* Log detailed command line arguments at debug level */
  DEBUG_LOG("Command line argument count: %d", argc);
  int j;
  for (j = 0; j < argc; j++) {
    DEBUG_LOG("Argument %d: %s", j, argv[j]);
  }

  /* Read parameter file and initialize simulation */
  read_parameter_file(argv[1]);
  init();

  /* Register and initialize galaxy physics modules */
  INFO_LOG("Initializing galaxy physics module system");
  register_all_modules(); /* Physics-agnostic: core doesn't know which modules
                             exist */
  if (module_system_init() != 0) {
    ERROR_LOG("Module system initialization failed");
    myexit(1);
  }

  /* Initialize HDF5 output system if HDF5 format is selected */
#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    INFO_LOG("Initializing HDF5 output system");
    calc_hdf5_props();
  }
#endif

  /* Main loop to process merger tree files */
#ifdef MPI
  /* In MPI mode, distribute files across processors using stride of NTask */
  for (filenr = MimicConfig.FirstFile + ThisTask; filenr <= MimicConfig.LastFile;
       filenr += NTask)
#else
  /* In serial mode, process all files sequentially */
  for (filenr = MimicConfig.FirstFile; filenr <= MimicConfig.LastFile; filenr++)
#endif
  {
    /* Construct tree filename and check if it exists */
    snprintf(bufz0, MAX_BUFZ0_SIZE, "%s/%s.%d%s", MimicConfig.SimulationDir,
             MimicConfig.TreeName, filenr, MimicConfig.TreeExtension);
    if (!(fd = fopen(bufz0, "r"))) {
      INFO_LOG("Missing tree %s ... skipping", bufz0);
      continue; // tree file does not exist, move along
    } else
      fclose(fd);

    /* Check if output file already exists (to avoid reprocessing unless
     * overwrite flag is set) */
#ifdef HDF5
    if (MimicConfig.OutputFormat == output_hdf5) {
      /* HDF5 format: one file per filenr (e.g., model_000.hdf5) */
      snprintf(bufz0, MAX_BUFZ0_SIZE, "%s/%s_%03d.hdf5", MimicConfig.OutputDir,
               MimicConfig.OutputFileBaseName, filenr);
    } else {
      /* Binary format: one file per snapshot per filenr (e.g., model_z0.000_0) */
      snprintf(bufz0, MAX_BUFZ0_SIZE, "%s/%s_z%1.3f_%d", MimicConfig.OutputDir,
               MimicConfig.OutputFileBaseName, MimicConfig.ZZ[MimicConfig.ListOutputSnaps[0]], filenr);
    }
#else
    /* Binary format only (no HDF5 support compiled in) */
    snprintf(bufz0, MAX_BUFZ0_SIZE, "%s/%s_z%1.3f_%d", MimicConfig.OutputDir,
             MimicConfig.OutputFileBaseName, MimicConfig.ZZ[MimicConfig.ListOutputSnaps[0]], filenr);
#endif
    if (stat(bufz0, &filestatus) == 0 && !MimicConfig.OverwriteOutputFiles) {
      INFO_LOG("Output for tree %s already exists ... skipping", bufz0);
      continue; // output seems to already exist, dont overwrite, move along
    }

    /* Create output file to mark that we're processing this tree */
    if ((fd = fopen(bufz0, "w")))
      fclose(fd);

    /* Load the tree table and process each tree */
    FileNum = filenr;
    load_tree_table(filenr, MimicConfig.TreeType);

    for (treenr = 0; treenr < Ntrees; treenr++) {
      /* Check if we've received a CPU time limit signal */
      assert(!gotXCPU);

      /* Log progress periodically */
      if (treenr % TREE_PROGRESS_INTERVAL == 0) {
#ifdef MPI
        INFO_LOG("Processing task: %d node: %s file: %i tree: %i of %i",
                 ThisTask, ThisNode, filenr, treenr, Ntrees);
#else
        INFO_LOG("Processing file: %i tree: %i of %i", filenr, treenr, Ntrees);
#endif
      }

      /* Set the current tree ID and load the tree */
      TreeID = treenr;
      load_tree(treenr, MimicConfig.TreeType);

      /* Random seed setting removed - not actually used in computation */

      /* Reset halo counters */
      NumProcessedHalos = 0;
      HaloCounter = 0;

      /* Construct objects for each unprocessed halo in the tree */
      for (halonr = 0; halonr < InputTreeNHalos[treenr]; halonr++)
        if (HaloAux[halonr].DoneFlag == 0)
          build_halo_tree(halonr, treenr, 0);

      /* Save the processed halos (format depends on OutputFormat parameter) */
#ifdef HDF5
      if (MimicConfig.OutputFormat == output_hdf5) {
        save_halos_hdf5(filenr, treenr);
      } else {
        save_halos(filenr, treenr);
      }
#else
      save_halos(filenr, treenr);
#endif
      free_halos_and_tree();
    }

    /* Finalize output files (format depends on OutputFormat parameter) */
#ifdef HDF5
    if (MimicConfig.OutputFormat == output_hdf5) {
      /* Write metadata attributes for each output snapshot */
      int n;
      for (n = 0; n < MimicConfig.NOUT; n++) {
        write_hdf5_attrs(n, filenr);
      }

      /* Close the HDF5 file */
      if (HDF5_current_file_id >= 0) {
        DEBUG_LOG("Closing HDF5 file (ID %lld) for filenr %d",
                  (long long)HDF5_current_file_id, filenr);
        H5Fclose(HDF5_current_file_id);
        HDF5_current_file_id = -1;
      }
    } else {
      finalize_halo_file(filenr);
    }
#else
    finalize_halo_file(filenr);
#endif
    free_tree_table(MimicConfig.TreeType);

    INFO_LOG("Completed processing file %d", filenr);
  }

  /* Create master HDF5 file and free HDF5 resources if using HDF5 output */
#ifdef HDF5
  if (MimicConfig.OutputFormat == output_hdf5) {
    INFO_LOG("Creating master HDF5 file");
    write_master_file();
    free_hdf5_ids();
  }
#endif

  /* Clean up allocated memory */

  /* Free Age array using original allocation pointer (fix for issue 1.2.1) */
  myfree(Age_base);

  /* Random generator freeing removed - not actually used in computation */

  /* Cleanup galaxy physics modules */
  INFO_LOG("Cleaning up galaxy physics module system");
  module_system_cleanup();

  /* Check for memory leaks and clean up memory system */
  check_memory_leaks();
  cleanup_memory_system();

  /* Copy parameter file and snapshot list file to output metadata directory */
  char metadata_dir[MAX_STRING_LEN +
                    15]; // +15 for "/metadata" and null terminator
  char source_path[MAX_STRING_LEN];
  char dest_path[MAX_STRING_LEN + 50]; // Extra space for filenames

  // Create metadata directory if it doesn't exist
  snprintf(metadata_dir, sizeof(metadata_dir), "%s/metadata",
           MimicConfig.OutputDir);
  mkdir(metadata_dir, 0777);

  // Copy parameter file
  snprintf(source_path, sizeof(source_path), "%s", argv[1]);
  snprintf(dest_path, sizeof(dest_path), "%s/%s", metadata_dir,
           get_filename_from_path(argv[1]));
  if (copy_file(source_path, dest_path) == 0) {
    // Copy snapshot list file
    snprintf(source_path, sizeof(source_path), "%s",
             MimicConfig.FileWithSnapList);
    snprintf(dest_path, sizeof(dest_path), "%s/%s", metadata_dir,
             get_filename_from_path(MimicConfig.FileWithSnapList));
    if (copy_file(source_path, dest_path) == 0) {
      INFO_LOG("Parameter file and snapshot list copied to %s", metadata_dir);
    }
  }

  // Create version metadata file
  if (create_version_metadata(MimicConfig.OutputDir, argv[1]) != 0) {
    WARNING_LOG("Failed to create version metadata file");
  }

  /* Set exit status to success */
  exitfail = 0;
  return 0;
}
