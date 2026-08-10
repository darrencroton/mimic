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

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "core/module_registry.h"
#include "include/constants.h"
#include "include/globals.h"
#include "include/types.h"
#include "util/error.h"

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
 * @brief   Does `line` declare `input.processing_order: snapshot_ordered`?
 *
 * Anchored, not a substring search: skips leading whitespace, rejects a
 * comment line outright, then requires the literal key immediately at that
 * position and the captured value to equal "snapshot_ordered" exactly. A
 * prose comment such as "# processing_order: snapshot_ordered is not
 * supported here" -- plausible in a package's simulation_info.yaml banner --
 * must not match.
 */
static inline int yaml_line_declares_snapshot_ordered(const char *line) {
  const char *cursor = line;
  char value[64];

  while (*cursor == ' ' || *cursor == '\t') {
    cursor++;
  }
  if (*cursor == '#') {
    return 0;
  }
  return sscanf(cursor, "processing_order: %63s", value) == 1 &&
         strcmp(value, "snapshot_ordered") == 0;
}

/**
 * @brief   Does the compiled SIMULATION package declare processing_order:
 *          snapshot_ordered?
 *
 * Reads simulations/<SIMULATION>/simulation_info.yaml directly rather than
 * hardcoding package names, so a future snapshot-ordered package is detected
 * without a fixture edit here. Detection alone does not make
 * test_cosmology_param_file() safe for such a package -- see its own doc
 * comment for the rest of what that requires.
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
    if (yaml_line_declares_snapshot_ordered(line)) {
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
 * @brief   Is `line` a bare top-level YAML mapping header matching `key:`?
 *
 * True only for a line that, after skipping leading whitespace, is exactly
 * `key:` followed by nothing but trailing whitespace/newline -- not `key:
 * value` on one line, and not `key:` written at some other indentation.
 * Every generated core run file emits `model:`, `simulation:`, `output:`,
 * `input:`, etc. in this exact bare block-mapping form
 * (scripts/generate_test_inputs.py via yaml.safe_dump(default_flow_style=
 * False)), so this is the correct anchor for "this line opens section key".
 */
static inline int yaml_line_is_bare_section_header(const char *line, const char *key) {
  const char *cursor = line;
  const size_t key_len = strlen(key);

  while (*cursor == ' ' || *cursor == '\t') {
    cursor++;
  }
  if (strncmp(cursor, key, key_len) != 0 || cursor[key_len] != ':') {
    return 0;
  }
  cursor += key_len + 1;
  while (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n') {
    cursor++;
  }
  return *cursor == '\0';
}

/**
 * @brief   Is `line` a top-level YAML key line (any `key:` at column 0)?
 *
 * Used only to detect leaving whichever section a scan is currently inside:
 * every generated core run file nests section contents by exactly two spaces
 * (yaml.safe_dump's default), so a line with no leading whitespace is always
 * either a fresh top-level key, a blank line, or a `#`-comment line -- never
 * a continuation of the section above it.
 */
static inline int yaml_line_is_top_level_key(const char *line) {
  return line[0] != '\0' && line[0] != ' ' && line[0] != '\t' && line[0] != '#' &&
         line[0] != '\n' && line[0] != '\r' && strchr(line, ':') != NULL;
}

/**
 * @brief   Does `line`, skipping leading whitespace, start with `key`?
 *
 * Used by test_cosmology_param_file() to drop one specific nested key while
 * scoped inside the section it belongs to -- see the section-tracking there,
 * which is what keeps this anchored rather than a same-named key elsewhere
 * in the file being dropped by accident.
 */
static inline int yaml_line_starts_with_key(const char *line, const char *key) {
  const char *cursor = line;
  while (*cursor == ' ' || *cursor == '\t') {
    cursor++;
  }
  return strncmp(cursor, key, strlen(key)) == 0;
}

