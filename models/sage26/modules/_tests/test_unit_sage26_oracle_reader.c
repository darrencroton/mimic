/**
 * @file    test_unit_sage26_oracle_reader.c
 * @brief   Self-test of the SAGE26 oracle fixture reader's limits (sage26_oracle_fixture.h)
 *
 * The shared reader every oracle unit test uses once capped a module at 64 fixture
 * names, stopping silently at the cap, and a fixture file at 512 fields. These tests
 * pin the lifted limits (oracle README "Reader Limits") on scratch manifests and
 * fixture files written under $TMPDIR:
 *
 * - a manifest listing more than 64 fixtures for a module is read in full, up to and
 *   including ORACLE_MAX_FIXTURES, and its fixture count check passes;
 * - a fixture file of more than 512 fields is read in full, up to and including
 *   ORACLE_MAX_FIELDS;
 * - a manifest or file beyond a limit (fixtures, declared count, fields, line length,
 *   token length, path length) fails with a message naming the limit;
 * - a truncated manifest (fewer fixtures than declared, or a listed fixture missing a
 *   file) still fails its count check, and a truncated fixture file fails to load.
 *
 * Error paths run in a forked child (tests/framework/child_capture.h) so their stderr
 * can be checked for the limit's name. Platform independent: no bit-exact comparison.
 * The scratch directory holds only files this test names, and it removes exactly those
 * (remove() and rmdir(), POSIX.1 and C; no nftw, which glibc hides in default gnu mode).
 */

#include "framework/test_framework.h"
#include "framework/child_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "modules/_tests/sage26_oracle_fixture.h"

/* Test framework counters (each test translation unit owns its own) */
static int passed = 0;
static int failed = 0;

#define MODULE "sage26_reader_selftest"
#define OTHER_MODULE "sage26_reader_selftest_other"

/** Scratch files at the top of the scratch directory (each removed at exit if present) */
static const char *const scratch_leaves[] = {"manifest_full", "manifest_count", "manifest_over",
                                             "manifest_long", "fixture_full",   "fixture_over",
                                             "fixture_bad"};

/** The fixture whose .out the missing-file case hides, and the extension it is hidden under */
#define HIDDEN_FIXTURE 100
#define HIDDEN_EXT ".out.hidden"

/** Scratch directory for this run; created in main(), removed at exit */
static char scratch_dir[ORACLE_MAX_PATH];

/** Arguments of the child bodies (set by the parent before each fork) */
static char child_manifest[ORACLE_MAX_PATH];
static char child_file[ORACLE_MAX_PATH];

// ============================================================================
// SCRATCH FILES
// ============================================================================

/** @brief "<scratch>/<leaf>" */
static void scratch_path(char *buffer, const size_t size, const char *leaf) {
  snprintf(buffer, size, "%s/%s", scratch_dir, leaf);
}

/** @brief Fixture name number i, "fx_<i>" zero-padded so names sort and are distinct */
static void fixture_name(char *buffer, const size_t size, const int i) {
  snprintf(buffer, size, "fx_%04d", i);
}

/**
 * @brief Write a manifest declaring `declared` fixtures for MODULE and listing `listed`
 *
 * Another module's lines are interleaved, as in the real manifest, so the reader must
 * select by owner.
 * @return 0 on success
 */
static int write_manifest(const char *path, const int declared, const int listed) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return -1;
  }
  fprintf(file, "# scratch manifest for the reader self-test\n\n");
  fprintf(file, "module %s 2\nfixture %s other_a\n", OTHER_MODULE, OTHER_MODULE);
  fprintf(file, "module %s %d\n", MODULE, declared);
  for (int i = 0; i < listed; i++) {
    char name[ORACLE_MAX_NAME];
    fixture_name(name, sizeof(name), i);
    fprintf(file, "fixture %s %s\n", MODULE, name);
    if (i == listed / 2) {
      fprintf(file, "fixture %s other_b\n", OTHER_MODULE);
    }
  }
  return fclose(file) == 0 ? 0 : -1;
}

/** The three directories of <scratch>/<module>/_tests/oracle/, outermost first */
static const char *const fixture_dirs[3] = {MODULE, MODULE "/_tests", MODULE "/_tests/oracle"};

