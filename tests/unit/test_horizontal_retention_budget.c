/**
 * @file    test_horizontal_retention_budget.c
 * @brief   Unit tests for the horizontal driver's pre-allocation retention accounting.
 *
 * Before the reader allocates a snapshot's slab, the horizontal driver sizes that
 * snapshot's generation from its halo count and struct widths, reports the size,
 * and -- when input.retention_memory_ceiling_mb is set -- refuses a retention set
 * above the ceiling. These tests drive run_horizontal_driver() itself through a
 * fake HorizontalReader whose snapshot_halo_count() reports any row count,
 * including one above INT32_MAX, and whose load_slab() ends the run the moment it
 * is reached. Nothing is ever loaded, so a slab of billions of rows is exercised
 * through the reader interface's and the driver's index and byte arithmetic
 * without allocating it.
 *
 * Every case runs in a forked child, because both outcomes end the process: a
 * refusal is a FATAL_ERROR, and reaching load_slab() is the fake reader's
 * _exit(). The parent reads the child's log and exit status and checks the
 * reported bytes against the struct-width arithmetic computed here
 * independently.
 *
 * The run_tests.sh entry for this test compiles horizontal_driver.c under -DHDF5
 * in place of the shared, non-HDF5 driver object, whose output setup is a
 * fail-fast stub that would abort before any generation is sized.
 */

#include "../../src/include/proto.h"
#include "../../src/include/types.h"
#include "../../src/io/horizontal/reader.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"
#include "../framework/test_framework.h"

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <hdf5.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

/*
 * Link-time stand-ins for the HDF5 output path the -DHDF5 driver references
 * but these tests never reach: every case ends at or before the first slab
 * load, long before any output is written. allvars.c and metadata_hdf5.c are
 * compiled without -DHDF5 or not linked in the shared unit-test object pool,
 * so this translation unit supplies their symbols, as test_hdf5_write_attrs.c
 * does. Either metadata hook aborts if it is ever called.
 */
size_t HDF5_dst_size;
size_t *HDF5_dst_offsets;
size_t *HDF5_dst_sizes;
const char **HDF5_field_names;
hid_t *HDF5_field_types;
int HDF5_n_props;
hid_t HDF5_current_file_id = -1;

void write_perfile_metadata(hid_t file_id) {
  (void)file_id;
  fprintf(stderr, "write_perfile_metadata() reached in a test that writes no output\n");
  abort();
}

void write_description_attr(hid_t obj_id, const char *text) {
  (void)obj_id;
  (void)text;
  fprintf(stderr, "write_description_attr() reached in a test that writes no output\n");
  abort();
}

/* The fake reader's exit status once load_slab() is reached: distinct from
 * FATAL_ERROR's 1 and from a crash, so the parent can tell "sized and accepted"
 * from "refused". */
#define LOAD_SLAB_REACHED_EXIT 77
#define LOAD_SLAB_REACHED_MARKER "FAKE_READER: load_slab reached"

/* Above INT32_MAX (2,147,483,647) and above MAX_HALO_ARRAY_SIZE, so the output
 * seed takes its past-the-ceiling branch. */
#define WIDE_SLAB_ROWS INT64_C(3000000000)
#define NORMAL_SLAB_ROWS INT64_C(1000)

#define OUTPUT_DIR "archive/test-fixtures/horizontal_retention_budget"

/* ------------------------------------------------------------------------- */
/* Fake reader                                                                */
/* ------------------------------------------------------------------------- */

/* Set by each case before it forks; the child inherits them. */
static int64_t FakeHaloCount = 0;
static int32_t FakeFormatVersion = 3;

static void fake_open_run(struct HorizontalRunInfo *info) {
  info->snapshot_count = 1;
  info->format_version = FakeFormatVersion;
  info->links_adjacent = 1;
  info->n_forests_total = HORIZONTAL_EMPTY_N_FORESTS;
  info->max_halo_rank_in_forest = HORIZONTAL_EMPTY_MAX_RANK;
}

