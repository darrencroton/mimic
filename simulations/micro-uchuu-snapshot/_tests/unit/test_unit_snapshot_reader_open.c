/**
 * @file    test_unit_snapshot_reader_open.c
 * @brief   Unit tests for the snapshot_hdf5 reader's run lifecycle.
 *
 * Validates: registry lookup, the run metadata and per-snapshot counts
 * published by open_run against the committed fixture dataset, the
 * out-of-range count contract, every corrupt-input abort the format requires,
 * the missing-hook diagnostic, and freedom from leaks across open/close.
 *
 * Corrupt-input cases work on a scratch copy of the committed fixture: the
 * parent stages the copy, mutates it with the HDF5 C API, then forks a child
 * that opens it. open_run aborts through FATAL_ERROR -> myexit(), so a child
 * process is the only way to observe an abort and its message. That mirrors
 * tests/unit/test_parameter_parsing.c, which isolates configuration FATALs the
 * same way.
 *
 * The committed fixture is deliberately chunked (8,)/(8,3) rather than the
 * production (65536,), so these tests also exercise the reader's independence
 * from chunk shape.
 */

#include "../../../../tests/framework/test_framework.h"

#include "../../../../src/include/proto.h"
#include "../../../../src/include/types.h"
#include "../../../../src/io/snapshot/reader.h"
#include "../../../../src/util/error.h"
#include "../../../../src/util/memory.h"

#include <hdf5.h>

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Test statistics (required for TEST_RUN macro) */
static int passed = 0;
static int failed = 0;

extern struct MimicConfig MimicConfig;

/* ---------------------------------------------------------------------------
 * Fixture facts (simulations/micro-uchuu-snapshot/_tests/data)
 * ------------------------------------------------------------------------- */

#define FIXTURE_SNAPSHOTS 6
#define FIXTURE_N_FORESTS_TOTAL 3
#define FIXTURE_MAX_RANK 6
#define FIXTURE_FORMAT_VERSION 1
#define FIXTURE_A_LIST "micro-uchuu-fixture.a_list"

static const int64_t FIXTURE_HALO_COUNTS[FIXTURE_SNAPSHOTS] = {0, 1, 1, 1, 6, 4};

static const char *fixture_source_dir(void) {
  static char path[MAX_STRING_LEN];
  snprintf(path, sizeof(path), "simulations/%s/_tests/data", MIMIC_COMPILED_SIMULATION);
  return path;
}

/* ---------------------------------------------------------------------------
 * Scratch fixture staging
 * ------------------------------------------------------------------------- */

static void snapshot_path(char *buf, size_t size, const char *dir, int snap) {
  snprintf(buf, size, "%s/snapshot_%03d.h5", dir, snap);
}

static int copy_file(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  if (in == NULL) {
    return -1;
  }
  FILE *out = fopen(dst, "wb");
  if (out == NULL) {
    fclose(in);
    return -1;
  }

  char buffer[65536];
  size_t n;
  int rc = 0;
  while ((n = fread(buffer, 1, sizeof(buffer), in)) > 0) {
    if (fwrite(buffer, 1, n, out) != n) {
      rc = -1;
      break;
    }
  }
  if (ferror(in)) {
    rc = -1;
  }
  if (fclose(out) != 0) {
    rc = -1;
  }
  fclose(in);
  return rc;
}

/** @brief Copy the committed fixture into a fresh scratch directory. */
static int stage_fixture(char *dir, size_t dir_size) {
  char template_path[] = "/tmp/mimic_snapshot_reader_XXXXXX";
  char *made = mkdtemp(template_path);
  if (made == NULL) {
    return -1;
  }
  snprintf(dir, dir_size, "%s", made);

  char src[MAX_STRING_LEN];
  char dst[MAX_STRING_LEN];
  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    snapshot_path(src, sizeof(src), fixture_source_dir(), snap);
    snapshot_path(dst, sizeof(dst), dir, snap);
    if (copy_file(src, dst) != 0) {
      return -1;
    }
  }

  snprintf(src, sizeof(src), "%s/%s", fixture_source_dir(), FIXTURE_A_LIST);
  snprintf(dst, sizeof(dst), "%s/%s", dir, FIXTURE_A_LIST);
  return copy_file(src, dst);
}

static void remove_staged_fixture(const char *dir) {
  char path[MAX_STRING_LEN];
  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    snapshot_path(path, sizeof(path), dir, snap);
    unlink(path);
  }
  snprintf(path, sizeof(path), "%s/%s", dir, FIXTURE_A_LIST);
  unlink(path);
  rmdir(dir);
}

