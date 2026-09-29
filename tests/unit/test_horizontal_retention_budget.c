/**
 * @file    test_horizontal_retention_budget.c
 * @brief   Unit tests for the horizontal driver's retention memory accounting.
 *
 * Before the reader allocates a snapshot's slab, the horizontal driver sizes that
 * snapshot's generation from its halo count and struct widths (slab and
 * reader-owned arrays, aux, seeded output buffer and, when no spare pool will be
 * reused, a new galaxy pool), reports the size, and -- when
 * input.retention_memory_ceiling_mb is set -- refuses a retention set above the
 * ceiling. After each sweep it measures what the retention pool holds and feeds
 * the run memory profile. These tests drive run_horizontal_driver() itself:
 *
 *  - through a fake HorizontalReader whose snapshot_halo_count() reports any row
 *    count, including one above INT32_MAX, and whose load_slab() ends the run the
 *    moment it is reached, so a slab of billions of rows is exercised through the
 *    reader interface's and the driver's index and byte arithmetic without
 *    allocating it;
 *  - through the same fake reader building small real slabs, so whole sweeps
 *    complete across overlapping generations and the measured maxima, the run
 *    profile and the in-sweep growth warning can be checked;
 *  - through the real horizontal_hdf5 reader on the committed version 3 fixture,
 *    so the driver's per-row slab width is checked against the arrays that reader
 *    actually allocates.
 *
 * Every case that runs the driver does so in a forked child, because each such
 * run ends the process: a refusal is a FATAL_ERROR, reaching the exiting
 * load_slab() is the fake reader's _exit(), and a completed run exits once it has
 * printed its profile. The parent reads the child's log and exit status and
 * checks the reported bytes against struct-width arithmetic computed here
 * independently. (test_reader_interface_carries_wide_count calls only the reader
 * dispatch, in process.)
 *
 * Fake slabs set each RawHalo field by its core role through the generated
 * catalog field table, so the fake reader builds a valid slab under whichever
 * simulation package is compiled.
 *
 * The run_tests.sh entry for this test compiles horizontal_driver.c under -DHDF5
 * in place of the shared, non-HDF5 driver object, whose output setup is a
 * fail-fast stub that would abort before any generation is sized.
 */

#include "../../src/core/galaxy_pool.h"
#include "../../src/include/constants.h"
#include "../../src/include/proto.h"
#include "../../src/include/types.h"
#include "../../src/io/horizontal/reader.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"
#include "../../src/util/run_profile.h"
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
 * but these tests never reach: every case requests no output snapshot, so no
 * output file is ever written. allvars.c and metadata_hdf5.c are compiled
 * without -DHDF5 or not linked in the shared unit-test object pool, so this
 * translation unit supplies their symbols, as test_hdf5_write_attrs.c does.
 * Either metadata hook aborts if it is ever called.
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

/* The child's exit status once the exiting load_slab() is reached, and once a
 * completed run has printed its profile: distinct from FATAL_ERROR's 1 and from
 * a crash, so the parent can tell the outcomes apart. */
#define LOAD_SLAB_REACHED_EXIT 77
#define LOAD_SLAB_REACHED_MARKER "FAKE_READER: load_slab reached"
#define RUN_COMPLETED_EXIT 78

/* Above INT32_MAX (2,147,483,647) and above MAX_HALO_ARRAY_SIZE, so the output
 * seed takes its past-the-ceiling branch. */
#define WIDE_SLAB_ROWS INT64_C(3000000000)
#define NORMAL_SLAB_ROWS INT64_C(1000)

/* More halos than a new galaxy pool's first chunk holds, so a sweep over them
 * grows the pool by a chunk. Checked against the pool itself at run time. */
#define POOL_GROWING_ROWS INT64_C(9000)

/* Whole-MB ceilings are in the parser's unit, 1 MB = 1024^2 B. */
#define BYTES_PER_MB INT64_C(1048576)

#define MAX_FAKE_SNAPSHOTS 4
#define OUTPUT_DIR "archive/test-fixtures/horizontal_retention_budget"

/* The committed version 3 fixture (see tests/data/README.md) and the only
 * package whose /schema it matches. */
#define V3_FIXTURE_DIR "tests/data/horizontal_v3/dataset"
#define V3_FIXTURE_A_LIST "fixture.a_list"
#define V3_FIXTURE_PACKAGE "mini-millennium-horizontal"
#define V3_FIXTURE_SNAPSHOTS 4
/* Reader-owned arrays a version 3 slab carries: halos, two identity columns, three
   target-snapshot columns and SourceHaloID. */
#define V3_SLAB_ARRAYS 7

extern double *Age;

/* ------------------------------------------------------------------------- */
/* Setting RawHalo fields by core role                                        */
/* ------------------------------------------------------------------------- */

static void set_int_member(int *member, double value) { *member = (int)value; }
static void set_long_member(long *member, double value) { *member = (long)value; }
static void set_llong_member(long long *member, double value) { *member = (long long)value; }
static void set_float_member(float *member, double value) { *member = (float)value; }
static void set_double_member(double *member, double value) { *member = value; }

/* Reached only when a core role names a member of a type this setter does not
 * handle, which would leave the fake slab silently wrong. */
static void set_unsupported_member(const void *member, double value) {
  (void)member;
  (void)value;
  fprintf(stderr, "FAKE_READER: a core-role member has a type the fake reader cannot set\n");
  abort();
}

#define SET_MEMBER(pointer, value)                                                                 \
  _Generic((pointer),                                                                              \
      int *: set_int_member,                                                                       \
      long *: set_long_member,                                                                     \
      long long *: set_llong_member,                                                               \
      float *: set_float_member,                                                                   \
      double *: set_double_member,                                                                 \
      default: set_unsupported_member)((pointer), (value))

/* Assign `value` to the member bound to core role `role`, whatever the compiled
 * package calls it. A role no catalog field carries would leave the fake slab
 * silently wrong, so it ends the run instead. */