static void fake_close_run(void) {}

static int64_t fake_snapshot_halo_count(int64_t snapnum) {
  (void)snapnum;
  return FakeHaloCount;
}

/* Reached only once the driver has sized and accepted the generation. Ends the
 * run here, before anything the size describes is allocated. */
static void fake_load_slab(int64_t snapnum, struct SnapshotSlab *slab) {
  (void)slab;
  fprintf(stderr, "%s for snapshot %" PRId64 "\n", LOAD_SLAB_REACHED_MARKER, snapnum);
  fflush(NULL);
  _exit(LOAD_SLAB_REACHED_EXIT);
}

static void fake_release_slab(struct SnapshotSlab *slab) { *slab = snapshot_slab_empty(); }

static const struct HorizontalReader FakeReader = {
    .name = "fake_retention_budget",
    .processing_order = INPUT_PROCESSING_ORDER_HORIZONTAL,
    .open_run = fake_open_run,
    .close_run = fake_close_run,
    .snapshot_halo_count = fake_snapshot_halo_count,
    .load_slab = fake_load_slab,
    .release_slab = fake_release_slab,
};

/* ------------------------------------------------------------------------- */
/* Expected footprint, from struct widths                                     */
/* ------------------------------------------------------------------------- */

/* The generation's resident bytes, computed here from the same struct widths
 * and seeding rule the driver documents, independently of its code. */
static int64_t expected_generation_bytes(int64_t nhalos, int32_t format_version) {
  int64_t row = (int64_t)sizeof(struct RawHalo) + 2 * (int64_t)sizeof(int64_t);
  if (format_version >= 3) {
    row += 3 * (int64_t)sizeof(int32_t) + (int64_t)sizeof(int64_t);
  }
  const int64_t slab = nhalos * row;
  const int64_t aux = (nhalos > 0 ? nhalos : 1) * (int64_t)sizeof(struct HorizontalHaloAux);

  int64_t seed;
  if (nhalos > MAX_HALO_ARRAY_SIZE) {
    seed = nhalos + MIN_HALO_ARRAY_GROWTH;
  } else {
    int64_t headroom = (int64_t)((double)nhalos * HORIZONTAL_OUTPUT_SEED_HEADROOM);
    if (headroom < MIN_HALO_ARRAY_GROWTH) {
      headroom = MIN_HALO_ARRAY_GROWTH;
    }
    seed = nhalos + headroom;
    if (seed > MAX_HALO_ARRAY_SIZE) {
      seed = MAX_HALO_ARRAY_SIZE;
    }
  }

  return slab + aux + seed * (int64_t)sizeof(struct Halo);
}

/* ------------------------------------------------------------------------- */
/* Child harness                                                              */
/* ------------------------------------------------------------------------- */

struct ChildResult {
  int exited;      /* the child exited rather than being killed */
  int exit_status; /* its status when it exited */
  char log[16384]; /* everything it wrote to stdout and stderr */
};

/* Run the driver in a child against the fake reader with `nhalos` rows and the
 * given ceiling in bytes (0 = none), capturing its log. Returns 0 on success. */