/** @brief Path of fixture i's file with extension ext under the scratch tree; 0 on success */
static int fixture_file(char *buffer, const size_t size, const int i, const char *ext) {
  char name[ORACLE_MAX_NAME];
  fixture_name(name, sizeof(name), i);
  return oracle_fixture_path_in(buffer, size, scratch_dir, MODULE, name, ext);
}

/** @brief Write a placeholder file (presence is all the count check reads) */
static int touch(const char *path) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return -1;
  }
  fputs(ORACLE_FORMAT_LINE "\n", file);
  return fclose(file) == 0 ? 0 : -1;
}

/**
 * @brief Create <scratch>/<module>/_tests/oracle/ holding the .in and .out of fixtures
 *        0 .. ORACLE_MAX_FIXTURES - 1, so a manifest of any size up to the limit is all present
 * @return 0 on success
 */
static int make_fixture_tree(void) {
  for (int d = 0; d < 3; d++) {
    char path[ORACLE_MAX_PATH];
    scratch_path(path, sizeof(path), fixture_dirs[d]);
    if (mkdir(path, 0700) != 0) {
      return -1;
    }
  }
  const char *exts[2] = {".in", ".out"};
  for (int i = 0; i < ORACLE_MAX_FIXTURES; i++) {
    for (int e = 0; e < 2; e++) {
      char path[ORACLE_MAX_PATH];
      if (fixture_file(path, sizeof(path), i, exts[e]) != 0 || touch(path) != 0) {
        return -1;
      }
    }
  }
  return 0;
}

/**
 * @brief Remove exactly what this test created: the scratch leaves, the fixture files (and a
 *        still-hidden .out), the fixture directories innermost first, then the scratch directory
 */
static void remove_scratch(void) {
  char path[ORACLE_MAX_PATH];
  for (size_t k = 0; k < sizeof(scratch_leaves) / sizeof(scratch_leaves[0]); k++) {
    scratch_path(path, sizeof(path), scratch_leaves[k]);
    (void)remove(path);
  }
  const char *exts[3] = {".in", ".out", HIDDEN_EXT};
  for (int i = 0; i < ORACLE_MAX_FIXTURES; i++) {
    for (int e = 0; e < 3; e++) {
      if (fixture_file(path, sizeof(path), i, exts[e]) == 0) {
        (void)remove(path);
      }
    }
  }
  for (int d = 2; d >= 0; d--) {
    scratch_path(path, sizeof(path), fixture_dirs[d]);
    (void)rmdir(path);
  }
  if (rmdir(scratch_dir) != 0) {
    fprintf(stderr, "  note: scratch directory %s is not empty and is left in place\n",
            scratch_dir);
  }
}

/**
 * @brief Write a fixture file with `nfields` f64 fields: field.<i> holds the double i
 * @return 0 on success
 */
static int write_fixture(const char *path, const int nfields) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return -1;
  }
  fprintf(file, "# scratch fixture for the reader self-test\n%s\nroutine selftest\n",
          ORACLE_FORMAT_LINE);
  for (int i = 0; i < nfields; i++) {
    fprintf(file, "f64 field.%d 0x%016" PRIx64 "  # %d\n", i, oracle_f64_to_bits((double)i), i);
  }
  return fclose(file) == 0 ? 0 : -1;
}

/** @brief Write a file holding exactly `text` */
static int write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  if (file == NULL) {
    return -1;
  }
  fputs(text, file);
  return fclose(file) == 0 ? 0 : -1;
}

// ============================================================================
// CHILD BODIES (stderr captured by the parent)
// ============================================================================

/** @brief Read child_manifest for MODULE; exit 0 if it reads, 1 if the reader refuses it */
static void body_read_manifest(const char *arg) {
  (void)arg;
  static char names[ORACLE_MAX_FIXTURES][ORACLE_MAX_NAME];
  int declared = -1;
  const int listed = oracle_manifest_fixtures_in(child_manifest, MODULE, names, &declared);
  _exit(listed < 0 ? 1 : 0);
}

