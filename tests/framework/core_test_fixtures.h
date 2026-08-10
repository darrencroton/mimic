/**
 * @file    core_test_fixtures.h
 * @brief   Shared fixture boilerplate for core (model-neutral) C unit tests
 *
 * Core-test sibling of the model-owned fixture headers (e.g.
 * models/sage16/modules/_tests/sage_test_fixtures.h). Include it after the
 * standard test includes; paths resolve via the -I flags set by
 * tests/unit/run_tests.sh.
 *
 * This header carries fixtures only: it must never weaken or absorb test
 * assertions. Test-statistics counters stay file-owned.
 */

#ifndef CORE_TEST_FIXTURES_H
#define CORE_TEST_FIXTURES_H

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "core/module_registry.h"
#include "include/globals.h"
#include "include/types.h"

/* Test fixture: reset global configuration state between tests */
static inline void reset_config(void) { memset(&MimicConfig, 0, sizeof(MimicConfig)); }

/* Test fixture: register all compiled modules exactly once */
static inline void ensure_modules_registered(void) {
  static int modules_registered = 0;
  if (!modules_registered) {
    register_all_modules();
    modules_registered = 1;
  }
}

/**
 * @brief   Does the compiled SIMULATION package declare processing_order:
 *          snapshot_ordered?
 *
 * Reads simulations/<SIMULATION>/simulation_info.yaml directly rather than
 * hardcoding package names, so this covers any future snapshot-ordered
 * package without a fixture edit here.
 */
static inline int compiled_simulation_is_snapshot_ordered(void) {
  char path[MAX_STRING_LEN];
  char line[512];
  FILE *fp;
  int result = 0;

  snprintf(path, sizeof(path), "simulations/%s/simulation_info.yaml", MIMIC_COMPILED_SIMULATION);
  fp = fopen(path, "r");
  if (fp == NULL) {
    return 0;
  }
  while (fgets(line, sizeof(line), fp) != NULL) {
    if (strstr(line, "processing_order:") != NULL && strstr(line, "snapshot_ordered") != NULL) {
      result = 1;
      break;
    }
  }
  fclose(fp);
  return result;
}

/* Test fixture: generated core run file for the compiled MODEL/SIMULATION pair */
static inline const char *test_binary_param_file(void) {
  static char path[MAX_STRING_LEN];
  snprintf(path, sizeof(path), "build/generated/test_inputs/%s/%s/core/test_binary.yaml",
           MIMIC_COMPILED_MODEL, MIMIC_COMPILED_SIMULATION);
  return path;
}

/**
 * @brief   Install main.c's CLI default for MimicConfig.OverwriteOutputFiles.
 *
 * main.c:216 sets this to 1 before any run file is parsed; --skip (main.c:256)
 * is the only thing that clears it. No C unit test drives that CLI path, so a
 * test that calls read_parameter_file() directly leaves the field at its
 * memset/BSS zero -- bit-for-bit indistinguishable from "--skip was given" --
 * which the snapshot-configuration gating in validate_and_postprocess()
 * rejects. Call this before parsing any configuration a test expects to pass
 * validation.
 */
static inline void install_overwrite_output_default_for_test(void) {
  MimicConfig.OverwriteOutputFiles = 1;
}

/**
 * @brief   A generated core run file valid for the compiled MODEL/SIMULATION
 *          pair, for tests that need cosmology only -- not the pair's true
 *          processing order or reader family.
 *
 * output_format: binary is rejected at config time for a snapshot-ordered
 * package (validate_and_postprocess()'s snapshot-configuration gating), so
 * test_binary_param_file() is invalid by construction for such a package.
 * output_format: hdf5 is not a usable substitute here: tests/unit/run_tests.sh
 * compiles the shared CORE_SRCS object (including read_parameter_file.c) once
 * without -DHDF5 -- only a short, explicit list of HDF5-reader source files
 * gets that flag -- so 'hdf5' hits read_parameter_file.c's own #ifndef HDF5
 * guard and FATALs there regardless of whether HDF5 is actually available on
 * the machine. No generated core run file is parseable by this shared object
 * for a snapshot-ordered package.
 *
 * For such a package this instead returns a scratch copy of
 * test_binary_param_file() (which still carries the package's real
 * cosmology) with input.processing_order forced to tree_ordered and
 * input.tree_type/tree_name forced to a registered tree reader -- never
 * opened, since these tests only reach the config-time reader lookup, not the
 * driver. For every other package this returns test_binary_param_file()
 * unchanged.
 */
static inline const char *test_cosmology_param_file(void) {
  static char path[MAX_STRING_LEN];
  FILE *src;
  FILE *dst;
  char line[1024];
  int wrote_input = 0;

  if (!compiled_simulation_is_snapshot_ordered()) {
    return test_binary_param_file();
  }

  mkdir("archive", 0777);
  mkdir("archive/test-fixtures", 0777);
  snprintf(path, sizeof(path), "archive/test-fixtures/test_cosmology_%s_%s.yaml",
           MIMIC_COMPILED_MODEL, MIMIC_COMPILED_SIMULATION);

  src = fopen(test_binary_param_file(), "r");
  if (src == NULL) {
    return path;
  }
  dst = fopen(path, "w");
  if (dst == NULL) {
    fclose(src);
    return path;
  }

  while (fgets(line, sizeof(line), src) != NULL) {
    const char *cursor = line;
    while (*cursor == ' ' || *cursor == '\t') {
      cursor++;
    }
    /* Drop any inherited reader declaration; the override below replaces it. */
    if (strncmp(cursor, "processing_order:", strlen("processing_order:")) == 0 ||
        strncmp(cursor, "tree_type:", strlen("tree_type:")) == 0 ||
        strncmp(cursor, "tree_name:", strlen("tree_name:")) == 0) {
      continue;
    }
    fputs(line, dst);
    if (!wrote_input && strncmp(line, "input:", 6) == 0 &&
        (line[6] == '\n' || line[6] == '\r' || line[6] == '\0')) {
      fputs("  processing_order: tree_ordered\n  tree_type: lhalo_binary\n  tree_name: "
            "trees_063\n",
            dst);
      wrote_input = 1;
    }
  }
  if (!wrote_input) {
    fputs("\ninput:\n  processing_order: tree_ordered\n  tree_type: lhalo_binary\n"
          "  tree_name: trees_063\n",
          dst);
  }

  fclose(dst);
  fclose(src);
  return path;
}

#endif /* CORE_TEST_FIXTURES_H */
