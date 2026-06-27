/**
 * @file    test_parameter_parsing.c
 * @brief   Unit tests for parameter file parsing
 *
 * Validates: Configuration file parsing and parameter validation
 *
 * This test validates that Mimic's parameter file parser correctly:
 * - Reads parameter files without errors
 * - Extracts parameter values correctly
 * - Handles comments and whitespace
 * - Handles YAML snapshot lists
 * - Populates MimicConfig structure correctly
 */

#include "../../src/include/proto.h"
#include "../../src/include/types.h"
#include "../../src/io/tree/reader.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"
#include "../framework/test_framework.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;
static const char *TestExecutablePath = NULL;

/* Shared core-test fixtures (config reset, registration, generated run file path) */
#include "../framework/core_test_fixtures.h"

/**
 * @brief   Setup function for test initialization
 *
 * Initializes memory system and error handling with warning level.
 */
static void setup_test(void) {
  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);
}

static void install_output_chunking_defaults_for_test(void) {
  MimicConfig.TargetFileSize = MIMIC_DEFAULT_TARGET_FILE_SIZE;
  MimicConfig.ForestsPerFile = MIMIC_DEFAULT_FORESTS_PER_FILE;
}

/**
 * @brief   Teardown function for test cleanup
 *
 * Checks for memory leaks after test execution.
 */
static void teardown_test(void) { check_memory_leaks(); }

static int is_input_section_header(const char *line) {
  const char *cursor = line;
  while (*cursor == ' ' || *cursor == '\t') {
    cursor++;
  }
  if (strncmp(cursor, "input:", strlen("input:")) != 0) {
    return 0;
  }
  cursor += strlen("input:");
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
    cursor++;
  }
  return *cursor == '\0';
}

static int is_output_section_header(const char *line) {
  const char *cursor = line;
  while (*cursor == ' ' || *cursor == '\t') {
    cursor++;
  }
  if (strncmp(cursor, "output:", strlen("output:")) != 0) {
    return 0;
  }
  cursor += strlen("output:");
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
    cursor++;
  }
  return *cursor == '\0';
}

/**
 * @brief   Write a fixture that injects extra content immediately under the modules: header.
 *
 * Copies the canonical test YAML line-by-line; after the bare "modules:" header,
 * appends extra_content (caller must include newlines and correct YAML indentation).
 * Used to produce fixtures with explicit null/tilde phase values without duplicating the
 * full config.
 */
static int write_null_phase_fixture(char *path, size_t path_size, const char *label,
                                    const char *extra_content) {
  FILE *src;
  FILE *dst;
  char line[1024];
  int injected = 0;

  if (mkdir("archive", 0777) != 0 && errno != EEXIST) {
    return -1;
  }
  if (mkdir("archive/test-fixtures", 0777) != 0 && errno != EEXIST) {
    return -1;
  }

  snprintf(path, path_size, "archive/test-fixtures/test_null_phase_%s.yaml", label);
  src = fopen(test_binary_param_file(), "r");
  if (src == NULL) {
    return -1;
  }
  dst = fopen(path, "w");
  if (dst == NULL) {
    fclose(src);
    return -1;
  }

  while (fgets(line, sizeof(line), src) != NULL) {
    fputs(line, dst);
    if (!injected && strncmp(line, "modules:", 8) == 0 &&
        (line[8] == '\n' || line[8] == '\r' || line[8] == '\0')) {
      fputs(extra_content, dst);
      injected = 1;
    }
  }

  fclose(dst);
  fclose(src);
  return injected ? 0 : -1;
}