/** @brief Load child_file; exit 0 if it loads, 1 if the reader refuses it */
static void body_load_file(const char *arg) {
  (void)arg;
  static struct oracle_fixture fx;
  _exit(oracle_load(child_file, &fx) == 0 ? 0 : 1);
}

/** A modules directory long enough that no fixture path under it fits ORACLE_MAX_PATH */
static char long_dir[ORACLE_MAX_PATH + 64];

/**
 * @brief Build a path under long_dir; exit 1 if it is refused with the buffer emptied (never a
 *        truncated path), 0 otherwise
 */
static void body_long_fixture_path(const char *arg) {
  (void)arg;
  char path[ORACLE_MAX_PATH];
  const int status = oracle_fixture_path_in(path, sizeof(path), long_dir, MODULE, "fx", ".in");
  _exit(status != 0 && path[0] == '\0' ? 1 : 0);
}

/** @brief Load a fixture whose path is longer than ORACLE_MAX_PATH; exit 1 if refused */
static void body_load_long_path(const char *arg) {
  (void)arg;
  static struct oracle_fixture fx;
  _exit(oracle_load(long_dir, &fx) == 0 ? 0 : 1);
}

/**
 * @brief Require the reader to refuse the input with a message containing `needle`
 * @return 1 when it does, 0 otherwise (captured output printed), -1 on a harness failure
 */
static int expect_refused(child_body_fn body, const char *needle) {
  return expect_fatal(NULL, body, needle, NULL);
}

// ============================================================================
// TESTS: MANIFEST
// ============================================================================

/**
 * @test    test_manifest_beyond_64_read_in_full
 * @brief   200 fixtures (more than the old cap of 64) and ORACLE_MAX_FIXTURES are read in full
 */
int test_manifest_beyond_64_read_in_full(void) {
  static char names[ORACLE_MAX_FIXTURES][ORACLE_MAX_NAME];
  char path[ORACLE_MAX_PATH];
  const int sizes[2] = {200, ORACLE_MAX_FIXTURES};
  for (int s = 0; s < 2; s++) {
    scratch_path(path, sizeof(path), "manifest_full");
    TEST_ASSERT(write_manifest(path, sizes[s], sizes[s]) == 0, "scratch manifest must be written");
    int declared = -1;
    const int listed = oracle_manifest_fixtures_in(path, MODULE, names, &declared);
    printf("\n  manifest of %d fixtures: listed %d, declared %d ", sizes[s], listed, declared);
    TEST_ASSERT(listed == sizes[s], "every fixture line must be read (no silent stop at 64)");
    TEST_ASSERT(declared == sizes[s], "the declared count must be read");
    const int probes[4] = {0, 63, 64, sizes[s] - 1};
    for (int p = 0; p < 4; p++) {
      char expected[ORACLE_MAX_NAME];
      fixture_name(expected, sizeof(expected), probes[p]);
      TEST_ASSERT(strcmp(names[probes[p]], expected) == 0,
                  "fixture names must be read in manifest order, past the 64th");
    }
  }
  return TEST_PASS;
}

/**
 * @test    test_fixture_count_beyond_64
 * @brief   The count check passes for 200 present fixtures and fails for a truncated manifest
 *          or a listed fixture missing its .out
 */
int test_fixture_count_beyond_64(void) {
  const int n = 200;
  char manifest[ORACLE_MAX_PATH];
  scratch_path(manifest, sizeof(manifest), "manifest_count");
  TEST_ASSERT(write_manifest(manifest, n, n) == 0, "scratch manifest must be written");
  TEST_ASSERT(oracle_fixture_count_ok_in(manifest, scratch_dir, MODULE),
              "200 listed and present fixtures must pass the count check");

  /* Truncated manifest: one fixture line fewer than declared */
  TEST_ASSERT(write_manifest(manifest, n, n - 1) == 0, "scratch manifest must be written");
  TEST_ASSERT(!oracle_fixture_count_ok_in(manifest, scratch_dir, MODULE),
              "a manifest listing fewer fixtures than it declares must fail the count check");

  /* A listed fixture whose .out is missing (hidden for the check, then restored) */
  char out[ORACLE_MAX_PATH];
  char hidden[ORACLE_MAX_PATH];
  TEST_ASSERT(fixture_file(out, sizeof(out), HIDDEN_FIXTURE, ".out") == 0 &&
                  fixture_file(hidden, sizeof(hidden), HIDDEN_FIXTURE, HIDDEN_EXT) == 0,
              "scratch fixture paths must fit");
  TEST_ASSERT(write_manifest(manifest, n, n) == 0, "scratch manifest must be written");
  TEST_ASSERT(rename(out, hidden) == 0, "a fixture's .out must be hidden for the check");
  const int ok_with_missing = oracle_fixture_count_ok_in(manifest, scratch_dir, MODULE);
  TEST_ASSERT(rename(hidden, out) == 0, "the hidden .out must be restored");
  TEST_ASSERT(!ok_with_missing, "a listed fixture without its .out must fail the count check");
  return TEST_PASS;
}