/**
 * @brief   Point MimicConfig at a staged fixture directory.
 *
 * Unit tests never call init(), so the configuration fields the reader consumes
 * are set directly (the tests/unit/test_tree_reader_counts.c pattern). The
 * snapshot list is loaded with the production reader so the scale factors the
 * reader compares against are exactly the ones a real run would hold.
 */
static void configure_for_fixture(const char *dir) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s", dir);
  snprintf(MimicConfig.FileWithSnapList, sizeof(MimicConfig.FileWithSnapList), "%s/%s", dir,
           FIXTURE_A_LIST);
  read_snap_list();
  MimicConfig.MAXSNAPS = MimicConfig.Snaplistlen;
}

/* ---------------------------------------------------------------------------
 * Fixture mutation helpers (HDF5 C API)
 *
 * Each returns 0 on success and -1 on failure; the calling test asserts on the
 * return value so a broken mutation can never masquerade as a passing abort.
 * ------------------------------------------------------------------------- */

static int write_scalar_attr(const char *file_path, const char *name, hid_t type,
                             const void *value) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t group = H5Gopen2(file, "/header", H5P_DEFAULT);
  int rc = 0;
  if (group < 0) {
    rc = -1;
  } else {
    hid_t attr = H5Aopen(group, name, H5P_DEFAULT);
    if (attr < 0 || H5Awrite(attr, type, value) < 0) {
      rc = -1;
    }
    if (attr >= 0) {
      H5Aclose(attr);
    }
    H5Gclose(group);
  }
  H5Fclose(file);
  return rc;
}

static int set_attr_i32(const char *file_path, const char *name, int32_t value) {
  return write_scalar_attr(file_path, name, H5T_NATIVE_INT32, &value);
}

static int set_attr_i64(const char *file_path, const char *name, int64_t value) {
  return write_scalar_attr(file_path, name, H5T_NATIVE_INT64, &value);
}

static int set_attr_f64(const char *file_path, const char *name, double value) {
  return write_scalar_attr(file_path, name, H5T_NATIVE_DOUBLE, &value);
}

/** @brief Set a run-scoped header attribute in every fixture file. */
static int set_attr_i64_all(const char *dir, const char *name, int64_t value) {
  char path[MAX_STRING_LEN];
  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    snapshot_path(path, sizeof(path), dir, snap);
    if (set_attr_i64(path, name, value) != 0) {
      return -1;
    }
  }
  return 0;
}

static int delete_header_attr(const char *file_path, const char *name) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t group = H5Gopen2(file, "/header", H5P_DEFAULT);
  int rc = 0;
  if (group < 0 || H5Adelete(group, name) < 0) {
    rc = -1;
  }
  if (group >= 0) {
    H5Gclose(group);
  }
  H5Fclose(file);
  return rc;
}

/** @brief Replace a header attribute with one of a different (wrong) dtype. */
static int retype_header_attr(const char *file_path, const char *name, hid_t file_type,
                              hid_t mem_type, const void *value) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t group = H5Gopen2(file, "/header", H5P_DEFAULT);
  int rc = 0;
  if (group < 0 || H5Adelete(group, name) < 0) {
    rc = -1;
  } else {
    hid_t space = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(group, name, file_type, space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr < 0 || H5Awrite(attr, mem_type, value) < 0) {
      rc = -1;
    }
    if (attr >= 0) {
      H5Aclose(attr);
    }
    H5Sclose(space);
  }
  if (group >= 0) {
    H5Gclose(group);
  }
  H5Fclose(file);
  return rc;
}

static int delete_halo_dataset(const char *file_path, const char *dataset) {
  char link[MAX_STRING_LEN];
  snprintf(link, sizeof(link), "/halos/%s", dataset);

  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  const int rc = H5Ldelete(file, link, H5P_DEFAULT) < 0 ? -1 : 0;
  H5Fclose(file);
  return rc;
}

/**
 * @brief   Create a chunked rank-2 /halos dataset with a huge second dimension.
 *
 * Chunked so the enormous logical extent costs no storage: nothing is written,
 * so no chunk is allocated. This is the shape that a narrowing `(int)dims[1]`
 * comparison would accept, because 2^32 + 3 truncates to 3.
 */