static int write_processing_order_fixture(char *path, size_t path_size,
                                          const char *processing_order) {
  FILE *src;
  FILE *dst;
  char line[1024];
  int wrote_processing_order = 0;

  if (mkdir("archive", 0777) != 0 && errno != EEXIST) {
    return -1;
  }
  if (mkdir("archive/test-fixtures", 0777) != 0 && errno != EEXIST) {
    return -1;
  }

  snprintf(path, path_size, "archive/test-fixtures/test_processing_order_%s.yaml",
           processing_order);
  src = fopen(test_binary_param_file(), "r");
  if (src == NULL) {
    return -1;
  }
  dst = fopen(path, "w");
  if (dst == NULL) {
    fclose(src);
    return -1;
  }

  while (fgets(line, sizeof(line), src) != NULL) {
    fputs(line, dst);
    if (!wrote_processing_order && is_input_section_header(line)) {
      fprintf(dst, "  processing_order: %s\n", processing_order);
      wrote_processing_order = 1;
    }
  }
  if (!wrote_processing_order) {
    fprintf(dst, "\ninput:\n  processing_order: %s\n", processing_order);
    wrote_processing_order = 1;
  }

  fclose(dst);
  fclose(src);
  return wrote_processing_order ? 0 : -1;
}

static int write_output_chunking_fixture(char *path, size_t path_size, const char *label,
                                         const char *extra_content) {
  FILE *src;
  FILE *dst;
  char line[1024];
  int injected = 0;

  if (mkdir("archive", 0777) != 0 && errno != EEXIST) {
    return -1;
  }
  if (mkdir("archive/test-fixtures", 0777) != 0 && errno != EEXIST) {
    return -1;
  }

  snprintf(path, path_size, "archive/test-fixtures/test_output_chunking_%s.yaml", label);
  src = fopen(test_binary_param_file(), "r");
  if (src == NULL) {
    return -1;
  }
  dst = fopen(path, "w");
  if (dst == NULL) {
    fclose(src);
    return -1;
  }

  while (fgets(line, sizeof(line), src) != NULL) {
    fputs(line, dst);
    if (!injected && is_output_section_header(line)) {
      fputs(extra_content, dst);
      injected = 1;
    }
  }

  fclose(dst);
  fclose(src);
  return injected ? 0 : -1;
}

static int write_simulation_output_fixture(char *run_path, size_t run_path_size, const char *label,
                                           int include_run_override) {
  FILE *src;
  FILE *dst;
  char line[1024];
  char sim_path[MAX_STRING_LEN];
  int replaced_sim_config = 0;
  int injected_run_override = 0;

  if (mkdir("archive", 0777) != 0 && errno != EEXIST) {
    return -1;
  }
  if (mkdir("archive/test-fixtures", 0777) != 0 && errno != EEXIST) {
    return -1;
  }

  snprintf(sim_path, sizeof(sim_path), "archive/test-fixtures/test_simulation_output_%s.yaml",
           label);
  dst = fopen(sim_path, "w");
  if (dst == NULL) {
    return -1;
  }
  fprintf(dst, "input:\n"
               "  first_file: 0\n"
               "  last_file: 0\n"
               "  tree_name: trees_063\n"
               "  tree_type: lhalo_binary\n"
               "  simulation_dir: simulations/mini-millennium/snapshots\n"
               "  snapshot_list_file: simulations/mini-millennium/mini-millennium.a_list\n"
               "\n"
               "output:\n"
               "  target_file_size_mb: 4096\n"
               "  forests_per_file: 2468\n"
               "\n"
               "simulation:\n"
               "  cosmology:\n"
               "    omega_matter: 0.25\n"
               "    omega_lambda: 0.75\n"
               "    hubble_h: 0.73\n"
               "  box_size:\n"
               "    value: 62.5\n"
               "    units: Mpc/h\n"
               "    h_convention: carried\n"
               "  particle_mass:\n"
               "    value: 0.0860657\n"
               "    units: 1e10 Msun/h\n"
               "    h_convention: carried\n");
  fclose(dst);

  snprintf(run_path, run_path_size, "archive/test-fixtures/test_run_simulation_output_%s.yaml",
           label);
  src = fopen(test_binary_param_file(), "r");
  if (src == NULL) {
    return -1;
  }
  dst = fopen(run_path, "w");
  if (dst == NULL) {
    fclose(src);
    return -1;
  }

  while (fgets(line, sizeof(line), src) != NULL) {
    if (strncmp(line, "  config:", strlen("  config:")) == 0) {
      fprintf(dst, "  config: %s\n", sim_path);
      replaced_sim_config = 1;
    } else {
      fputs(line, dst);
    }

    if (include_run_override && !injected_run_override && is_output_section_header(line)) {
      fputs("  target_file_size_mb: 8192\n"
            "  forests_per_file: 1357\n",
            dst);
      injected_run_override = 1;
    }
  }

  fclose(dst);
  fclose(src);

  if (!replaced_sim_config) {
    return -1;
  }
  if (include_run_override && !injected_run_override) {
    return -1;
  }
  return 0;
}