/**
 * @test    test_manifest_over_limit_refused
 * @brief   More than ORACLE_MAX_FIXTURES fixture lines, or a larger declared count, is refused
 *          with a message naming ORACLE_MAX_FIXTURES, and the count check fails
 *
 * All ORACLE_MAX_FIXTURES listed fixtures are present (make_fixture_tree), so the count check
 * passes for a manifest at the limit; with one line more it can fail only because the reader
 * refuses that line. A reader that dropped it silently would list ORACLE_MAX_FIXTURES, equal
 * to the declared count, all present, and pass.
 */
int test_manifest_over_limit_refused(void) {
  scratch_path(child_manifest, sizeof(child_manifest), "manifest_over");

  TEST_ASSERT(write_manifest(child_manifest, ORACLE_MAX_FIXTURES, ORACLE_MAX_FIXTURES) == 0,
              "scratch manifest must be written");
  TEST_ASSERT(oracle_fixture_count_ok_in(child_manifest, scratch_dir, MODULE),
              "a manifest at ORACLE_MAX_FIXTURES, all present, must pass the count check");

  TEST_ASSERT(write_manifest(child_manifest, ORACLE_MAX_FIXTURES, ORACLE_MAX_FIXTURES + 1) == 0,
              "scratch manifest must be written");
  TEST_ASSERT(
      !oracle_fixture_count_ok_in(child_manifest, scratch_dir, MODULE),
      "one fixture line over ORACLE_MAX_FIXTURES must fail the count check, not be dropped");
  TEST_ASSERT(expect_refused(body_read_manifest, "ORACLE_MAX_FIXTURES") == 1,
              "one fixture line over ORACLE_MAX_FIXTURES must be refused, naming the limit");

  TEST_ASSERT(write_manifest(child_manifest, ORACLE_MAX_FIXTURES + 1, 1) == 0,
              "scratch manifest must be written");
  TEST_ASSERT(expect_refused(body_read_manifest, "ORACLE_MAX_FIXTURES") == 1,
              "a declared count over ORACLE_MAX_FIXTURES must be refused, naming the limit");
  return TEST_PASS;
}

/**
 * @test    test_manifest_overlong_line_and_name_refused
 * @brief   A manifest line of ORACLE_MAX_LINE characters or more, or a fixture name of
 *          ORACLE_MAX_NAME or more, is refused naming the limit, not cut
 */
int test_manifest_overlong_line_and_name_refused(void) {
  static char text[4 * ORACLE_MAX_LINE];
  scratch_path(child_manifest, sizeof(child_manifest), "manifest_long");

  /* A comment line one character longer than a line may be */
  int used = snprintf(text, sizeof(text), "module %s 1\nfixture %s fx_0000\n#", MODULE, MODULE);
  for (int i = 0; i < ORACLE_MAX_LINE - 1; i++) {
    text[used++] = 'c';
  }
  text[used++] = '\n';
  text[used] = '\0';
  TEST_ASSERT(write_text(child_manifest, text) == 0, "scratch manifest must be written");
  TEST_ASSERT(expect_refused(body_read_manifest, "ORACLE_MAX_LINE") == 1,
              "a manifest line longer than ORACLE_MAX_LINE - 1 must be refused, naming the limit");

  /* A fixture name of exactly ORACLE_MAX_NAME characters */
  used = snprintf(text, sizeof(text), "module %s 1\nfixture %s ", MODULE, MODULE);
  for (int i = 0; i < ORACLE_MAX_NAME; i++) {
    text[used++] = 'n';
  }
  text[used++] = '\n';
  text[used] = '\0';
  TEST_ASSERT(write_text(child_manifest, text) == 0, "scratch manifest must be written");
  TEST_ASSERT(expect_refused(body_read_manifest, "ORACLE_MAX_NAME") == 1,
              "a fixture name of ORACLE_MAX_NAME characters must be refused, naming the limit");
  return TEST_PASS;
}