static int create_wide_halo_dataset(const char *file_path, const char *dataset, hid_t type,
                                    hsize_t rows, hsize_t cols) {
  char link[MAX_STRING_LEN];
  snprintf(link, sizeof(link), "/halos/%s", dataset);

  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  const hsize_t dims[2] = {rows, cols};
  const hsize_t chunk[2] = {1, 1};
  hid_t space = H5Screate_simple(2, dims, NULL);
  hid_t plist = H5Pcreate(H5P_DATASET_CREATE);
  int rc = 0;
  if (space < 0 || plist < 0 || H5Pset_chunk(plist, 2, chunk) < 0) {
    rc = -1;
  } else {
    hid_t dset = H5Dcreate2(file, link, type, space, H5P_DEFAULT, plist, H5P_DEFAULT);
    if (dset < 0) {
      rc = -1;
    } else {
      H5Dclose(dset);
    }
  }
  if (plist >= 0) {
    H5Pclose(plist);
  }
  if (space >= 0) {
    H5Sclose(space);
  }
  H5Fclose(file);
  return rc;
}

/** @brief Create a /halos dataset with the given type and shape. */
static int create_halo_dataset(const char *file_path, const char *dataset, hid_t type, int rank,
                               hsize_t rows, hsize_t cols) {
  char link[MAX_STRING_LEN];
  snprintf(link, sizeof(link), "/halos/%s", dataset);

  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hsize_t dims[2] = {rows, cols};
  hid_t space = H5Screate_simple(rank, dims, NULL);
  hid_t dset = H5Dcreate2(file, link, type, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  const int rc = dset < 0 ? -1 : 0;
  if (dset >= 0) {
    H5Dclose(dset);
  }
  H5Sclose(space);
  H5Fclose(file);
  return rc;
}

/** @brief Overwrite one element of a rank-1 int32 /halos dataset. */
static int set_i32_element(const char *file_path, const char *dataset, hsize_t index,
                           int32_t value) {
  char link[MAX_STRING_LEN];
  snprintf(link, sizeof(link), "/halos/%s", dataset);

  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t dset = H5Dopen2(file, link, H5P_DEFAULT);
  int rc = 0;
  if (dset < 0) {
    rc = -1;
  } else {
    hid_t fspace = H5Dget_space(dset);
    const hsize_t start[1] = {index};
    const hsize_t count[1] = {1};
    hid_t mspace = H5Screate_simple(1, count, NULL);
    if (H5Sselect_hyperslab(fspace, H5S_SELECT_SET, start, NULL, count, NULL) < 0 ||
        H5Dwrite(dset, H5T_NATIVE_INT32, mspace, fspace, H5P_DEFAULT, &value) < 0) {
      rc = -1;
    }
    H5Sclose(mspace);
    H5Sclose(fspace);
    H5Dclose(dset);
  }
  H5Fclose(file);
  return rc;
}

/* ---------------------------------------------------------------------------
 * Child-process abort harness
 * ------------------------------------------------------------------------- */

typedef void (*child_body_fn)(const char *dir);

/**
 * @brief   Run `body` in a forked child and require it to abort.
 * @return  1 when the child exited non-zero and its stderr contained both
 *          needles (NULL needles are not required), 0 otherwise, -1 on a
 *          harness failure.
 *
 * On mismatch the captured output is printed, so a wrong abort message is
 * diagnosable rather than a bare failure.
 */
static int expect_fatal(const char *dir, child_body_fn body, const char *needle_a,
                        const char *needle_b) {
  int pipefd[2];
  char output[16384];
  size_t used = 0;
  ssize_t nread;
  int status;

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
    if (freopen("/dev/null", "w", stdout) == NULL) {
      _exit(127);
    }
    dup2(pipefd[1], STDERR_FILENO);
    close(pipefd[1]);
    body(dir);
    /* The body was supposed to abort. Exit 0 so the parent reports a failure. */
    _exit(0);
  }

  close(pipefd[1]);
  while (used < sizeof(output) - 1 &&
         (nread = read(pipefd[0], output + used, sizeof(output) - 1 - used)) > 0) {
    used += (size_t)nread;
  }
  output[used] = '\0';
  close(pipefd[0]);

  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFSIGNALED(status)) {
    fprintf(stderr, "  child died on signal %d; captured output:\n%s\n", WTERMSIG(status), output);
    return 0;
  }
  if (!WIFEXITED(status) || WEXITSTATUS(status) == 0) {
    fprintf(stderr, "  child did not abort; captured output:\n%s\n", output);
    return 0;
  }
  if ((needle_a != NULL && strstr(output, needle_a) == NULL) ||
      (needle_b != NULL && strstr(output, needle_b) == NULL)) {
    fprintf(stderr, "  abort message missing an expected fragment\n");
    fprintf(stderr, "    wanted: '%s' and '%s'\n", needle_a != NULL ? needle_a : "(any)",
            needle_b != NULL ? needle_b : "(any)");
    fprintf(stderr, "    got:\n%s\n", output);
    return 0;
  }
  return 1;
}