static int run_driver_in_child(int64_t nhalos, int32_t format_version, int64_t ceiling,
                               struct ChildResult *result) {
  int pipefd[2];
  memset(result, 0, sizeof(*result));

  FakeHaloCount = nhalos;
  FakeFormatVersion = format_version;

  fflush(NULL);
  if (pipe(pipefd) != 0) {
    return -1;
  }

  const pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }

  if (pid == 0) {
    close(pipefd[0]);
    dup2(pipefd[1], STDOUT_FILENO);
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);

    init_memory_system(0);
    initialize_error_handling(LOG_LEVEL_INFO, NULL);
    set_verbose_format(1);

    memset(&MimicConfig, 0, sizeof(MimicConfig));
    snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", OUTPUT_DIR);
    snprintf(MimicConfig.OutputFileBaseName, sizeof(MimicConfig.OutputFileBaseName), "model");
    MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_HORIZONTAL;
    MimicConfig.horizontal_reader = &FakeReader;
    MimicConfig.OverwriteOutputFiles = 1;
    MimicConfig.NOUT = 0;
    MimicConfig.RetentionMemoryCeiling = ceiling;

    run_horizontal_driver();

    /* Unreachable in every case here: the driver either refuses the generation
     * or reaches the fake load_slab(). */
    fprintf(stderr, "FAKE_READER: driver returned\n");
    fflush(NULL);
    _exit(0);
  }

  close(pipefd[1]);
  size_t used = 0;
  ssize_t nread;
  while (used < sizeof(result->log) - 1 &&
         (nread = read(pipefd[0], result->log + used, sizeof(result->log) - 1 - used)) > 0) {
    used += (size_t)nread;
  }
  result->log[used] = '\0';
  close(pipefd[0]);

  int status;
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  result->exited = WIFEXITED(status);
  result->exit_status = result->exited ? WEXITSTATUS(status) : -1;
  return 0;
}

static int prepare_output_dir(void) {
  if (mkdir("archive", 0777) != 0 && errno != EEXIST) {
    return -1;
  }
  if (mkdir("archive/test-fixtures", 0777) != 0 && errno != EEXIST) {
    return -1;
  }
  if (mkdir(OUTPUT_DIR, 0777) != 0 && errno != EEXIST) {
    return -1;
  }
  return 0;
}

static int log_contains(const struct ChildResult *result, const char *needle) {
  return strstr(result->log, needle) != NULL;
}

/* Echo the child's first log line containing `needle`, so the test log carries
 * the driver's own wording as evidence. */
static void print_log_line(const struct ChildResult *result, const char *needle) {
  const char *start = strstr(result->log, needle);
  if (start == NULL) {
    return;
  }
  const char *end = strchr(start, '\n');
  const int length = (end != NULL) ? (int)(end - start) : (int)strlen(start);
  printf("    driver: %.*s\n", length, start);
}

/* The driver's pre-allocation report for snapshot 0, whose retention pool is
 * still empty, so the pool total with it equals the generation's own size. */
static void expected_report(char *buf, size_t size, int64_t bytes, int64_t nhalos) {
  snprintf(buf, size, "Snapshot 0 generation needs %" PRId64 " B for %" PRId64 " halos", bytes,
           nhalos);
}

static void expected_pool_total(char *buf, size_t size, int64_t bytes) {
  snprintf(buf, size, "retention pool holds 0 B, %" PRId64 " B with it", bytes);
}

/* ------------------------------------------------------------------------- */
/* Tests                                                                      */
/* ------------------------------------------------------------------------- */

/**
 * @test    test_reader_interface_carries_wide_count
 * @brief   A halo count above INT32_MAX crosses the reader dispatch unchanged.
 */
int test_reader_interface_carries_wide_count(void) {
  FakeHaloCount = WIDE_SLAB_ROWS;
  const int64_t count = horizontal_reader_halo_count(&FakeReader, 0);
  TEST_ASSERT(count == WIDE_SLAB_ROWS,
              "horizontal_reader_halo_count() should return the reader's int64 count unnarrowed");
  TEST_ASSERT(count > INT32_MAX, "The synthetic slab should be wider than INT32_MAX rows");
  return TEST_PASS;
}

/**
 * @test    test_normal_slab_is_sized_before_load
 * @brief   With no ceiling, a normal slab is sized and reported, then loaded.
 */