// ============================================================================
// TESTS: FIXTURE FILES
// ============================================================================

/**
 * @test    test_file_beyond_512_read_in_full
 * @brief   1500 fields (more than the old cap of 512) and ORACLE_MAX_FIELDS are read in full
 */
int test_file_beyond_512_read_in_full(void) {
  static struct oracle_fixture fx;
  char path[ORACLE_MAX_PATH];
  scratch_path(path, sizeof(path), "fixture_full");
  const int sizes[2] = {1500, ORACLE_MAX_FIELDS};
  for (int s = 0; s < 2; s++) {
    TEST_ASSERT(write_fixture(path, sizes[s]) == 0, "scratch fixture must be written");
    TEST_ASSERT(oracle_load(path, &fx) == 0, "a fixture within ORACLE_MAX_FIELDS must load");
    printf("\n  fixture of %d fields: read %d ", sizes[s], fx.num_fields);
    TEST_ASSERT(fx.num_fields == sizes[s], "every field statement must be read");
    TEST_ASSERT(strcmp(fx.routine, "selftest") == 0, "the routine line must be read");
    const int probes[4] = {0, 511, 512, sizes[s] - 1};
    for (int p = 0; p < 4; p++) {
      char name[ORACLE_MAX_NAME];
      snprintf(name, sizeof(name), "field.%d", probes[p]);
      const double value = oracle_in_f64(&fx, name);
      TEST_ASSERT(oracle_f64_to_bits(value) == oracle_f64_to_bits((double)probes[p]),
                  "a field past the 512th must hold its exact bit pattern");
    }
    TEST_ASSERT(fx.errors == 0, "no probed field may be missing or mistyped");
  }
  return TEST_PASS;
}

/**
 * @test    test_file_over_limit_refused
 * @brief   A fixture of ORACLE_MAX_FIELDS + 1 fields is refused with a message naming
 *          ORACLE_MAX_FIELDS
 */
int test_file_over_limit_refused(void) {
  scratch_path(child_file, sizeof(child_file), "fixture_over");
  TEST_ASSERT(write_fixture(child_file, ORACLE_MAX_FIELDS + 1) == 0,
              "scratch fixture must be written");
  TEST_ASSERT(expect_refused(body_load_file, "ORACLE_MAX_FIELDS") == 1,
              "one field over ORACLE_MAX_FIELDS must be refused, naming the limit");
  return TEST_PASS;
}

/**
 * @test    test_file_truncated_or_overlong_refused
 * @brief   A fixture file cut short, or with an overlong line, name or out-of-range i32,
 *          is refused with a message saying why
 */