/** @brief Child body: configure for `dir` and open the dataset. */
static void child_open_run(const char *dir) {
  struct SnapshotRunInfo info;
  configure_for_fixture(dir);
  snapshot_reader_open_run(snapshot_reader_lookup("snapshot_hdf5"), &info);
}

/* ---------------------------------------------------------------------------
 * Corrupt-input case table
 * ------------------------------------------------------------------------- */

struct corrupt_case {
  const char *description;
  int (*mutate)(const char *dir);
  const char *needle_file;   /* fragment naming the offending file, or NULL */
  const char *needle_detail; /* fragment naming the offending object or value */
};

static int corrupt_format_version(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 2);
  return set_attr_i32(path, "format_version", 99);
}

static int corrupt_links_adjacent(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 3);
  return set_attr_i32(path, "links_adjacent", 0);
}

static int corrupt_snapshot_number(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return set_attr_i32(path, "snapshot_number", 7);
}

static int corrupt_n_halos_vs_dataset(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return set_attr_i64(path, "n_halos", 5);
}

static int corrupt_n_halos_negative(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 1);
  return set_attr_i64(path, "n_halos", -1);
}

static int corrupt_n_forests_total_mismatch(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 3);
  return set_attr_i64(path, "n_forests_total", 4);
}

static int corrupt_max_rank_mismatch(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 2);
  return set_attr_i64(path, "max_halo_rank_in_forest", 9);
}

static int corrupt_missing_snapshot_file(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 3);
  return unlink(path);
}

static int corrupt_missing_halo_dataset(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return delete_halo_dataset(path, "Vmax");
}

static int corrupt_extra_halo_dataset(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return create_halo_dataset(path, "Bogus", H5T_STD_I32LE, 1, 6, 0);
}

static int corrupt_halo_dataset_dtype(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  if (delete_halo_dataset(path, "Len") != 0) {
    return -1;
  }
  return create_halo_dataset(path, "Len", H5T_IEEE_F64LE, 1, 6, 0);
}

static int corrupt_vector_dataset_shape(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  if (delete_halo_dataset(path, "Pos") != 0) {
    return -1;
  }
  return create_halo_dataset(path, "Pos", H5T_IEEE_F32LE, 2, 6, 4);
}

static int corrupt_vector_dataset_wide_shape(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  if (delete_halo_dataset(path, "Pos") != 0) {
    return -1;
  }
  /* 2^32 + 3: truncates to exactly 3 in an int, so only a wide comparison
     rejects it. */
  return create_wide_halo_dataset(path, "Pos", H5T_IEEE_F32LE, 6, 4294967299ULL);
}

static int corrupt_missing_header_attr(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 2);
  return delete_header_attr(path, "hubble_h");
}

static int corrupt_header_attr_dtype(const char *dir) {
  char path[MAX_STRING_LEN];
  int32_t value = 1;
  snapshot_path(path, sizeof(path), dir, 1);
  return retype_header_attr(path, "n_halos", H5T_STD_I32LE, H5T_NATIVE_INT32, &value);
}

static int corrupt_scale_factor(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 2);
  /* One ULP away: the producer compares exactly, so the reader must too. */
  return set_attr_f64(path, "scale_factor", nextafter(0.5, 1.0));
}

static int corrupt_snapnum_value(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 5);
  return set_i32_element(path, "SnapNum", 0, 3);
}

static int corrupt_max_rank_too_large(const char *dir) {
  return set_attr_i64_all(dir, "max_halo_rank_in_forest", 9);
}

static int corrupt_max_rank_too_small(const char *dir) {
  return set_attr_i64_all(dir, "max_halo_rank_in_forest", 2);
}

static int corrupt_n_forests_total_vs_data(const char *dir) {
  return set_attr_i64_all(dir, "n_forests_total", 5);
}