static void set_role(struct RawHalo *halo, const char *role, double value) {
  int matched = 0;
#define CATALOG_FIELD(member, dataset, type, units, h_convention, core_role, role_kind)            \
  if (strcmp(core_role, role) == 0) {                                                              \
    SET_MEMBER(&halo->member, value);                                                              \
    matched = 1;                                                                                   \
  }
#include "../../src/include/generated/catalog_field_metadata.inc"
#undef CATALOG_FIELD
  if (!matched) {
    fprintf(stderr, "FAKE_READER: no catalog field carries core role '%s'\n", role);
    abort();
  }
}

/* ------------------------------------------------------------------------- */
/* Fake reader                                                                */
/* ------------------------------------------------------------------------- */

/* What load_slab() does: end the run before anything is allocated, or build a
 * small real slab and let the sweep run. */
enum FakeLoad { FAKE_LOAD_EXITS, FAKE_LOAD_BUILDS };

/* Set by each case before it forks; the child inherits it. In a chain, every
 * snapshot holds one halo whose descendant is the next snapshot's halo, so two
 * generations overlap at every step; otherwise every halo is its own FoF group
 * with no descendant, and each generation is released after its own sweep. */
static struct {
  int64_t snapshot_count;
  int64_t halo_count[MAX_FAKE_SNAPSHOTS];
  int32_t format_version;
  enum FakeLoad load;
  int chain;
  int extra_rows;          /* rows built beyond the reported count (count-mismatch case) */
  int descendant_past_end; /* last snapshot's halo names a descendant (horizon case) */
} Fake;

/* The slab row width the fake reader publishes at open, and for version 2 slabs
 * exactly what fake_load_slab allocates per row (the raw record and the two
 * identity columns). The version 3 branch mirrors the real reader's width so the
 * driver's accounting of a version 3 run can be checked; the fake never builds
 * those arrays. */
static int64_t fake_slab_row_bytes(int32_t format_version) {
  int64_t row = (int64_t)sizeof(struct RawHalo) + 2 * (int64_t)sizeof(int64_t);
  if (format_version >= 3) {
    row += 3 * (int64_t)sizeof(int32_t) + (int64_t)sizeof(int64_t);
  }
  return row;
}

static void fake_open_run(struct HorizontalRunInfo *info) {
  int64_t widest = 0;
  for (int64_t k = 0; k < Fake.snapshot_count; k++) {
    widest = Fake.halo_count[k] > widest ? Fake.halo_count[k] : widest;
  }
  info->snapshot_count = Fake.snapshot_count;
  info->format_version = Fake.format_version;
  info->links_adjacent = 1;
  info->slab_row_bytes = fake_slab_row_bytes(Fake.format_version);
  info->n_forests_total = Fake.chain ? 1 : widest;
  info->max_halo_rank_in_forest = Fake.chain ? Fake.snapshot_count - 1 : 0;
}

static void fake_close_run(void) {}

static int64_t fake_snapshot_halo_count(int64_t snapnum) { return Fake.halo_count[snapnum]; }

static void fake_load_slab(int64_t snapnum, struct SnapshotSlab *slab) {
  if (Fake.load == FAKE_LOAD_EXITS) {
    /* Reached only once the driver has sized and accepted the generation. Ends
     * the run here, before anything the size describes is allocated. */
    fprintf(stderr, "%s for snapshot %" PRId64 "\n", LOAD_SLAB_REACHED_MARKER, snapnum);
    fflush(NULL);
    _exit(LOAD_SLAB_REACHED_EXIT);
  }

  /* A version 2 slab: the raw halos and the two identity columns, with links
   * implicitly naming N+1 (Descendant) and N-1 (FirstProgenitor). */
  const int64_t nhalos = Fake.halo_count[snapnum] + Fake.extra_rows;
  *slab = snapshot_slab_empty();
  slab->snapnum = snapnum;
  slab->nhalos = nhalos;
  if (nhalos == 0) {
    return;
  }

  slab->halos = mymalloc(sizeof(struct RawHalo) * (size_t)nhalos);
  slab->forest_index = mymalloc(sizeof(int64_t) * (size_t)nhalos);
  slab->halo_rank_in_forest = mymalloc(sizeof(int64_t) * (size_t)nhalos);
  memset(slab->halos, 0, sizeof(struct RawHalo) * (size_t)nhalos);

  const int has_descendant = Fake.chain && snapnum + 1 < Fake.snapshot_count;
  const int has_progenitor = Fake.chain && snapnum > 0;
  for (int64_t i = 0; i < nhalos; i++) {
    struct RawHalo *halo = &slab->halos[i];
    const int names_descendant =
        has_descendant || (Fake.descendant_past_end && snapnum + 1 == Fake.snapshot_count);
    set_role(halo, "Descendant", names_descendant ? 0 : -1);
    set_role(halo, "FirstProgenitor", has_progenitor ? 0 : -1);
    set_role(halo, "NextProgenitor", -1);
    set_role(halo, "FirstHaloInFOFgroup", (double)i);
    set_role(halo, "NextHaloInFOFgroup", -1);
    set_role(halo, "SnapNum", (double)snapnum);
    set_role(halo, "Len", 100);
    set_role(halo, "HaloMass", 10.0);
    slab->forest_index[i] = Fake.chain ? 0 : i;
    slab->halo_rank_in_forest[i] = Fake.chain ? snapnum : 0;
  }
}

static void fake_release_slab(struct SnapshotSlab *slab) {
  if (slab->halos != NULL) {
    myfree(slab->halo_rank_in_forest);
    myfree(slab->forest_index);
    myfree(slab->halos);
  }
  *slab = snapshot_slab_empty();
}

static const struct HorizontalReader FakeReader = {
    .name = "fake_retention_budget",
    .processing_order = INPUT_PROCESSING_ORDER_HORIZONTAL,
    .open_run = fake_open_run,
    .close_run = fake_close_run,
    .snapshot_halo_count = fake_snapshot_halo_count,
    .load_slab = fake_load_slab,
    .release_slab = fake_release_slab,
};

/* One snapshot of `nhalos`, ended at load: the pre-allocation cases. */
static void fake_single_snapshot(int64_t nhalos, int32_t format_version) {
  memset(&Fake, 0, sizeof(Fake));
  Fake.snapshot_count = 1;
  Fake.halo_count[0] = nhalos;
  Fake.format_version = format_version;
  Fake.load = FAKE_LOAD_EXITS;
}