static int read_parameter_file_should_fatal(const char *path) {
  fflush(NULL);

  pid_t pid = fork();
  int status;

  if (pid < 0) {
    return -1;
  }

  if (pid == 0) {
    freopen("/dev/null", "w", stdout);
    freopen("/dev/null", "w", stderr);
    execl(TestExecutablePath, TestExecutablePath, "--expect-fatal", path, (char *)NULL);
    _exit(127);
  }

  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }

  if (WIFSIGNALED(status)) {
    return -1;
  }

  if (WIFEXITED(status) && WEXITSTATUS(status) == 127) {
    return -1;
  }

  return WIFEXITED(status) && WEXITSTATUS(status) != 0;
}

/**
 * @test    test_basic_parsing
 * @brief   Test that parameter file can be parsed without errors
 *
 * Expected: test_binary.yaml loads successfully
 * Validates: Parser can read and process parameter file
 */
int test_basic_parsing(void) {
  /* ===== SETUP ===== */
  setup_test();

  /* ===== EXECUTE ===== */
  read_parameter_file(test_binary_param_file());

  /* ===== VALIDATE ===== */
  printf("  ✓ Parameter file parsed successfully\n");

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_default_processing_order
 * @brief   Test that omitted input.processing_order defaults to tree_ordered
 */
int test_default_processing_order(void) {
  /* ===== SETUP ===== */
  setup_test();

  /* ===== EXECUTE ===== */
  read_parameter_file(test_binary_param_file());

  /* ===== VALIDATE ===== */
  TEST_ASSERT(MimicConfig.ProcessingOrder == INPUT_PROCESSING_ORDER_TREE,
              "Default processing_order should be tree_ordered");

  printf("  processing_order: %s\n",
         input_processing_order_name((enum InputProcessingOrder)MimicConfig.ProcessingOrder));

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_explicit_tree_ordered_processing_order
 * @brief   Test that explicit input.processing_order=tree_ordered parses successfully
 */
int test_explicit_tree_ordered_processing_order(void) {
  char fixture_path[MAX_STRING_LEN];

  /* ===== SETUP ===== */
  setup_test();

  TEST_ASSERT(write_processing_order_fixture(fixture_path, sizeof(fixture_path), "tree_ordered") ==
                  0,
              "Should create explicit tree_ordered fixture");

  /* ===== EXECUTE ===== */
  read_parameter_file(fixture_path);

  /* ===== VALIDATE ===== */
  TEST_ASSERT(MimicConfig.ProcessingOrder == INPUT_PROCESSING_ORDER_TREE,
              "Explicit processing_order should be tree_ordered");

  printf("  explicit processing_order: %s\n",
         input_processing_order_name((enum InputProcessingOrder)MimicConfig.ProcessingOrder));

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_integer_parameters
 * @brief   Test that integer parameters are read correctly
 *
 * Expected: File range is valid and NumOutputs=1
 * Validates: Integer parameter parsing
 */
int test_integer_parameters(void) {
  /* ===== SETUP ===== */
  setup_test();

  /* ===== EXECUTE ===== */
  read_parameter_file(test_binary_param_file());

  /* ===== VALIDATE ===== */
  TEST_ASSERT(MimicConfig.FirstFile == 0, "FirstFile should be 0");
  TEST_ASSERT(MimicConfig.LastFile >= MimicConfig.FirstFile, "LastFile should be >= FirstFile");
  TEST_ASSERT(MimicConfig.NOUT == 1, "NumOutputs should be 1");
  TEST_ASSERT(MimicConfig.LastSnapshotNr > 0, "LastSnapshotNr should be positive");

  printf("  FirstFile: %d\n", MimicConfig.FirstFile);
  printf("  LastFile: %d\n", MimicConfig.LastFile);
  printf("  NumOutputs: %d\n", MimicConfig.NOUT);
  printf("  LastSnapshotNr: %d\n", MimicConfig.LastSnapshotNr);

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_float_parameters
 * @brief   Test that float parameters are read correctly
 *
 * Expected: Parsed simulation-scale values are positive and finite.
 * Validates: Float parameter parsing
 */
int test_float_parameters(void) {
  /* ===== SETUP ===== */
  setup_test();

  /* ===== EXECUTE ===== */
  read_parameter_file(test_binary_param_file());

  /* ===== VALIDATE ===== */
  TEST_ASSERT(MimicConfig.BoxSize > 0.0, "BoxSize should be positive");
  TEST_ASSERT(MimicConfig.PartMass > 0.0, "PartMass should be positive");

  printf("  BoxSize: %.2f Mpc/h\n", MimicConfig.BoxSize);
  printf("  PartMass: %.7f\n", MimicConfig.PartMass);

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_string_parameters
 * @brief   Test that string parameters are read correctly
 *
 * Expected: OutputDir, TreeName, etc. correctly parsed
 * Validates: String parameter parsing
 */
int test_string_parameters(void) {
  /* ===== SETUP ===== */
  setup_test();

  /* ===== EXECUTE ===== */
  read_parameter_file(test_binary_param_file());

  /* ===== VALIDATE ===== */
  TEST_ASSERT_STRING_EQUAL(MimicConfig.OutputFileBaseName, "model",
                           "OutputFileBaseName should be 'model'");
  TEST_ASSERT(strlen(MimicConfig.TreeName) > 0, "TreeName should be populated");

  /* Check that OutputDir contains expected path */
  TEST_ASSERT(strstr(MimicConfig.OutputDir, "test") != NULL, "OutputDir should contain 'test'");

  printf("  OutputFileBaseName: %s\n", MimicConfig.OutputFileBaseName);
  printf("  TreeName: %s\n", MimicConfig.TreeName);
  printf("  OutputDir: %s\n", MimicConfig.OutputDir);
  printf("  SimulationDir: %s\n", MimicConfig.SimulationDir);

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_cosmology_parameters
 * @brief   Test that cosmological parameters are read correctly
 *
 * Expected: Cosmological parameters are positive and finite.
 * Validates: Cosmological parameter parsing
 */
int test_cosmology_parameters(void) {
  /* ===== SETUP ===== */
  setup_test();

  /* ===== EXECUTE ===== */
  read_parameter_file(test_binary_param_file());

  /* ===== VALIDATE ===== */
  TEST_ASSERT(MimicConfig.Omega > 0.0 && MimicConfig.Omega < 1.0,
              "Omega should be in a physical open interval");
  TEST_ASSERT(MimicConfig.OmegaLambda > 0.0 && MimicConfig.OmegaLambda < 1.0,
              "OmegaLambda should be in a physical open interval");
  TEST_ASSERT(MimicConfig.Hubble_h > 0.0, "Hubble_h should be positive");

  printf("  Omega: %.3f\n", MimicConfig.Omega);
  printf("  OmegaLambda: %.3f\n", MimicConfig.OmegaLambda);
  printf("  Hubble_h: %.3f\n", MimicConfig.Hubble_h);

  /* Sanity check: Omega + OmegaLambda should be ~1.0 for flat universe */
  double omega_total = MimicConfig.Omega + MimicConfig.OmegaLambda;
  TEST_ASSERT_DOUBLE_EQUAL(omega_total, 1.0, 0.01,
                           "Omega + OmegaLambda should be ~1.0 (flat universe)");

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_snapshot_list
 * @brief   Test that snapshot list is parsed correctly
 *
 * Expected: Final snapshot in output list
 * Validates: YAML snapshot list parsing
 */
int test_snapshot_list(void) {
  /* ===== SETUP ===== */
  setup_test();

  /* ===== EXECUTE ===== */
  read_parameter_file(test_binary_param_file());

  /* ===== VALIDATE ===== */
  TEST_ASSERT(MimicConfig.NOUT == 1, "Should have 1 output snapshot");
  TEST_ASSERT(MimicConfig.ListOutputSnaps[0] == MimicConfig.LastSnapshotNr,
              "First output snapshot should be the simulation's final snapshot");

  printf("  Number of output snapshots: %d\n", MimicConfig.NOUT);
  printf("  Output snapshot list:");
  for (int i = 0; i < MimicConfig.NOUT; i++) {
    printf(" %d", MimicConfig.ListOutputSnaps[i]);
  }
  printf("\n");

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_output_chunking_defaults
 * @brief   Default chunking parameters are positive after parsing the canonical run file
 */
int test_output_chunking_defaults(void) {
  /* ===== SETUP ===== */
  setup_test();
  install_output_chunking_defaults_for_test();

  /* ===== EXECUTE ===== */
  read_parameter_file(test_binary_param_file());

  /* ===== VALIDATE ===== */
  TEST_ASSERT(MimicConfig.TargetFileSize > 0, "Effective target_file_size should be positive");
  TEST_ASSERT(MimicConfig.ForestsPerFile >= 0, "Effective forests_per_file should be non-negative");
  if (MimicConfig.reader != NULL &&
      strcmp(MimicConfig.reader->name, "consistent_trees_ascii") == 0) {
    TEST_ASSERT(MimicConfig.ForestsPerFile > 0,
                "ASCII tests should inherit a positive forests_per_file default");
  } else if (MimicConfig.ForestsPerFile == MIMIC_DEFAULT_FORESTS_PER_FILE) {
    TEST_ASSERT_EQUAL(MimicConfig.TargetFileSize, MIMIC_DEFAULT_TARGET_FILE_SIZE,
                      "Global target_file_size fallback should be 4 GiB");
  }

  printf("  effective target_file_size default: %lld\n", (long long)MimicConfig.TargetFileSize);
  printf("  effective forests_per_file default: %lld\n", (long long)MimicConfig.ForestsPerFile);

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_output_chunking_parameters
 * @brief   target_file_size_mb converts to bytes and forests_per_file parses as int64
 */
int test_output_chunking_parameters(void) {
  char fixture_path[MAX_STRING_LEN];

  /* ===== SETUP ===== */
  setup_test();
  install_output_chunking_defaults_for_test();

  TEST_ASSERT(write_output_chunking_fixture(fixture_path, sizeof(fixture_path), "valid",
                                            "  target_file_size_mb: 5120\n"
                                            "  forests_per_file: 12345\n") == 0,
              "Should create output chunking fixture");

  /* ===== EXECUTE ===== */
  read_parameter_file(fixture_path);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(MimicConfig.TargetFileSize, 5120LL * 1024 * 1024,
                    "target_file_size_mb should convert MB to bytes");
  TEST_ASSERT_EQUAL(MimicConfig.ForestsPerFile, 12345LL, "forests_per_file should parse as int64");

  printf("  target_file_size parsed: %lld\n", (long long)MimicConfig.TargetFileSize);
  printf("  forests_per_file parsed: %lld\n", (long long)MimicConfig.ForestsPerFile);

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_output_chunking_accepts_zero_forests
 * @brief   forests_per_file: 0 is accepted and stored (size budget takes over)
 */
int test_output_chunking_accepts_zero_forests(void) {
  char fixture_path[MAX_STRING_LEN];

  /* ===== SETUP ===== */
  setup_test();
  install_output_chunking_defaults_for_test();

  TEST_ASSERT(write_output_chunking_fixture(fixture_path, sizeof(fixture_path), "zero_forests",
                                            "  target_file_size_mb: 4096\n"
                                            "  forests_per_file: 0\n") == 0,
              "Should create zero forests_per_file fixture");

  /* ===== EXECUTE ===== */
  read_parameter_file(fixture_path);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(MimicConfig.TargetFileSize, 4096LL * 1024 * 1024,
                    "target_file_size_mb should parse with forests_per_file zero");
  TEST_ASSERT_EQUAL(MimicConfig.ForestsPerFile, 0LL, "forests_per_file should accept zero");

  printf("  target_file_size parsed: %lld\n", (long long)MimicConfig.TargetFileSize);
  printf("  forests_per_file parsed: %lld\n", (long long)MimicConfig.ForestsPerFile);

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_simulation_output_chunking_defaults_and_run_override
 * @brief   Simulation output defaults are applied; run-level output section overrides them
 */
int test_simulation_output_chunking_defaults_and_run_override(void) {
  char simulation_default_path[MAX_STRING_LEN];
  char run_override_path[MAX_STRING_LEN];

  /* ===== SETUP ===== */
  setup_test();
  install_output_chunking_defaults_for_test();

  TEST_ASSERT(write_simulation_output_fixture(simulation_default_path,
                                              sizeof(simulation_default_path), "default", 0) == 0,
              "Should create simulation output default fixture");

  /* ===== EXECUTE ===== */
  read_parameter_file(simulation_default_path);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(MimicConfig.TargetFileSize, 4096LL * 1024 * 1024,
                    "simulation output.target_file_size_mb should set the default");
  TEST_ASSERT_EQUAL(MimicConfig.ForestsPerFile, 2468LL,
                    "simulation output.forests_per_file should set the default");

  printf("  simulation target_file_size default: %lld\n", (long long)MimicConfig.TargetFileSize);
  printf("  simulation forests_per_file default: %lld\n", (long long)MimicConfig.ForestsPerFile);

  /* ===== CLEANUP ===== */
  teardown_test();

  /* ===== SETUP ===== */
  setup_test();
  install_output_chunking_defaults_for_test();

  TEST_ASSERT(write_simulation_output_fixture(run_override_path, sizeof(run_override_path),
                                              "override", 1) == 0,
              "Should create run output override fixture");

  /* ===== EXECUTE ===== */
  read_parameter_file(run_override_path);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(MimicConfig.TargetFileSize, 8192LL * 1024 * 1024,
                    "run output.target_file_size_mb should override the simulation default");
  TEST_ASSERT_EQUAL(MimicConfig.ForestsPerFile, 1357LL,
                    "run output.forests_per_file should override the simulation default");

  printf("  run target_file_size override: %lld\n", (long long)MimicConfig.TargetFileSize);
  printf("  run forests_per_file override: %lld\n", (long long)MimicConfig.ForestsPerFile);

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_output_chunking_rejects_bad_values
 * @brief   Negative, zero-target, and non-numeric chunking parameters trigger a fatal
 */
int test_output_chunking_rejects_bad_values(void) {
  char negative_path[MAX_STRING_LEN];
  char zero_target_path[MAX_STRING_LEN];
  char negative_forests_path[MAX_STRING_LEN];
  char garbage_target_path[MAX_STRING_LEN];
  char garbage_forests_path[MAX_STRING_LEN];

  /* ===== SETUP ===== */
  setup_test();
  install_output_chunking_defaults_for_test();

  TEST_ASSERT(write_output_chunking_fixture(negative_path, sizeof(negative_path), "negative",
                                            "  target_file_size_mb: -1\n") == 0,
              "Should create negative target_file_size_mb fixture");
  TEST_ASSERT(write_output_chunking_fixture(zero_target_path, sizeof(zero_target_path), "zero",
                                            "  target_file_size_mb: 0\n") == 0,
              "Should create zero target_file_size_mb fixture");
  TEST_ASSERT(write_output_chunking_fixture(negative_forests_path, sizeof(negative_forests_path),
                                            "negative_forests", "  forests_per_file: -1\n") == 0,
              "Should create negative forests_per_file fixture");
  TEST_ASSERT(write_output_chunking_fixture(garbage_target_path, sizeof(garbage_target_path),
                                            "garbage_target", "  target_file_size_mb: 4x\n") == 0,
              "Should create garbage target_file_size_mb fixture");
  TEST_ASSERT(write_output_chunking_fixture(garbage_forests_path, sizeof(garbage_forests_path),
                                            "garbage_forests", "  forests_per_file: 12x\n") == 0,
              "Should create garbage forests_per_file fixture");

  /* ===== EXECUTE / VALIDATE ===== */
  int negative_target_result = read_parameter_file_should_fatal(negative_path);
  TEST_ASSERT(negative_target_result != -1, "Negative target_file_size_mb child should not crash");
  TEST_ASSERT(negative_target_result == 1, "Negative target_file_size_mb should fatal");

  int zero_target_result = read_parameter_file_should_fatal(zero_target_path);
  TEST_ASSERT(zero_target_result != -1, "Zero target_file_size_mb child should not crash");
  TEST_ASSERT(zero_target_result == 1, "Zero target_file_size_mb should fatal");

  int negative_forests_result = read_parameter_file_should_fatal(negative_forests_path);
  TEST_ASSERT(negative_forests_result != -1, "Negative forests_per_file child should not crash");
  TEST_ASSERT(negative_forests_result == 1, "Negative forests_per_file should fatal");

  int garbage_target_result = read_parameter_file_should_fatal(garbage_target_path);
  TEST_ASSERT(garbage_target_result != -1, "Garbage target_file_size_mb child should not crash");
  TEST_ASSERT(garbage_target_result == 1, "Garbage target_file_size_mb should fatal");

  int garbage_forests_result = read_parameter_file_should_fatal(garbage_forests_path);
  TEST_ASSERT(garbage_forests_result != -1, "Garbage forests_per_file child should not crash");
  TEST_ASSERT(garbage_forests_result == 1, "Garbage forests_per_file should fatal");

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_null_pre_timestep
 * @brief   pre_timestep: null is accepted as an empty phase (no modules)
 *
 * Regression: before the null-scalar fix, explicit YAML null produced
 * "must be a sequence" instead of treating the phase as empty.
 */
int test_null_pre_timestep(void) {
  char fixture_path[MAX_STRING_LEN];

  /* ===== SETUP ===== */
  setup_test();

  TEST_ASSERT(write_null_phase_fixture(fixture_path, sizeof(fixture_path), "pre_null",
                                       "  pre_timestep: null\n") == 0,
              "Should create pre_timestep: null fixture");

  /* ===== EXECUTE ===== */
  read_parameter_file(fixture_path);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(MimicConfig.num_pre_timestep, 0, "pre_timestep: null should yield 0 modules");
  TEST_ASSERT(MimicConfig.pre_timestep == NULL, "pre_timestep: null pointer should be NULL");

  printf("  pre_timestep: null → %d modules\n", MimicConfig.num_pre_timestep);

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_tilde_post_timestep
 * @brief   post_timestep: ~ (YAML tilde null) is accepted as an empty phase
 *
 * Regression: ~ is the compact YAML null form; the parser must treat it
 * identically to the explicit "null" keyword.
 */
int test_tilde_post_timestep(void) {
  char fixture_path[MAX_STRING_LEN];

  /* ===== SETUP ===== */
  setup_test();

  TEST_ASSERT(write_null_phase_fixture(fixture_path, sizeof(fixture_path), "post_tilde",
                                       "  post_timestep: ~\n") == 0,
              "Should create post_timestep: ~ fixture");

  /* ===== EXECUTE ===== */
  read_parameter_file(fixture_path);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(MimicConfig.num_post_timestep, 0, "post_timestep: ~ should yield 0 modules");
  TEST_ASSERT(MimicConfig.post_timestep == NULL, "post_timestep: ~ pointer should be NULL");

  printf("  post_timestep: ~ → %d modules\n", MimicConfig.num_post_timestep);

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_null_phases_block
 * @brief   phases: null is accepted as no substep phases
 *
 * Regression: a null phases block must be treated identically to an absent
 * phases key — zero substep phases, no crash.
 */
int test_null_phases_block(void) {
  char fixture_path[MAX_STRING_LEN];

  /* ===== SETUP ===== */
  setup_test();

  TEST_ASSERT(write_null_phase_fixture(fixture_path, sizeof(fixture_path), "phases_null",
                                       "  phases: null\n") == 0,
              "Should create phases: null fixture");

  /* ===== EXECUTE ===== */
  read_parameter_file(fixture_path);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(MimicConfig.num_substep_phases, 0,
                    "phases: null should yield 0 substep phases");
  TEST_ASSERT(MimicConfig.substep_phases == NULL,
              "phases: null substep_phases pointer should be NULL");

  printf("  phases: null → %d substep phases\n", MimicConfig.num_substep_phases);

  /* ===== CLEANUP ===== */
  teardown_test();

  return TEST_PASS;
}

/**
 * @test    test_null_named_phase
 * @brief   A named substep phase set to null is accepted with 0 modules
 *
 * Regression: the original bug triggered when named phases had a null value.
 * The phase must be recorded (name preserved) but with 0 modules and a NULL
 * modules pointer.
 */
int test_null_named_phase(void) {
  char fixture_path[MAX_STRING_LEN];

  /* ===== SETUP ===== */
  setup_test();

  TEST_ASSERT(write_null_phase_fixture(fixture_path, sizeof(fixture_path), "named_null",
                                       "  phases:\n    physics: null\n") == 0,
              "Should create phases.physics: null fixture");

  /* ===== EXECUTE ===== */
  read_parameter_file(fixture_path);

  /* ===== VALIDATE ===== */
  TEST_ASSERT_EQUAL(MimicConfig.num_substep_phases, 1,
                    "Named null phase should still be registered as a phase");
  TEST_ASSERT(MimicConfig.substep_phases != NULL,
              "substep_phases array should be allocated for a named phase");
  TEST_ASSERT_EQUAL(MimicConfig.substep_phases[0].num_modules, 0,
                    "Named null phase should have 0 modules");
  TEST_ASSERT(MimicConfig.substep_phases[0].modules == NULL,
              "Named null phase modules pointer should be NULL");
  TEST_ASSERT_STRING_EQUAL(MimicConfig.substep_phases[0].name, "physics",
                           "Named null phase name should be preserved");

  printf("  phases.physics: null → phase '%s' with %d modules\n",
         MimicConfig.substep_phases[0].name, MimicConfig.substep_phases[0].num_modules);

  /* ===== CLEANUP ===== */
  /* parse_modules_section allocates substep_phases via mymalloc and the phase name
   * via strdup; free both so check_memory_leaks() stays clean. */
  free(MimicConfig.substep_phases[0].name);
  myfree(MimicConfig.substep_phases);
  MimicConfig.substep_phases = NULL;
  MimicConfig.num_substep_phases = 0;
  teardown_test();

  return TEST_PASS;
}

/**
 * @brief   Main test runner
 *
 * Executes all test cases and reports results.
 */
int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "--expect-fatal") == 0) {
    setup_test();
    install_output_chunking_defaults_for_test();
    read_parameter_file(argv[2]);
    teardown_test();
    return 0;
  }

  TestExecutablePath = argv[0];

  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Parameter Parsing\n");
  printf("============================================================\n");
  printf("%s", NC);

  /* Initialize error handling for tests */
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  /* Run all test cases */
  TEST_RUN(test_basic_parsing);
  TEST_RUN(test_default_processing_order);
  TEST_RUN(test_explicit_tree_ordered_processing_order);
  TEST_RUN(test_integer_parameters);
  TEST_RUN(test_float_parameters);
  TEST_RUN(test_string_parameters);
  TEST_RUN(test_cosmology_parameters);
  TEST_RUN(test_snapshot_list);
  TEST_RUN(test_output_chunking_defaults);
  TEST_RUN(test_output_chunking_parameters);
  TEST_RUN(test_output_chunking_accepts_zero_forests);
  TEST_RUN(test_simulation_output_chunking_defaults_and_run_override);
  TEST_RUN(test_output_chunking_rejects_bad_values);
  TEST_RUN(test_null_pre_timestep);
  TEST_RUN(test_tilde_post_timestep);
  TEST_RUN(test_null_phases_block);
  TEST_RUN(test_null_named_phase);

  /* Print summary and return result */
  TEST_SUMMARY();
  return TEST_RESULT();
}