static int corrupt_shape_and_data(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  /* Both a structural defect and a data defect in one file: the structural one
     must be reported, proving structure is validated before any bulk read. */
  if (set_i32_element(path, "SnapNum", 0, 99) != 0) {
    return -1;
  }
  if (delete_halo_dataset(path, "Pos") != 0) {
    return -1;
  }
  return create_halo_dataset(path, "Pos", H5T_IEEE_F32LE, 2, 6, 4);
}

static const struct corrupt_case CORRUPT_CASES[] = {
    {"unsupported format_version", corrupt_format_version, "snapshot_002.h5",
     "'format_version' is 99"},
    {"links_adjacent != 1", corrupt_links_adjacent, "snapshot_003.h5", "'links_adjacent' is 0"},
    {"snapshot_number disagrees with the filename", corrupt_snapshot_number, "snapshot_004.h5",
     "'snapshot_number' is 7"},
    {"n_halos disagrees with a dataset length", corrupt_n_halos_vs_dataset, "snapshot_004.h5",
     "has length 6 but header n_halos is 5"},
    {"negative n_halos", corrupt_n_halos_negative, "snapshot_001.h5", "'n_halos' is -1"},
    {"n_forests_total differs between files", corrupt_n_forests_total_mismatch, "snapshot_003.h5",
     "'n_forests_total' is 4 but snapshot 0 declares 3"},
    {"max_halo_rank_in_forest differs between files", corrupt_max_rank_mismatch, "snapshot_002.h5",
     "'max_halo_rank_in_forest' is 9 but snapshot 0 declares 6"},
    {"missing snapshot file", corrupt_missing_snapshot_file, "snapshot_003.h5",
     "no readable file for configured snapshot 3"},
    {"missing /halos dataset", corrupt_missing_halo_dataset, "snapshot_004.h5",
     "required dataset '/halos/Vmax' is missing"},
    {"extra /halos dataset", corrupt_extra_halo_dataset, "snapshot_004.h5",
     "unexpected dataset '/halos/Bogus'"},
    {"/halos dataset of the wrong dtype", corrupt_halo_dataset_dtype, "snapshot_004.h5",
     "dataset '/halos/Len' must be int32"},
    {"vector dataset of shape [n_halos, 4]", corrupt_vector_dataset_shape, "snapshot_004.h5",
     "dataset '/halos/Pos' must have shape [6, 3]"},
    {"vector dataset whose second dimension truncates to 3 in an int",
     corrupt_vector_dataset_wide_shape, "snapshot_004.h5", "found second dimension 4294967299"},
    {"missing header attribute", corrupt_missing_header_attr, "snapshot_002.h5",
     "required header attribute 'hubble_h' is missing"},
    {"header attribute of the wrong dtype", corrupt_header_attr_dtype, "snapshot_001.h5",
     "header attribute 'n_halos' must be int64"},
    {"scale_factor disagrees with the a_list", corrupt_scale_factor, "snapshot_002.h5",
     "header attribute 'scale_factor' is"},
    {"SnapNum disagrees with the header", corrupt_snapnum_value, "snapshot_005.h5",
     "'/halos/SnapNum' is 3 at halo 0"},
    {"max_halo_rank_in_forest above the measured maximum", corrupt_max_rank_too_large, NULL,
     "declares max_halo_rank_in_forest 9 but the measured maximum"},
    {"max_halo_rank_in_forest below the measured maximum", corrupt_max_rank_too_small, NULL,
     "declares max_halo_rank_in_forest 2 but the measured maximum"},
    {"n_forests_total disagrees with the measured ForestIndex range",
     corrupt_n_forests_total_vs_data, NULL, "declares n_forests_total 5 but the measured maximum"},
    {"structural defect is reported before any bulk read", corrupt_shape_and_data,
     "snapshot_004.h5", "dataset '/halos/Pos' must have shape [6, 3]"},
};
#define CORRUPT_CASE_COUNT (sizeof(CORRUPT_CASES) / sizeof(CORRUPT_CASES[0]))

/* ---------------------------------------------------------------------------
 * Tests
 * ------------------------------------------------------------------------- */

/**
 * @test  test_registry_lookup
 * Resolves snapshot_hdf5 case-insensitively and rejects unknown and NULL names.
 */