/* A chain of `snapshots` one-halo snapshots, each descending into the next, built
 * and swept for real: two generations overlap at every step. */
static void fake_chain(int64_t snapshots) {
  memset(&Fake, 0, sizeof(Fake));
  Fake.snapshot_count = snapshots;
  for (int64_t k = 0; k < snapshots; k++) {
    Fake.halo_count[k] = 1;
  }
  Fake.format_version = 2;
  Fake.load = FAKE_LOAD_BUILDS;
  Fake.chain = 1;
}

/* ------------------------------------------------------------------------- */
/* Expected footprint, from struct widths                                     */
/* ------------------------------------------------------------------------- */

/* A new galaxy pool's exact resident bytes, as galaxy_pool_create(0) allocates
 * them. The driver creates pools with that hint. */
static int64_t new_pool_bytes(void) {
  int64_t bytes = -1;
  if (!galaxy_pool_initial_resident_bytes(0, &bytes)) {
    abort();
  }
  return bytes;
}

static int64_t output_seed_records(int64_t nhalos) {
  if (nhalos > MAX_HALO_ARRAY_SIZE) {
    return nhalos + MIN_HALO_ARRAY_GROWTH;
  }
  int64_t headroom = (int64_t)((double)nhalos * HORIZONTAL_OUTPUT_SEED_HEADROOM);
  if (headroom < MIN_HALO_ARRAY_GROWTH) {
    headroom = MIN_HALO_ARRAY_GROWTH;
  }
  const int64_t seed = nhalos + headroom;
  return seed > MAX_HALO_ARRAY_SIZE ? MAX_HALO_ARRAY_SIZE : seed;
}

/* The generation's resident bytes without any galaxy pool: what the driver
 * counted before a new pool was a footprint term. */
static int64_t generation_bytes_without_pool(int64_t nhalos, int32_t format_version) {
  const int64_t slab = nhalos * fake_slab_row_bytes(format_version);
  const int64_t aux = (nhalos > 0 ? nhalos : 1) * (int64_t)sizeof(struct HorizontalHaloAux);
  return slab + aux + output_seed_records(nhalos) * (int64_t)sizeof(struct Halo);
}

/* The generation's footprint as the driver must now compute it: with a new
 * galaxy pool whenever no spare one is reused. */
static int64_t generation_bytes(int64_t nhalos, int32_t format_version, int new_pool) {
  return generation_bytes_without_pool(nhalos, format_version) + (new_pool ? new_pool_bytes() : 0);
}

/* ------------------------------------------------------------------------- */
/* Child harness                                                              */
/* ------------------------------------------------------------------------- */

struct ChildResult {
  int exited;      /* the child exited rather than being killed */
  int exit_status; /* its status when it exited */
  char log[65536]; /* everything it wrote to stdout and stderr */
};

/* Lookback times and redshifts for `count` snapshots, with the leading slot
 * Age[-1] that init() also provides. Values only need to be finite and ordered:
 * no physics module runs. */
static double AgeStorage[ABSOLUTEMAXSNAPS + 1];

static void install_time_axis(int count) {
  Age = AgeStorage + 1;
  Age[-1] = 13.7;
  for (int k = 0; k < count; k++) {
    if (MimicConfig.AA[k] <= 0.0) {
      MimicConfig.AA[k] = 0.5 + 0.5 * (double)k / (double)(count > 1 ? count - 1 : 1);
    }
    MimicConfig.ZZ[k] = 1.0 / MimicConfig.AA[k] - 1.0;
    Age[k] = 13.0 * (1.0 - MimicConfig.AA[k]) + 0.1;
  }
}

/* The configuration every child starts from: a horizontal run of `reader` with
 * no output snapshot requested and an empty module pipeline. */
static void configure_child(const struct HorizontalReader *reader, int64_t ceiling) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  snprintf(MimicConfig.OutputDir, sizeof(MimicConfig.OutputDir), "%s", OUTPUT_DIR);
  snprintf(MimicConfig.OutputFileBaseName, sizeof(MimicConfig.OutputFileBaseName), "model");
  MimicConfig.ProcessingOrder = (int)INPUT_PROCESSING_ORDER_HORIZONTAL;
  MimicConfig.horizontal_reader = reader;
  MimicConfig.OverwriteOutputFiles = 1;
  MimicConfig.NOUT = 0;
  MimicConfig.RetentionMemoryCeiling = ceiling;
  MimicConfig.UniqueGalaxyIDMultiplier = (int64_t)TREE_MUL_FAC;
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.PartMass = 0.0860657;
  MimicConfig.G = 43.0071;
  MimicConfig.Hubble = 100.0;
  MimicConfig.SubSteps = 1;
  MimicConfig.MaxDynamicSubsteps = 200;
  MimicConfig.TimestepScheme = TIMESTEP_SCHEME_FIXED;
}

/* Point the child at the committed version 3 fixture, as a run of its package
 * would configure it (mirrors test_horizontal_v3_reader.c). */
static void configure_v3_fixture(void) {
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s", V3_FIXTURE_DIR);
  snprintf(MimicConfig.FileWithSnapList, sizeof(MimicConfig.FileWithSnapList), "%s/%s",
           V3_FIXTURE_DIR, V3_FIXTURE_A_LIST);
  read_snap_list();
  MimicConfig.MAXSNAPS = MimicConfig.Snaplistlen;
  MimicConfig.BoxSize = 62.5;
  MimicConfig.Omega = 0.25;
  MimicConfig.OmegaLambda = 0.75;
  MimicConfig.Hubble_h = 0.73;
  MimicConfig.PartMass = 0.0860657;
}

/* Run the driver in a child against `reader` (the fake one unless the v3
 * fixture is requested) with the given ceiling in bytes (0 = none), capturing its
 * log. A run that completes prints the run memory profile and the leak check,
 * then exits RUN_COMPLETED_EXIT. Returns 0 on success. */