int test_file_truncated_or_overlong_refused(void) {
  static char text[4 * ORACLE_MAX_LINE];
  scratch_path(child_file, sizeof(child_file), "fixture_bad");

  /* Cut mid-statement: the last field has no value */
  snprintf(text, sizeof(text), "%s\nroutine selftest\nf64 field.0 0x0000000000000000\nf64 field.1",
           ORACLE_FORMAT_LINE);
  TEST_ASSERT(write_text(child_file, text) == 0, "scratch fixture must be written");
  TEST_ASSERT(expect_refused(body_load_file, "malformed field statement") == 1,
              "a fixture cut mid-statement must be refused");

  /* Cut after the format line: no routine */
  snprintf(text, sizeof(text), "%s\n", ORACLE_FORMAT_LINE);
  TEST_ASSERT(write_text(child_file, text) == 0, "scratch fixture must be written");
  TEST_ASSERT(expect_refused(body_load_file, "missing format or routine line") == 1,
              "a fixture cut after its format line must be refused");

  /* A line one character longer than a line may be (a long trailing comment) */
  int used = snprintf(text, sizeof(text), "%s\nroutine selftest\n", ORACLE_FORMAT_LINE);
  const int line_start = used;
  used += snprintf(text + used, sizeof(text) - (size_t)used, "f64 field.0 0x%016d  #", 0);
  while (used - line_start < ORACLE_MAX_LINE + 40) {
    text[used++] = 'c';
  }
  text[used++] = '\n';
  text[used] = '\0';
  TEST_ASSERT(write_text(child_file, text) == 0, "scratch fixture must be written");
  TEST_ASSERT(expect_refused(body_load_file, "ORACLE_MAX_LINE") == 1,
              "a fixture line longer than ORACLE_MAX_LINE - 1 must be refused, naming the limit");

  /* A field name of exactly ORACLE_MAX_NAME characters */
  used = snprintf(text, sizeof(text), "%s\nroutine selftest\nf64 ", ORACLE_FORMAT_LINE);
  for (int i = 0; i < ORACLE_MAX_NAME; i++) {
    text[used++] = 'n';
  }
  used += snprintf(text + used, sizeof(text) - (size_t)used, " 0x0000000000000000\n");
  TEST_ASSERT(write_text(child_file, text) == 0, "scratch fixture must be written");
  TEST_ASSERT(expect_refused(body_load_file, "ORACLE_MAX_NAME") == 1,
              "a field name of ORACLE_MAX_NAME characters must be refused, naming the limit");

  /* An i32 outside the 32-bit range, which a cast would silently wrap */
  snprintf(text, sizeof(text), "%s\nroutine selftest\ni32 call.n 2147483648\n", ORACLE_FORMAT_LINE);
  TEST_ASSERT(write_text(child_file, text) == 0, "scratch fixture must be written");
  TEST_ASSERT(expect_refused(body_load_file, "bad field") == 1,
              "an i32 outside the 32-bit range must be refused");
  return TEST_PASS;
}

/**
 * @test    test_path_over_limit_refused
 * @brief   A fixture path longer than ORACLE_MAX_PATH - 1 is refused naming ORACLE_MAX_PATH, in
 *          oracle_fixture_path_in (buffer emptied) and in oracle_load, never truncated
 */
int test_path_over_limit_refused(void) {
  memset(long_dir, 'd', sizeof(long_dir) - 1);
  long_dir[0] = '/';
  long_dir[sizeof(long_dir) - 1] = '\0';
  TEST_ASSERT(expect_refused(body_long_fixture_path, "ORACLE_MAX_PATH") == 1,
              "a fixture path that does not fit must be refused, naming ORACLE_MAX_PATH");
  TEST_ASSERT(expect_refused(body_load_long_path, "ORACLE_MAX_PATH") == 1,
              "loading a path longer than ORACLE_MAX_PATH - 1 must be refused, naming the limit");
  return TEST_PASS;
}

int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: SAGE26 oracle fixture reader limits (self-test)\n");
  printf("============================================================\n");
  printf("%s", NC);

  char template_path[ORACLE_MAX_PATH];
  test_temp_template(template_path, sizeof(template_path), "mimic_oracle_reader_XXXXXX");
  if (mkdtemp(template_path) == NULL) {
    fprintf(stderr, "cannot create a scratch directory under $TMPDIR\n");
    return 1;
  }
  snprintf(scratch_dir, sizeof(scratch_dir), "%s", template_path);
  if (make_fixture_tree() != 0) {
    fprintf(stderr, "cannot create the scratch fixture tree under %s\n", scratch_dir);
    remove_scratch();
    return 1;
  }

  TEST_RUN(test_manifest_beyond_64_read_in_full);
  TEST_RUN(test_fixture_count_beyond_64);
  TEST_RUN(test_manifest_over_limit_refused);
  TEST_RUN(test_manifest_overlong_line_and_name_refused);
  TEST_RUN(test_file_beyond_512_read_in_full);
  TEST_RUN(test_file_over_limit_refused);
  TEST_RUN(test_file_truncated_or_overlong_refused);
  TEST_RUN(test_path_over_limit_refused);

  remove_scratch();

  TEST_SUMMARY();
  return TEST_RESULT();
}