int test_registry_lookup(void) {
  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");

  TEST_ASSERT(reader != NULL, "snapshot_hdf5 should be registered in an HDF5 build");
  TEST_ASSERT_STRING_EQUAL(reader->name, "snapshot_hdf5", "reader should carry its registry name");
  TEST_ASSERT_EQUAL(reader->processing_order, INPUT_PROCESSING_ORDER_SNAPSHOT,
                    "snapshot_hdf5 should feed the snapshot-ordered driver");
  TEST_ASSERT(snapshot_reader_lookup("SNAPSHOT_HDF5") == reader,
              "lookup should be case-insensitive");
  TEST_ASSERT(snapshot_reader_lookup("Snapshot_HDF5") == reader,
              "lookup should be case-insensitive for mixed case");
  TEST_ASSERT(snapshot_reader_lookup("no_such_reader") == NULL,
              "an unknown reader name should not resolve");
  TEST_ASSERT(snapshot_reader_lookup(NULL) == NULL, "a NULL reader name should not resolve");

  return TEST_PASS;
}

/**
 * @test  test_open_run_publishes_run_metadata
 * open_run publishes the fixture's run metadata and per-snapshot halo counts.
 */
int test_open_run_publishes_run_metadata(void) {
  char dir[MAX_STRING_LEN];
  struct SnapshotRunInfo info;

  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  configure_for_fixture(dir);
  TEST_ASSERT_EQUAL(MimicConfig.Snaplistlen, FIXTURE_SNAPSHOTS,
                    "the fixture snapshot list should hold six entries");

  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  TEST_ASSERT(reader != NULL, "snapshot_hdf5 should be registered");

  snapshot_reader_open_run(reader, &info);

  TEST_ASSERT_EQUAL(info.snapshot_count, FIXTURE_SNAPSHOTS,
                    "run info should publish the snapshot count");
  TEST_ASSERT_EQUAL(info.format_version, FIXTURE_FORMAT_VERSION,
                    "run info should publish the on-disk format version");
  TEST_ASSERT_EQUAL(info.n_forests_total, FIXTURE_N_FORESTS_TOTAL,
                    "run info should publish n_forests_total");
  TEST_ASSERT_EQUAL(info.max_halo_rank_in_forest, FIXTURE_MAX_RANK,
                    "run info should publish max_halo_rank_in_forest");

  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    TEST_ASSERT_EQUAL(snapshot_reader_halo_count(reader, snap), FIXTURE_HALO_COUNTS[snap],
                      "snapshot_halo_count should match the fixture header");
  }

  snapshot_reader_close_run(reader);
  remove_staged_fixture(dir);
  return TEST_PASS;
}

static void child_count_below_range(const char *dir) {
  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  struct SnapshotRunInfo info;
  configure_for_fixture(dir);
  snapshot_reader_open_run(reader, &info);
  (void)snapshot_reader_halo_count(reader, -1);
}

static void child_count_above_range(const char *dir) {
  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  struct SnapshotRunInfo info;
  configure_for_fixture(dir);
  snapshot_reader_open_run(reader, &info);
  (void)snapshot_reader_halo_count(reader, info.snapshot_count);
}

/**
 * @test  test_snapshot_halo_count_range
 * snapshot_halo_count aborts outside [0, snapshot_count).
 */
int test_snapshot_halo_count_range(void) {
  char dir[MAX_STRING_LEN];
  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");

  TEST_ASSERT(expect_fatal(dir, child_count_below_range, "snapshot -1 is outside", "[0, 6)") == 1,
              "snapshot_halo_count(-1) should abort");
  TEST_ASSERT(expect_fatal(dir, child_count_above_range, "snapshot 6 is outside", "[0, 6)") == 1,
              "snapshot_halo_count(snapshot_count) should abort");

  remove_staged_fixture(dir);
  return TEST_PASS;
}

/**
 * @test  test_corrupt_inputs_abort
 * Every corrupt dataset the format forbids aborts, naming the file and the
 * offending object, attribute or value. None is silently repaired.
 */
int test_corrupt_inputs_abort(void) {
  for (size_t i = 0; i < CORRUPT_CASE_COUNT; i++) {
    const struct corrupt_case *test_case = &CORRUPT_CASES[i];
    char dir[MAX_STRING_LEN];

    TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
    if (test_case->mutate(dir) != 0) {
      fprintf(stderr, "  could not apply corruption: %s\n", test_case->description);
      remove_staged_fixture(dir);
      TEST_ASSERT(0, "fixture corruption helper failed");
    }

    const int aborted =
        expect_fatal(dir, child_open_run, test_case->needle_file, test_case->needle_detail);
    remove_staged_fixture(dir);

    if (aborted != 1) {
      fprintf(stderr, "  case: %s\n", test_case->description);
      TEST_ASSERT(0, "corrupt dataset should abort with a naming message");
    }
  }
  return TEST_PASS;
}