static int run_driver_in_child(int64_t ceiling, int use_v3_fixture, struct ChildResult *result) {
  int pipefd[2];
  memset(result, 0, sizeof(*result));

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

    const struct HorizontalReader *reader =
        use_v3_fixture ? horizontal_reader_lookup("horizontal_hdf5") : &FakeReader;
    configure_child(reader, ceiling);
    if (use_v3_fixture) {
      configure_v3_fixture();
      install_time_axis(MimicConfig.Snaplistlen);
    } else {
      install_time_axis((int)Fake.snapshot_count);
    }

    run_horizontal_driver();

    print_run_memory_profile();
    check_memory_leaks();
    fflush(NULL);
    _exit(RUN_COMPLETED_EXIT);
  }

  /* Read to EOF even once the log is full, discarding the excess: a child
   * blocked on a full pipe would never exit, and waitpid() would wait forever. */
  close(pipefd[1]);
  size_t used = 0;
  char discard[4096] = {0};
  for (;;) {
    const int keeping = used < sizeof(result->log) - 1;
    char *into = keeping ? result->log + used : discard;
    const size_t room = keeping ? sizeof(result->log) - 1 - used : sizeof(discard);
    const ssize_t nread = read(pipefd[0], into, room);
    if (nread <= 0) {
      break;
    }
    if (keeping) {
      used += (size_t)nread;
    }
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

static int log_count(const struct ChildResult *result, const char *needle) {
  int count = 0;
  for (const char *at = strstr(result->log, needle); at != NULL;
       at = strstr(at + strlen(needle), needle)) {
    count++;
  }
  return count;
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

/* The driver's pre-allocation report for snapshot `snap`: the generation's size
 * and the pool total with it. */
static void expected_report(char *buf, size_t size, int snap, int64_t bytes, int64_t nhalos) {
  snprintf(buf, size, "Snapshot %d generation needs %" PRId64 " B for %" PRId64 " halos", snap,
           bytes, nhalos);
}

static void expected_pool_line(char *buf, size_t size, int64_t pool_bytes, int new_pool,
                               int64_t resident, int64_t required) {
  snprintf(buf, size,
           "galaxy pool %" PRId64 " B%s); retention pool holds %" PRId64 " B, %" PRId64
           " B with it",
           pool_bytes, new_pool ? " new" : ", a resident spare reused", resident, required);
}

/* Run one pre-allocation case: accepted means the exiting load_slab() was
 * reached, refused means a FATAL before it. */
static int admission_accepted(int64_t ceiling, struct ChildResult *result) {
  if (run_driver_in_child(ceiling, 0, result) != 0) {
    return -1;
  }
  if (result->exited && result->exit_status == LOAD_SLAB_REACHED_EXIT) {
    return 1;
  }
  if (result->exited && result->exit_status == 1 &&
      !log_contains(result, LOAD_SLAB_REACHED_MARKER) &&
      log_contains(result, "Refused before allocation")) {
    return 0;
  }
  return -1;
}

/* ------------------------------------------------------------------------- */
/* Tests: pre-allocation sizing and refusal                                   */
/* ------------------------------------------------------------------------- */

/**
 * @test    test_reader_interface_carries_wide_count
 * @brief   A halo count above INT32_MAX crosses the reader dispatch unchanged.
 */
int test_reader_interface_carries_wide_count(void) {
  fake_single_snapshot(WIDE_SLAB_ROWS, 3);
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
  const int64_t bytes = generation_bytes(NORMAL_SLAB_ROWS, 2, 1);

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  fake_single_snapshot(NORMAL_SLAB_ROWS, 2);
  TEST_ASSERT(admission_accepted(0, &result) == 1, "An unbounded run should reach load_slab()");
  expected_report(needle, sizeof(needle), 0, bytes, NORMAL_SLAB_ROWS);
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
  const int64_t bytes = generation_bytes(WIDE_SLAB_ROWS, 3, 1);

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  fake_single_snapshot(WIDE_SLAB_ROWS, 3);
  TEST_ASSERT(admission_accepted(0, &result) == 1,
              "An unbounded wide slab should be sized and reach load_slab(), not abort");
  expected_report(needle, sizeof(needle), 0, bytes, WIDE_SLAB_ROWS);
  TEST_ASSERT(log_contains(&result, needle),
              "The report should state the wide slab's exact struct-width size");
  expected_pool_line(needle, sizeof(needle), new_pool_bytes(), 1, 0, bytes);
  TEST_ASSERT(log_contains(&result, needle),
              "The report should count the new galaxy pool and state the pool total with it");
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
    const int64_t bytes = generation_bytes(rows[i], 3, 1);
    fake_single_snapshot(rows[i], 3);
    TEST_ASSERT(admission_accepted(bytes, &result) == 1,
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
    const int64_t bytes = generation_bytes(rows[i], 3, 1);
    const int64_t ceiling = bytes - 1;
    fake_single_snapshot(rows[i], 3);
    TEST_ASSERT(admission_accepted(ceiling, &result) == 0,
                "A retention set one byte above the ceiling should be refused before load");
    snprintf(needle, sizeof(needle), "Snapshot 0 needs %" PRId64 " B", bytes);
    TEST_ASSERT(log_contains(&result, needle),
                "The refusal should name the snapshot and the bytes it needs");
    snprintf(needle, sizeof(needle), "bring the retention pool to %" PRId64 " B", bytes);
    TEST_ASSERT(log_contains(&result, needle), "The refusal should name the pool total");
    snprintf(needle, sizeof(needle), "input.retention_memory_ceiling_mb ceiling of %" PRId64 " B",
             ceiling);
    TEST_ASSERT(log_contains(&result, needle), "The refusal should name the ceiling and its key");
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
 * @test    test_new_pool_counts_against_a_whole_mb_ceiling_for_an_empty_snapshot
 * @brief   An empty snapshot's footprint includes the galaxy pool it must
 *          create, so a whole-MB ceiling is judged against that too, and the
 *          exact boundary sits at the footprint with the pool.
 *
 * The old footprint (no pool) is below both the exact boundary and one byte
 * under it, so the refusal one byte under is one the old accounting would have
 * accepted. Whether 1 MB itself admits the snapshot depends on the compiled
 * GalaxyData width; the expectation is computed, and the case says whether it
 * discriminates under this package.
 */
int test_new_pool_counts_against_a_whole_mb_ceiling_for_an_empty_snapshot(void) {
  struct ChildResult result;
  const int64_t old_bytes = generation_bytes_without_pool(0, 2);
  const int64_t bytes = generation_bytes(0, 2, 1);
  const int64_t one_mb = BYTES_PER_MB;

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  fake_single_snapshot(0, 2);

  TEST_ASSERT(admission_accepted(one_mb, &result) == (bytes <= one_mb ? 1 : 0),
              "A 1 MB ceiling should admit the empty snapshot exactly when its footprint with "
              "the new pool fits");
  TEST_ASSERT(admission_accepted(bytes, &result) == 1,
              "A ceiling equal to the footprint with the new pool should be accepted");
  TEST_ASSERT(admission_accepted(bytes - 1, &result) == 0,
              "One byte under the footprint with the new pool should be refused");
  TEST_ASSERT(bytes - 1 >= old_bytes,
              "The footprint without the pool should fit the refused ceiling, so the old "
              "accounting would have accepted it");
  printf("  empty snapshot: %" PRId64 " B without the pool, %" PRId64 " B with it; 1 MB %s%s\n",
         old_bytes, bytes, bytes <= one_mb ? "accepts" : "refuses",
         (old_bytes <= one_mb && bytes > one_mb) ? " (the old footprint would have accepted)" : "");
  return TEST_PASS;
}

/**
 * @test    test_new_pool_counts_against_a_whole_mb_ceiling_for_a_tiny_slab
 * @brief   The widest tiny slab whose old footprint fits 1 MB is refused at
 *          1 MB once its new galaxy pool is counted, and is accepted at the
 *          exact boundary of the footprint with the pool.
 */
int test_new_pool_counts_against_a_whole_mb_ceiling_for_a_tiny_slab(void) {
  struct ChildResult result;
  const int64_t one_mb = BYTES_PER_MB;

  int64_t rows = 1;
  while (generation_bytes_without_pool(rows + 1, 2) <= one_mb) {
    rows++;
  }
  const int64_t old_bytes = generation_bytes_without_pool(rows, 2);
  const int64_t bytes = generation_bytes(rows, 2, 1);
  TEST_ASSERT(old_bytes <= one_mb && bytes > one_mb,
              "The chosen slab should fit 1 MB without the pool and not with it");

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  fake_single_snapshot(rows, 2);
  TEST_ASSERT(admission_accepted(one_mb, &result) == 0,
              "A 1 MB ceiling should refuse a slab that fits only when its new pool is ignored");
  TEST_ASSERT(admission_accepted(bytes, &result) == 1,
              "A ceiling equal to the footprint with the new pool should be accepted");
  TEST_ASSERT(admission_accepted(bytes - 1, &result) == 0,
              "One byte under the footprint with the new pool should be refused");
  printf("  %" PRId64 "-row slab: %" PRId64 " B without the pool, %" PRId64
         " B with it; 1 MB refuses\n",
         rows, old_bytes, bytes);
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
  fake_single_snapshot(INT64_MAX, 3);
  for (int i = 0; i < 2; i++) {
    TEST_ASSERT(run_driver_in_child(ceilings[i], 0, &result) == 0,
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
  fake_single_snapshot(-1, 3);
  TEST_ASSERT(run_driver_in_child(0, 0, &result) == 0, "Should run the driver in a child");
  TEST_ASSERT(result.exited && result.exit_status == 1, "A negative halo count should FATAL");
  TEST_ASSERT(log_contains(&result, "a halo count cannot be negative"),
              "The refusal should name the negative count");
  TEST_ASSERT(!log_contains(&result, LOAD_SLAB_REACHED_MARKER),
              "The refusal should come before the reader loads the slab");
  return TEST_PASS;
}

/**
 * @test    test_count_mismatch_after_load_is_refused
 * @brief   A reader that loads a different row count from the one it reported is
 *          refused, and the generation already counted as retained is released
 *          by the failure path.
 */
int test_count_mismatch_after_load_is_refused(void) {
  struct ChildResult result;

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  fake_single_snapshot(2, 2);
  Fake.load = FAKE_LOAD_BUILDS;
  Fake.extra_rows = 1;
  TEST_ASSERT(run_driver_in_child(0, 0, &result) == 0, "Should run the driver in a child");
  TEST_ASSERT(result.exited && result.exit_status == 1,
              "A slab loaded with a different row count should FATAL");
  TEST_ASSERT(log_contains(&result, "loaded 3 halos for snapshot 0 after reporting 2; the "
                                    "generation was sized for the reported count"),
              "The refusal should name the loaded and reported counts");
  TEST_ASSERT(log_contains(&result, "releasing 1 retained generation"),
              "The failure path should release the generation counted before the check");
  /* Logged after the release and the reader close, not before them. */
  TEST_ASSERT(log_contains(&result, "Released snapshot 0 (raw slab"),
              "The failure path should actually release snapshot 0");
  TEST_ASSERT(log_contains(&result, "Closed horizontal run 'fake_retention_budget' with no slab"),
              "The failure path should close the reader's run with no slab loaded");
  printf("  3 rows loaded after 2 reported: refused, retained generation released\n");
  return TEST_PASS;
}

/**
 * @test    test_horizon_beyond_the_run_is_refused
 * @brief   A slab whose halo names a descendant snapshot past the run's last one
 *          is refused, and the retained generation is released by the failure path.
 */
int test_horizon_beyond_the_run_is_refused(void) {
  struct ChildResult result;

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  fake_chain(2);
  Fake.descendant_past_end = 1;
  TEST_ASSERT(run_driver_in_child(0, 0, &result) == 0, "Should run the driver in a child");
  TEST_ASSERT(result.exited && result.exit_status == 1,
              "A descendant beyond the last snapshot should FATAL");
  TEST_ASSERT(log_contains(&result, "names descendant snapshot 2, beyond the run's last "
                                    "snapshot 1"),
              "The refusal should name the horizon and the run's last snapshot");
  TEST_ASSERT(log_contains(&result, "releasing 2 retained generations"),
              "The failure path should release both retained generations");
  /* Logged after the release and the reader close, not before them. */
  TEST_ASSERT(log_contains(&result, "Released snapshot 0 (raw slab") &&
                  log_contains(&result, "Released snapshot 1 (raw slab"),
              "The failure path should actually release snapshots 0 and 1");
  TEST_ASSERT(log_contains(&result, "Closed horizontal run 'fake_retention_budget' with no slab"),
              "The failure path should close the reader's run with no slab loaded");
  printf("  descendant past the last snapshot: refused, generations released\n");
  return TEST_PASS;
}

/* ------------------------------------------------------------------------- */
/* Tests: completed sweeps, measurement and the run profile                   */
/* ------------------------------------------------------------------------- */

/**
 * @test    test_overlapping_generations_are_measured_and_profiled
 * @brief   A three-snapshot chain retains two generations at once; the driver's
 *          admission figures, its peak and the run profile all equal the
 *          struct-width accounting, and a new pool is counted exactly when no
 *          spare exists.
 *
 * Snapshots 0 and 1 each create a galaxy pool (nothing has been released yet);
 * snapshot 2 reuses the pool snapshot 0 released, which is resident as a spare
 * when snapshot 2 is admitted. Each generation holds one halo, one galaxy and
 * one output record, so nothing grows during a sweep and every generation holds
 * exactly its footprint with a new pool.
 */
int test_overlapping_generations_are_measured_and_profiled(void) {
  struct ChildResult result;
  char needle[256];
  const int64_t pool = new_pool_bytes();
  const int64_t generation = generation_bytes(1, 2, 1);
  const int64_t peak = 2 * generation;

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  fake_chain(3);

  TEST_ASSERT(run_driver_in_child(0, 0, &result) == 0, "Should run the driver in a child");
  TEST_ASSERT(result.exited && result.exit_status == RUN_COMPLETED_EXIT,
              "The chain should be processed to the end");

  expected_pool_line(needle, sizeof(needle), pool, 1, 0, generation);
  TEST_ASSERT(log_contains(&result, needle),
              "Snapshot 0 should be admitted with a new pool into an empty retention pool");
  expected_pool_line(needle, sizeof(needle), pool, 1, generation, peak);
  TEST_ASSERT(log_contains(&result, needle),
              "Snapshot 1 should be admitted with a new pool beside snapshot 0's generation");
  expected_pool_line(needle, sizeof(needle), 0, 0, generation + pool, peak);
  TEST_ASSERT(log_contains(&result, needle),
              "Snapshot 2 should reuse the released pool, which is already resident");
  TEST_ASSERT(log_count(&result, "Released snapshot ") == 3,
              "Every snapshot should be released exactly once");

  snprintf(needle, sizeof(needle),
           "Retained at most 2 generations concurrently, holding at most %" PRId64 " B resident",
           peak);
  TEST_ASSERT(log_contains(&result, needle),
              "The driver should report two concurrent generations and their exact peak bytes");
  TEST_ASSERT(log_contains(&result, "Retained generations R: at most 2 concurrently"),
              "The run profile should report two concurrent generations");
  snprintf(needle, sizeof(needle), "Retention pool resident: at most %" PRId64 " B", peak);
  TEST_ASSERT(log_contains(&result, needle),
              "The run profile should report the driver's own peak resident bytes");
  TEST_ASSERT(log_contains(&result, "No memory leaks detected"),
              "Every generation, pool and fake slab should be released");
  TEST_ASSERT(!log_contains(&result, "The retention pool grew to"),
              "No ceiling is set, so no in-sweep warning should be issued");
  printf("  3-snapshot chain: 2 generations, %" PRId64 " B peak (new pool %" PRId64 " B)\n", peak,
         pool);
  print_log_line(&result, "Retention pool resident:");
  return TEST_PASS;
}

/**
 * @test    test_ceiling_counts_the_resident_generation_below_the_boundary
 * @brief   With snapshot 0's generation still resident, snapshot 1 is refused
 *          one byte below the resident-plus-footprint total, and the refusal
 *          counts the resident generation.
 *
 * Snapshot 1's own footprint fits this ceiling many times over, so only the
 * resident term -- snapshot 0's whole generation, still retained because its
 * halo descends into snapshot 1 -- can push the total over it. An edit that
 * dropped the resident term from required = resident + footprint would admit
 * snapshot 1 here.
 */
int test_ceiling_counts_the_resident_generation_below_the_boundary(void) {
  struct ChildResult result;
  char needle[256];
  const int64_t generation = generation_bytes(1, 2, 1);
  const int64_t required = 2 * generation;
  const int64_t ceiling = required - 1;

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  fake_chain(2);
  TEST_ASSERT(generation <= ceiling, "Snapshot 1's own footprint should fit the ceiling");

  TEST_ASSERT(run_driver_in_child(ceiling, 0, &result) == 0, "Should run the driver in a child");
  TEST_ASSERT(result.exited && result.exit_status == 1,
              "Snapshot 1 should be refused one byte below the resident-plus-footprint total");
  expected_pool_line(needle, sizeof(needle), new_pool_bytes(), 1, 0, generation);
  TEST_ASSERT(log_contains(&result, needle), "Snapshot 0 should be admitted first");
  expected_pool_line(needle, sizeof(needle), new_pool_bytes(), 1, generation, required);
  TEST_ASSERT(log_contains(&result, needle),
              "Snapshot 1's admission should count snapshot 0's resident generation");
  snprintf(needle, sizeof(needle),
           "Snapshot 1 needs %" PRId64 " B (%.3f GB) resident for its 1 halos, which would bring "
           "the retention pool to %" PRId64 " B",
           generation, (double)generation / 1.0e9, required);
  TEST_ASSERT(log_contains(&result, needle),
              "The refusal should name snapshot 1, its own bytes and the resident-inclusive total");
  snprintf(needle, sizeof(needle), "input.retention_memory_ceiling_mb ceiling of %" PRId64 " B",
           ceiling);
  TEST_ASSERT(log_contains(&result, needle), "The refusal should name the ceiling");
  TEST_ASSERT(log_contains(&result, "Horizontal driver exiting early: releasing 1 retained "
                                    "generation"),
              "The failure path should release exactly snapshot 0's resident generation");
  TEST_ASSERT(log_count(&result, "Released snapshot ") == 1,
              "Only snapshot 0 was ever retained, so only it should be released");
  printf("  2-snapshot chain at %" PRId64 " B: snapshot 1 refused (own %" PRId64
         " B + resident %" PRId64 " B)\n",
         ceiling, generation, generation);
  print_log_line(&result, "Snapshot 1 needs ");
  return TEST_PASS;
}

/**
 * @test    test_ceiling_admits_the_resident_generation_at_the_boundary
 * @brief   With the ceiling exactly at the resident-plus-footprint total, every
 *          snapshot of a chain is admitted, whether its admission creates a new
 *          galaxy pool or reuses a resident spare.
 *
 * Snapshot 1 is admitted with a new pool beside snapshot 0's resident
 * generation. Snapshot 2 is admitted beside snapshot 1's generation and the
 * spare pool snapshot 0 left, which it reuses: the same total, reached with the
 * pool counted as resident rather than as footprint.
 */
int test_ceiling_admits_the_resident_generation_at_the_boundary(void) {
  struct ChildResult result;
  char needle[256];
  const int64_t pool = new_pool_bytes();
  const int64_t generation = generation_bytes(1, 2, 1);
  const int64_t required = 2 * generation;

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  fake_chain(3);
  TEST_ASSERT(run_driver_in_child(required, 0, &result) == 0, "Should run the driver in a child");
  TEST_ASSERT(result.exited && result.exit_status == RUN_COMPLETED_EXIT,
              "Every snapshot should be admitted exactly at the ceiling");
  expected_pool_line(needle, sizeof(needle), pool, 1, generation, required);
  TEST_ASSERT(log_contains(&result, needle),
              "Snapshot 1 should be admitted with a new pool beside the resident generation");
  expected_pool_line(needle, sizeof(needle), 0, 0, generation + pool, required);
  TEST_ASSERT(log_contains(&result, needle),
              "Snapshot 2 should be admitted reusing the resident spare pool");
  TEST_ASSERT(!log_contains(&result, "Refused before allocation"), "Nothing should be refused");
  TEST_ASSERT(!log_contains(&result, "The retention pool grew to"),
              "Nothing grows in these sweeps, so the ceiling should never be passed");
  snprintf(needle, sizeof(needle), "Retention pool resident: at most %" PRId64 " B", required);
  TEST_ASSERT(log_contains(&result, needle), "The peak should sit exactly at the ceiling");
  printf("  3-snapshot chain at %" PRId64 " B: all admitted (new pool, then spare reused)\n",
         required);
  return TEST_PASS;
}

/**
 * @test    test_in_sweep_pool_growth_past_the_ceiling_warns_once
 * @brief   Growth the admission check cannot see -- a galaxy pool adding a chunk
 *          mid-sweep -- is measured, reported in the profile and warned about
 *          once; the next admission counts the grown pool.
 *
 * A single snapshot holds more isolated halos than a new pool's first chunk, so
 * its sweep grows the pool; the ceiling is set exactly at its admission figure,
 * so admission passes and the grown pool passes the ceiling. A second, unbounded
 * run adds a one-halo snapshot after it, whose admission must count the grown
 * pool it reuses. (Under a ceiling that second admission may or may not fit,
 * depending on the compiled struct widths, so it is checked without one.)
 */
int test_in_sweep_pool_growth_past_the_ceiling_warns_once(void) {
  struct ChildResult result;
  char needle[256];

  /* The pool a sweep over POOL_GROWING_ROWS galaxies leaves behind, measured on
   * a real pool, and what it adds beyond a new pool. */
  init_memory_system(0);
  struct GalaxyPool *grown = galaxy_pool_create(0);
  struct GalaxyPoolStats fresh_stats;
  galaxy_pool_stats(grown, &fresh_stats);
  for (int64_t i = 0; i < POOL_GROWING_ROWS; i++) {
    (void)galaxy_pool_alloc(grown);
  }
  struct GalaxyPoolStats grown_stats;
  galaxy_pool_stats(grown, &grown_stats);
  galaxy_pool_destroy(grown);
  check_memory_leaks();
  TEST_ASSERT(grown_stats.chunk_count > fresh_stats.chunk_count,
              "The sweep's galaxies should outgrow a new pool's first chunk");
  const int64_t growth = grown_stats.resident_bytes - fresh_stats.resident_bytes;

  const int64_t admission = generation_bytes(POOL_GROWING_ROWS, 2, 1);
  const int64_t swept = admission + growth;

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  memset(&Fake, 0, sizeof(Fake));
  Fake.snapshot_count = 1;
  Fake.halo_count[0] = POOL_GROWING_ROWS;
  Fake.format_version = 2;
  Fake.load = FAKE_LOAD_BUILDS;

  TEST_ASSERT(run_driver_in_child(admission, 0, &result) == 0, "Should run the driver in a child");
  TEST_ASSERT(result.exited && result.exit_status == RUN_COMPLETED_EXIT,
              "A snapshot admitted exactly at the ceiling should be processed");
  snprintf(needle, sizeof(needle),
           "The retention pool grew to %" PRId64 " B during snapshot 0's sweep, above the "
           "input.retention_memory_ceiling_mb ceiling of %" PRId64 " B",
           swept, admission);
  TEST_ASSERT(log_contains(&result, needle),
              "The in-sweep growth past the ceiling should be warned about with exact bytes");
  TEST_ASSERT(log_contains(&result, "Only in-sweep growth of the output buffer and galaxy pool "
                                    "can pass the ceiling"),
              "The warning should name in-sweep output-buffer and galaxy-pool growth only");
  TEST_ASSERT(log_count(&result, "The retention pool grew to") == 1,
              "The warning should be issued once per run");
  snprintf(needle, sizeof(needle), "Retention pool resident: at most %" PRId64 " B", swept);
  TEST_ASSERT(log_contains(&result, needle),
              "The run profile should report the grown peak, not the admission figure");
  TEST_ASSERT(log_contains(&result, "Retained generations R: at most 1 concurrently"),
              "The run profile should report the one generation retained");
  TEST_ASSERT(log_contains(&result, "No memory leaks detected"),
              "Every generation, pool and fake slab should be released");
  printf("  %" PRId64 " isolated halos: admitted at %" PRId64 " B, swept to %" PRId64 " B\n",
         POOL_GROWING_ROWS, admission, swept);
  print_log_line(&result, "The retention pool grew to");

  Fake.snapshot_count = 2;
  Fake.halo_count[1] = 1;
  TEST_ASSERT(run_driver_in_child(0, 0, &result) == 0, "Should run the driver in a child");
  TEST_ASSERT(result.exited && result.exit_status == RUN_COMPLETED_EXIT,
              "The unbounded two-snapshot run should be processed");
  expected_pool_line(needle, sizeof(needle), 0, 0, grown_stats.resident_bytes,
                     grown_stats.resident_bytes + generation_bytes(1, 2, 0));
  TEST_ASSERT(log_contains(&result, needle),
              "The next admission should count the grown pool it reuses");
  TEST_ASSERT(!log_contains(&result, "The retention pool grew to"),
              "No ceiling is set, so no in-sweep warning should be issued");
  return TEST_PASS;
}

/* ------------------------------------------------------------------------- */
/* Tests: the slab width against the real reader                              */
/* ------------------------------------------------------------------------- */

/**
 * @test    test_slab_width_matches_the_real_v3_reader
 * @brief   For every snapshot of the committed version 3 fixture, the slab row
 *          width the real horizontal_hdf5 reader publishes, and the slab bytes the
 *          driver reports before load, match the bytes the allocator records for
 *          the real reader's load_slab (to within its 8-byte block rounding).
 *
 * Skips unless the fixture's package is compiled, as the fixture's own reader
 * tests do.
 */
int test_slab_width_matches_the_real_v3_reader(void) {
  struct ChildResult result;
  char needle[256];

  if (strcmp(MIMIC_COMPILED_SIMULATION, V3_FIXTURE_PACKAGE) != 0) {
    return TEST_SKIP_WITH("the v3 fixture's /schema matches only SIMULATION=" V3_FIXTURE_PACKAGE);
  }

  TEST_ASSERT(prepare_output_dir() == 0, "Should create the output directory");
  TEST_ASSERT(run_driver_in_child(0, 1, &result) == 0, "Should run the driver in a child");
  TEST_ASSERT(result.exited && result.exit_status == RUN_COMPLETED_EXIT,
              "The driver should process the whole fixture");

  init_memory_system(0);
  configure_child(horizontal_reader_lookup("horizontal_hdf5"), 0);
  configure_v3_fixture();
  const struct HorizontalReader *reader = MimicConfig.horizontal_reader;
  struct HorizontalRunInfo info;
  horizontal_reader_open_run(reader, &info);
  TEST_ASSERT(info.format_version == 3 && info.snapshot_count == V3_FIXTURE_SNAPSHOTS,
              "The fixture should open as a four-snapshot version 3 run");

  int loaded_nonempty = 0;
  for (int64_t snap = 0; snap < info.snapshot_count; snap++) {
    struct SnapshotSlab slab = snapshot_slab_empty();
    const size_t before = memory_category_bytes(MEM_TREES);
    horizontal_reader_load_slab(reader, snap, &slab);
    const int64_t actual = (int64_t)(memory_category_bytes(MEM_TREES) - before);
    /* The allocator rounds each block up to 8 B, so a column whose byte count is not a
       multiple of 8 (int32 columns of an odd row count) records up to 7 B more than
       nhalos times its width. Never less, and never more than that per array. */
    const int64_t published = slab.nhalos * info.slab_row_bytes;
    TEST_ASSERT(actual >= published && actual - published <= V3_SLAB_ARRAYS * 7,
                "load_slab's allocator delta should be nhalos times the published row width, "
                "plus at most the allocator's 8-byte block rounding");
    if (slab.nhalos > 0) {
      TEST_ASSERT(slab.descendant_snapshot != NULL && slab.source_halo_id != NULL,
                  "A non-empty version 3 slab should carry its reader-owned v3 arrays");
      loaded_nonempty++;
    }
    snprintf(needle, sizeof(needle),
             "for %" PRId64 " halos (slab and reader-owned arrays %" PRId64 " B,", slab.nhalos,
             published);
    char prefix[64];
    snprintf(prefix, sizeof(prefix), "Snapshot %" PRId64 " generation needs ", snap);
    const char *line = strstr(result.log, prefix);
    TEST_ASSERT(line != NULL, "The driver should report every snapshot's footprint");
    const char *end = strchr(line, '\n');
    const size_t length = (end != NULL) ? (size_t)(end - line) : strlen(line);
    char report[1024];
    snprintf(report, sizeof(report), "%.*s", (int)length, line);
    TEST_ASSERT(strstr(report, needle) != NULL,
                "The driver's slab bytes should equal nhalos times the reader's published width");
    printf("  snapshot %" PRId64 ": %" PRId64 " halos, allocator recorded %" PRId64
           " B, driver reported %" PRId64 " B\n",
           snap, slab.nhalos, actual, published);
    horizontal_reader_release_slab(reader, &slab);
  }
  horizontal_reader_close_run(reader);
  TEST_ASSERT(loaded_nonempty > 0, "The fixture should hold at least one non-empty snapshot");
  check_memory_leaks();
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
  TEST_RUN(test_new_pool_counts_against_a_whole_mb_ceiling_for_an_empty_snapshot);
  TEST_RUN(test_new_pool_counts_against_a_whole_mb_ceiling_for_a_tiny_slab);
  TEST_RUN(test_unrepresentable_slab_is_refused);
  TEST_RUN(test_negative_count_is_refused);
  TEST_RUN(test_count_mismatch_after_load_is_refused);
  TEST_RUN(test_horizon_beyond_the_run_is_refused);
  TEST_RUN(test_overlapping_generations_are_measured_and_profiled);
  TEST_RUN(test_ceiling_counts_the_resident_generation_below_the_boundary);
  TEST_RUN(test_ceiling_admits_the_resident_generation_at_the_boundary);
  TEST_RUN(test_in_sweep_pool_growth_past_the_ceiling_warns_once);
  TEST_RUN(test_slab_width_matches_the_real_v3_reader);

  TEST_SUMMARY();
  return TEST_RESULT();
}