/**
 * @brief   A generated core run file valid for the compiled MODEL/SIMULATION
 *          pair, for tests that need cosmology only -- not the pair's true
 *          processing order, reader family, or identity multiplier.
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
 * cosmology) with:
 *   - input.processing_order forced to tree_ordered and input.tree_type/
 *     tree_name forced to a registered tree reader -- never opened, since
 *     these tests only reach the config-time reader lookup, not the driver;
 *   - simulation.unique_galaxy_id_multiplier forced to the tree-ordered
 *     encoder's hard-coded TREE_MUL_FAC, since the tree-ordered branch these
 *     overrides now route through hard-rejects any other value, and nothing
 *     requires a snapshot-ordered package's declared multiplier (validated
 *     instead against its own identity bounds) to equal TREE_MUL_FAC.
 * Both overrides are scoped to the section they belong to (tracked while
 * scanning, not matched as a bare substring), so a same-named key belonging
 * to a different section is never touched. For every other package this
 * returns test_binary_param_file() unchanged.
 */
static inline const char *test_cosmology_param_file(void) {
  static char path[MAX_STRING_LEN];
  FILE *src;
  FILE *dst;
  char line[1024];
  int wrote_input = 0;
  int wrote_multiplier = 0;
  int in_input_section = 0;
  int in_simulation_section = 0;

  if (!compiled_simulation_is_snapshot_ordered()) {
    return test_binary_param_file();
  }

  if (mkdir("archive", 0777) != 0 && errno != EEXIST) {
    FATAL_ERROR("test_cosmology_param_file: failed to create 'archive' (%s)", strerror(errno));
  }
  if (mkdir("archive/test-fixtures", 0777) != 0 && errno != EEXIST) {
    FATAL_ERROR("test_cosmology_param_file: failed to create 'archive/test-fixtures' (%s)",
                strerror(errno));
  }
  snprintf(path, sizeof(path), "archive/test-fixtures/test_cosmology_%s_%s.yaml",
           MIMIC_COMPILED_MODEL, MIMIC_COMPILED_SIMULATION);

  src = fopen(test_binary_param_file(), "r");
  if (src == NULL) {
    FATAL_ERROR("test_cosmology_param_file: failed to open '%s' (%s)", test_binary_param_file(),
                strerror(errno));
    return NULL; /* unreachable: FATAL_ERROR calls myexit(), declared noreturn */
  }
  dst = fopen(path, "w");
  if (dst == NULL) {
    fclose(src);
    FATAL_ERROR("test_cosmology_param_file: failed to create '%s' (%s)", path, strerror(errno));
    return NULL; /* unreachable: FATAL_ERROR calls myexit(), declared noreturn */
  }

  while (fgets(line, sizeof(line), src) != NULL) {
    if (yaml_line_is_bare_section_header(line, "input")) {
      in_input_section = 1;
      in_simulation_section = 0;
      fputs(line, dst);
      fputs("  processing_order: tree_ordered\n  tree_type: lhalo_binary\n  tree_name: "
            "trees_063\n",
            dst);
      wrote_input = 1;
      continue;
    }
    if (yaml_line_is_bare_section_header(line, "simulation")) {
      in_simulation_section = 1;
      in_input_section = 0;
      fputs(line, dst);
      fprintf(dst, "  unique_galaxy_id_multiplier: %lld\n", (long long)TREE_MUL_FAC);
      wrote_multiplier = 1;
      continue;
    }
    if (yaml_line_is_top_level_key(line)) {
      /* Left whichever section (if any) the previous lines were inside. */
      in_input_section = 0;
      in_simulation_section = 0;
    }
    /* Drop only the one inherited key each override above already replaced,
       and only while still inside the section that key belongs to. */
    if (in_input_section && (yaml_line_starts_with_key(line, "processing_order:") ||
                             yaml_line_starts_with_key(line, "tree_type:") ||
                             yaml_line_starts_with_key(line, "tree_name:"))) {
      continue;
    }
    if (in_simulation_section && yaml_line_starts_with_key(line, "unique_galaxy_id_multiplier:")) {
      continue;
    }
    fputs(line, dst);
  }
  if (!wrote_input) {
    fputs("\ninput:\n  processing_order: tree_ordered\n  tree_type: lhalo_binary\n"
          "  tree_name: trees_063\n",
          dst);
  }
  if (!wrote_multiplier) {
    fprintf(dst, "\nsimulation:\n  unique_galaxy_id_multiplier: %lld\n", (long long)TREE_MUL_FAC);
  }

  fclose(dst);
  fclose(src);
  return path;
}

#endif /* CORE_TEST_FIXTURES_H */