/* A deliberately incomplete reader: registered name, no hooks. Used to prove
   the dispatchers check at their point of use rather than dereferencing NULL. */
static const struct SnapshotReader IncompleteReader = {
    .name = "incomplete_test_reader",
    .processing_order = INPUT_PROCESSING_ORDER_SNAPSHOT,
    .open_run = NULL,
    .close_run = NULL,
    .snapshot_halo_count = NULL,
    .load_slab = NULL,
    .release_slab = NULL,
};

static void child_missing_open_run(const char *dir) {
  struct SnapshotRunInfo info;
  (void)dir;
  snapshot_reader_open_run(&IncompleteReader, &info);
}

static void child_missing_close_run(const char *dir) {
  (void)dir;
  snapshot_reader_close_run(&IncompleteReader);
}

static void child_missing_halo_count(const char *dir) {
  (void)dir;
  (void)snapshot_reader_halo_count(&IncompleteReader, 0);
}

static void child_missing_load_slab(const char *dir) {
  struct SnapshotSlab slab = snapshot_slab_empty();
  (void)dir;
  snapshot_reader_load_slab(&IncompleteReader, 0, &slab);
}

static void child_missing_release_slab(const char *dir) {
  struct SnapshotSlab slab = snapshot_slab_empty();
  (void)dir;
  snapshot_reader_release_slab(&IncompleteReader, &slab);
}

/**
 * @test  test_missing_hook_aborts
 * Each dispatcher aborts by name when its hook is NULL.
 */
int test_missing_hook_aborts(void) {
  static const struct {
    child_body_fn body;
    const char *hook;
  } cases[] = {
      {child_missing_open_run, "'open_run'"},
      {child_missing_close_run, "'close_run'"},
      {child_missing_halo_count, "'snapshot_halo_count'"},
      {child_missing_load_slab, "'load_slab'"},
      {child_missing_release_slab, "'release_slab'"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const int aborted = expect_fatal(NULL, cases[i].body, "incomplete_test_reader", cases[i].hook);
    if (aborted != 1) {
      fprintf(stderr, "  hook: %s\n", cases[i].hook);
      TEST_ASSERT(0, "a NULL hook should abort naming that hook");
    }
  }
  return TEST_PASS;
}

/**
 * @test  test_open_close_leaves_no_leak
 * open_run followed by close_run releases every tracked allocation.
 *
 * check_memory_leaks() only logs, so the log is captured and inspected here;
 * it is then re-emitted so the unit-run output carries the same verdict.
 */
int test_open_close_leaves_no_leak(void) {
  char dir[MAX_STRING_LEN];
  struct SnapshotRunInfo info;

  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  configure_for_fixture(dir);

  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  snapshot_reader_open_run(reader, &info);
  snapshot_reader_close_run(reader);
  remove_staged_fixture(dir);

  char log_template[] = "/tmp/mimic_snapshot_leak_XXXXXX";
  const int fd = mkstemp(log_template);
  TEST_ASSERT(fd >= 0, "should create a scratch log file");
  FILE *log = fdopen(fd, "w+");
  TEST_ASSERT(log != NULL, "should open the scratch log file");

  FILE *previous = set_log_output(log);
  check_memory_leaks();
  set_log_output(previous);
  fflush(log);

  rewind(log);
  char captured[4096];
  const size_t read_bytes = fread(captured, 1, sizeof(captured) - 1, log);
  captured[read_bytes] = '\0';
  fclose(log);
  unlink(log_template);

  TEST_ASSERT(strstr(captured, "Memory leak detected") == NULL,
              "open_run followed by close_run should leave no tracked allocation");

  /* Re-emit for the captured unit-run output. */
  check_memory_leaks();
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Snapshot Reader Open\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_registry_lookup);
  TEST_RUN(test_open_run_publishes_run_metadata);
  TEST_RUN(test_snapshot_halo_count_range);
  TEST_RUN(test_corrupt_inputs_abort);
  TEST_RUN(test_missing_hook_aborts);
  TEST_RUN(test_open_close_leaves_no_leak);

  TEST_SUMMARY();
  return TEST_RESULT();
}