int test_normal_slab_is_sized_before_load(void) {
  struct ChildResult result;
  char needle[256];
  const int64_t bytes = expected_generation_bytes(NORMAL_SLAB_ROWS, 2);

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  TEST_ASSERT(run_driver_in_child(NORMAL_SLAB_ROWS, 2, 0, &result) == 0,
              "Should run the driver in a child");

  TEST_ASSERT(result.exited && result.exit_status == LOAD_SLAB_REACHED_EXIT,
              "An unbounded run should reach load_slab()");
  expected_report(needle, sizeof(needle), bytes, NORMAL_SLAB_ROWS);
  TEST_ASSERT(log_contains(&result, needle),
              "The report should state the struct-width size of a version 2 generation");
  TEST_ASSERT(log_contains(&result, "ceiling 0 B (none set)"),
              "The report should say that no ceiling is set");
  printf("  %" PRId64 "-row version 2 slab: %" PRId64 " B reported before load\n", NORMAL_SLAB_ROWS,
         bytes);
  return TEST_PASS;
}

/**
 * @test    test_wide_slab_is_sized_before_load
 * @brief   With no ceiling, a slab above INT32_MAX rows is sized exactly and
 *          reported before load, with no index error or narrowed value.
 */
int test_wide_slab_is_sized_before_load(void) {
  struct ChildResult result;
  char needle[256];
  const int64_t bytes = expected_generation_bytes(WIDE_SLAB_ROWS, 3);

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  TEST_ASSERT(run_driver_in_child(WIDE_SLAB_ROWS, 3, 0, &result) == 0,
              "Should run the driver in a child");

  TEST_ASSERT(result.exited && result.exit_status == LOAD_SLAB_REACHED_EXIT,
              "An unbounded wide slab should be sized and reach load_slab(), not abort");
  expected_report(needle, sizeof(needle), bytes, WIDE_SLAB_ROWS);
  TEST_ASSERT(log_contains(&result, needle),
              "The report should state the wide slab's exact struct-width size");
  expected_pool_total(needle, sizeof(needle), bytes);
  TEST_ASSERT(log_contains(&result, needle), "The report should state the pool total with it");
  TEST_ASSERT(!log_contains(&result, "FATAL"), "An unbounded wide slab should not abort");
  printf("  %" PRId64 "-row version 3 slab: %" PRId64 " B reported before load\n", WIDE_SLAB_ROWS,
         bytes);
  print_log_line(&result, "Snapshot 0 generation needs ");
  return TEST_PASS;
}

/**
 * @test    test_ceiling_accepts_retention_at_the_ceiling
 * @brief   A retention set exactly at the ceiling is accepted, for a normal and a
 *          wide slab.
 */
int test_ceiling_accepts_retention_at_the_ceiling(void) {
  struct ChildResult result;
  const int64_t rows[2] = {NORMAL_SLAB_ROWS, WIDE_SLAB_ROWS};

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  for (int i = 0; i < 2; i++) {
    const int64_t bytes = expected_generation_bytes(rows[i], 3);
    TEST_ASSERT(run_driver_in_child(rows[i], 3, bytes, &result) == 0,
                "Should run the driver in a child");
    TEST_ASSERT(result.exited && result.exit_status == LOAD_SLAB_REACHED_EXIT,
                "A retention set exactly at the ceiling should be accepted and loaded");
    printf("  %" PRId64 " rows at a ceiling of %" PRId64 " B: accepted\n", rows[i], bytes);
  }
  return TEST_PASS;
}

/**
 * @test    test_ceiling_refuses_retention_just_above
 * @brief   One byte over the ceiling is refused before load, naming the
 *          snapshot, the bytes required, the ceiling and the missing capability,
 *          for a normal and a wide slab.
 */
int test_ceiling_refuses_retention_just_above(void) {
  struct ChildResult result;
  char needle[256];
  const int64_t rows[2] = {NORMAL_SLAB_ROWS, WIDE_SLAB_ROWS};

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  for (int i = 0; i < 2; i++) {
    const int64_t bytes = expected_generation_bytes(rows[i], 3);
    const int64_t ceiling = bytes - 1;
    TEST_ASSERT(run_driver_in_child(rows[i], 3, ceiling, &result) == 0,
                "Should run the driver in a child");

    TEST_ASSERT(result.exited && result.exit_status == 1,
                "A retention set one byte above the ceiling should FATAL");
    TEST_ASSERT(!log_contains(&result, LOAD_SLAB_REACHED_MARKER),
                "The refusal should come before the reader loads the slab");
    snprintf(needle, sizeof(needle), "Snapshot 0 needs %" PRId64 " B", bytes);
    TEST_ASSERT(log_contains(&result, needle),
                "The refusal should name the snapshot and the bytes it needs");
    snprintf(needle, sizeof(needle), "bring the retention pool to %" PRId64 " B", bytes);
    TEST_ASSERT(log_contains(&result, needle), "The refusal should name the pool total");
    snprintf(needle, sizeof(needle), "input.retention_memory_ceiling_mb ceiling of %" PRId64 " B",
             ceiling);
    TEST_ASSERT(log_contains(&result, needle), "The refusal should name the ceiling and its key");
    TEST_ASSERT(log_contains(&result, "Refused before allocation"),
                "The refusal should say nothing was allocated");
    TEST_ASSERT(log_contains(&result, "needs chunked slab streaming, a capability Mimic does "
                                      "not implement"),
                "The refusal should name chunked slab streaming as the missing capability");
    printf("  %" PRId64 " rows at a ceiling of %" PRId64 " B: refused before load\n", rows[i],
           ceiling);
    print_log_line(&result, "Snapshot 0 needs ");
  }
  return TEST_PASS;
}

/**
 * @test    test_unrepresentable_slab_is_refused
 * @brief   A row count whose byte size overflows int64_t is refused before load,
 *          with or without a ceiling, rather than wrapped.
 */
int test_unrepresentable_slab_is_refused(void) {
  struct ChildResult result;
  const int64_t ceilings[2] = {0, INT64_MAX};

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  for (int i = 0; i < 2; i++) {
    TEST_ASSERT(run_driver_in_child(INT64_MAX, 3, ceilings[i], &result) == 0,
                "Should run the driver in a child");
    TEST_ASSERT(result.exited && result.exit_status == 1,
                "An INT64_MAX-row slab should FATAL whatever the ceiling");
    TEST_ASSERT(!log_contains(&result, LOAD_SLAB_REACHED_MARKER),
                "The refusal should come before the reader loads the slab");
    TEST_ASSERT(log_contains(&result, "too many for its generation's resident size to be "
                                      "counted in 64-bit bytes"),
                "The refusal should say the size cannot be counted");
    TEST_ASSERT(log_contains(&result, "chunked slab streaming"),
                "The refusal should name chunked slab streaming as the missing capability");
  }
  printf("  INT64_MAX rows: refused before load, never wrapped\n");
  return TEST_PASS;
}

/**
 * @test    test_negative_count_is_refused
 * @brief   A reader reporting a negative halo count is refused before load.
 */
int test_negative_count_is_refused(void) {
  struct ChildResult result;

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  TEST_ASSERT(run_driver_in_child(-1, 3, 0, &result) == 0, "Should run the driver in a child");
  TEST_ASSERT(result.exited && result.exit_status == 1, "A negative halo count should FATAL");
  TEST_ASSERT(log_contains(&result, "a halo count cannot be negative"),
              "The refusal should name the negative count");
  TEST_ASSERT(!log_contains(&result, LOAD_SLAB_REACHED_MARKER),
              "The refusal should come before the reader loads the slab");
  return TEST_PASS;
}

/**
 * @brief   Main test runner
 */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Horizontal Retention Budget\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_reader_interface_carries_wide_count);
  TEST_RUN(test_normal_slab_is_sized_before_load);
  TEST_RUN(test_wide_slab_is_sized_before_load);
  TEST_RUN(test_ceiling_accepts_retention_at_the_ceiling);
  TEST_RUN(test_ceiling_refuses_retention_just_above);
  TEST_RUN(test_unrepresentable_slab_is_refused);
  TEST_RUN(test_negative_count_is_refused);

  TEST_SUMMARY();
  return TEST_RESULT();
}
