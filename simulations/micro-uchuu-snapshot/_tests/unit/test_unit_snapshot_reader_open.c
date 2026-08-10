/**
 * @file    test_unit_snapshot_reader_open.c
 * @brief   Unit tests for the snapshot_hdf5 reader's full lifecycle.
 *
 * Validates: registry lookup, the run metadata and per-snapshot counts
 * published by open_run against the committed fixture dataset, the
 * out-of-range count contract, every corrupt-input abort the format requires,
 * the missing-hook diagnostic, slab contents against the fixture datasets
 * field by field, every link-range abort, the bounded shape of the load-path
 * diagnostics, the slab lifecycle contract, and freedom from leaks.
 *
 * The file keeps its `_open` name because tests/unit/run_tests.sh lists test
 * names explicitly for the non-HDF5 skip path; splitting the slab tests into a
 * second file would require an edit there.
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

#include "../../../../src/include/constants.h"
#include "../../../../src/include/proto.h"
#include "../../../../src/include/types.h"
#include "../../../../src/io/snapshot/reader.h"
#include "../../../../src/util/error.h"
#include "../../../../src/util/memory.h"

#include <hdf5.h>

#include <errno.h>
#include <float.h>
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

/* Largest halo count in the fixture; sizes the comparison buffers below. */
#define FIXTURE_MAX_HALOS 6

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
 * @brief   Copy the committed fixture into a fresh scratch directory.
 *
 * A failed copy partway through leaves some files staged and others not; every
 * failure path removes them with remove_staged_fixture() before returning, so
 * a broken copy never leaks the mkdtemp() scratch directory.
 */
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
      remove_staged_fixture(dir);
      return -1;
    }
  }

  snprintf(src, sizeof(src), "%s/%s", fixture_source_dir(), FIXTURE_A_LIST);
  snprintf(dst, sizeof(dst), "%s/%s", dir, FIXTURE_A_LIST);
  if (copy_file(src, dst) != 0) {
    remove_staged_fixture(dir);
    return -1;
  }
  return 0;
}

/* The physical values simulations/micro-uchuu-snapshot/simulation_info.yaml
   declares. The fixture's headers were stamped from these by the converter
   (create_snapshot_fixture.py), so open_run must see the same values to pass
   the unmodified fixture by construction. PartMass is carried in 1e10 Msun/h,
   the units simulation_info.yaml declares; open_run multiplies it up by 1e10
   before comparing against particle_mass_msun_h. */
#define FIXTURE_BOX_SIZE 100.0
#define FIXTURE_OMEGA_MATTER 0.3089
#define FIXTURE_OMEGA_LAMBDA 0.6911
#define FIXTURE_HUBBLE_H 0.6774
#define FIXTURE_PART_MASS 0.0325

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
  /* open_run checks the run-scoped identity bounds against this. A real run gets
     it from simulation.unique_galaxy_id_multiplier, which defaults to
     TREE_MUL_FAC; a memset MimicConfig would leave it at zero. */
  MimicConfig.UniqueGalaxyIDMultiplier = (int64_t)TREE_MUL_FAC;
  /* open_run now compares these against the header on every file (Slice 3);
     the fixture's headers were stamped from the package's own
     simulation_info.yaml, so these are that package's values. */
  MimicConfig.BoxSize = FIXTURE_BOX_SIZE;
  MimicConfig.Omega = FIXTURE_OMEGA_MATTER;
  MimicConfig.OmegaLambda = FIXTURE_OMEGA_LAMBDA;
  MimicConfig.Hubble_h = FIXTURE_HUBBLE_H;
  MimicConfig.PartMass = FIXTURE_PART_MASS;
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

/** @brief Read a scalar /header attribute back, so a mutation can be confirmed
    before it is exercised (e.g. that a perturbation did not round-trip away). */
static int read_attr_f64(const char *file_path, const char *name, double *out) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t group = H5Gopen2(file, "/header", H5P_DEFAULT);
  int rc = 0;
  if (group < 0) {
    rc = -1;
  } else {
    hid_t attr = H5Aopen(group, name, H5P_DEFAULT);
    if (attr < 0 || H5Aread(attr, H5T_NATIVE_DOUBLE, out) < 0) {
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

/** @brief Create a new scalar attribute on /header (the group must not already have one). */
static int add_header_attr(const char *file_path, const char *name, hid_t type, const void *value) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t group = H5Gopen2(file, "/header", H5P_DEFAULT);
  int rc = 0;
  if (group < 0) {
    rc = -1;
  } else {
    hid_t space = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate2(group, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr < 0 || H5Awrite(attr, type, value) < 0) {
      rc = -1;
    }
    if (attr >= 0) {
      H5Aclose(attr);
    }
    H5Sclose(space);
    H5Gclose(group);
  }
  H5Fclose(file);
  return rc;
}

/** @brief Create an empty group directly under the file root. */
static int create_root_group(const char *file_path, const char *name) {
  char link[MAX_STRING_LEN];
  snprintf(link, sizeof(link), "/%s", name);

  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t group = H5Gcreate2(file, link, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  const int rc = group < 0 ? -1 : 0;
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

/** @brief Overwrite one element of a rank-1 int64 /halos dataset. */
static int set_i64_element(const char *file_path, const char *dataset, hsize_t index,
                           int64_t value) {
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
        H5Dwrite(dset, H5T_NATIVE_INT64, mspace, fspace, H5P_DEFAULT, &value) < 0) {
      rc = -1;
    }
    H5Sclose(mspace);
    H5Sclose(fspace);
    H5Dclose(dset);
  }
  H5Fclose(file);
  return rc;
}

/** @brief Set every element of a rank-1 int32 /halos dataset. */
static int set_i32_column(const char *file_path, const char *dataset, int64_t n, int32_t value) {
  for (int64_t i = 0; i < n; i++) {
    if (set_i32_element(file_path, dataset, (hsize_t)i, value) != 0) {
      return -1;
    }
  }
  return 0;
}

/**
 * @brief   Give the staged fixture a populated snapshot 0.
 *
 * The committed fixture's snapshot 0 is empty, so it cannot by itself exercise
 * "a non-null FirstProgenitor in snapshot 0". Copying snapshot 1 over it and
 * re-stamping the two snapshot-identifying values produces a dataset that
 * open_run accepts and whose snapshot 0 holds one halo. The run-scoped forest
 * and rank maxima are unaffected: the duplicated halo introduces no new maximum
 * and the true maxima are attained in later snapshots.
 */
static int promote_snapshot_zero(const char *dir) {
  char src[MAX_STRING_LEN];
  char dst[MAX_STRING_LEN];
  snapshot_path(src, sizeof(src), dir, 1);
  snapshot_path(dst, sizeof(dst), dir, 0);

  if (copy_file(src, dst) != 0) {
    return -1;
  }
  if (set_attr_i32(dst, "snapshot_number", 0) != 0) {
    return -1;
  }
  /* First entry of the committed micro-uchuu-fixture.a_list. A drift here
     surfaces as an open_run scale_factor abort, never as a silent pass. */
  if (set_attr_f64(dst, "scale_factor", 0.25) != 0) {
    return -1;
  }
  return set_i32_element(dst, "SnapNum", 0, 0);
}

/* ---------------------------------------------------------------------------
 * Independent fixture reads
 *
 * Slab contents are compared against a direct read of the same file rather than
 * against transcribed constants, so the comparison follows the fixture and
 * covers every dataset. The read path here is deliberately a plain whole-
 * dataset H5Dread, not the reader's own.
 * ------------------------------------------------------------------------- */

static int read_halo_column(const char *file_path, const char *dataset, hid_t mem_type, void *out) {
  char link[MAX_STRING_LEN];
  snprintf(link, sizeof(link), "/halos/%s", dataset);

  hid_t file = H5Fopen(file_path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t dset = H5Dopen2(file, link, H5P_DEFAULT);
  int rc = 0;
  if (dset < 0) {
    rc = -1;
  } else {
    /* The comparison buffers below are hard-sized from FIXTURE_MAX_HALOS; a
       larger dataset extent would silently overflow them, so an unreadable or
       oversized extent fails the read (callers surface it as a test failure)
       with both handles closed rather than leaked. */
    hid_t space = H5Dget_space(dset);
    hsize_t dims[2] = {0, 0};
    if (space < 0 || H5Sget_simple_extent_dims(space, dims, NULL) < 0) {
      rc = -1;
    }
    if (space >= 0) {
      H5Sclose(space);
    }
    if (rc == 0 && dims[0] > (hsize_t)FIXTURE_MAX_HALOS) {
      fprintf(stderr,
              "  dataset '/halos/%s' has extent %" PRIu64 " but the fixed comparison buffer "
              "holds only %d; raise FIXTURE_MAX_HALOS\n",
              dataset, (uint64_t)dims[0], FIXTURE_MAX_HALOS);
      rc = -1;
    }

    if (rc == 0 && H5Dread(dset, mem_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, out) < 0) {
      rc = -1;
    }
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
static int expect_fatal_capture(const char *dir, child_body_fn body, const char *needle_a,
                                const char *needle_b, char *captured, size_t captured_size) {
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
  if (captured != NULL) {
    snprintf(captured, captured_size, "%s", output);
  }

  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (used == sizeof(output) - 1) {
    fprintf(stderr, "  child output truncated at %zu bytes; raise the capture buffer\n",
            sizeof(output) - 1);
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

/** @brief expect_fatal_capture() without access to the captured output. */
static int expect_fatal(const char *dir, child_body_fn body, const char *needle_a,
                        const char *needle_b) {
  return expect_fatal_capture(dir, body, needle_a, needle_b, NULL, 0);
}

/**
 * @brief   Run `body` in a forked child and require it to complete without
 *          aborting.
 * @return  1 when the child exited 0, 0 when it aborted or was signaled
 *          (captured output is printed for diagnosis), -1 on a harness
 *          failure.
 *
 * The mirror image of expect_fatal_capture(): used to pin the accepted side of
 * the physical-header tolerance, where open_run must run to completion.
 */
static int expect_success(const char *dir, child_body_fn body) {
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
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
    fprintf(stderr, "  child aborted; captured output:\n%s\n", output);
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

/** @brief Child body: configure for `dir`, open, and cleanly close. */
static void child_open_run_close(const char *dir) {
  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  struct SnapshotRunInfo info;
  configure_for_fixture(dir);
  snapshot_reader_open_run(reader, &info);
  snapshot_reader_close_run(reader);
}

/* ---------------------------------------------------------------------------
 * Corrupt-input case table
 * ------------------------------------------------------------------------- */

struct corrupt_case {
  const char *description;
  int (*mutate)(const char *dir);
  const char *needle_file;   /* fragment naming the offending file, or NULL */
  const char *needle_detail; /* fragment naming the offending object or value */
  /* Additional captured-output fragment to require, or NULL (the trailing
     struct member of every existing initializer below zero-initializes to
     NULL, so this is opt-in). needle_detail alone is a weak pin for the
     physical-header-mismatch cases: "'box_size_mpc_h' is" is also a substring
     of the missing-attribute message "required header attribute '%s' is
     missing". needle_extra pins the mismatch phrasing and, where practical,
     the exact printed text of both compared values, so a message that
     dropped the comparison detail (or fired for the wrong reason) would fail
     this needle even though needle_file/needle_detail still matched. */
  const char *needle_extra;
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

/* ---------------------------------------------------------------------------
 * Physical header agreement (Slice 3)
 *
 * These mutate the physical header attributes open_run now checks against the
 * configured simulation (MimicConfig.BoxSize/Omega/OmegaLambda/Hubble_h/
 * PartMass). Each mutates a single file; the check runs per file against the
 * same configured values, so one corrupted file is enough to trigger it.
 * ------------------------------------------------------------------------- */

static int corrupt_box_size(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 2);
  return set_attr_f64(path, "box_size_mpc_h", 90.0);
}

static int corrupt_omega_matter(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 3);
  return set_attr_f64(path, "omega_matter", 0.5);
}

static int corrupt_omega_lambda(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return set_attr_f64(path, "omega_lambda", 0.5);
}

static int corrupt_hubble_h(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 1);
  return set_attr_f64(path, "hubble_h", 0.5);
}

static int corrupt_particle_mass(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 2);
  return set_attr_f64(path, "particle_mass_msun_h", 4.0e8);
}

/**
 * @brief   The naive-comparison trap: particle_mass_msun_h set to the
 *          configured PartMass value with the 1e10 unit factor omitted.
 *
 * A reader that compared the header directly against MimicConfig.PartMass
 * (instead of MimicConfig.PartMass * 1e10) would accept this file. The
 * correct comparison must reject it: 0.0325 is nowhere near
 * 325000000.0 under a rounding tolerance.
 */
static int corrupt_particle_mass_missing_unit_factor(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 3);
  return set_attr_f64(path, "particle_mass_msun_h", FIXTURE_PART_MASS);
}

static int corrupt_box_size_nan(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return set_attr_f64(path, "box_size_mpc_h", NAN);
}

static int corrupt_hubble_h_infinite(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 1);
  return set_attr_f64(path, "hubble_h", INFINITY);
}

/** @brief The check must run on every file, not only snapshot 0: mutate the
    LAST fixture snapshot (index FIXTURE_SNAPSHOTS - 1). */
static int corrupt_box_size_last_snapshot(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, FIXTURE_SNAPSHOTS - 1);
  return set_attr_f64(path, "box_size_mpc_h", 90.0);
}

/**
 * @brief   Pin the tolerance from both sides: a 4 * DBL_EPSILON relative
 *          perturbation is inside it (accepted), a 1e-9 relative
 *          perturbation is outside it (rejected).
 *
 * 4 * DBL_EPSILON is a quarter of the frozen 16 * DBL_EPSILON tolerance;
 * 1e-9 is roughly six orders of magnitude beyond it, so both sides have
 * comfortable margin against arithmetic rounding in the perturbation itself.
 */
static int mutate_box_size_relative(const char *dir, int snap, double relative_perturbation) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, snap);
  return set_attr_f64(path, "box_size_mpc_h", FIXTURE_BOX_SIZE * (1.0 + relative_perturbation));
}

static int perturb_box_size_within_tolerance(const char *dir) {
  return mutate_box_size_relative(dir, 2, 4.0 * DBL_EPSILON);
}

static int perturb_box_size_beyond_tolerance(const char *dir) {
  return mutate_box_size_relative(dir, 2, 1e-9);
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

static int corrupt_n_forests_total_below_data(const char *dir) {
  return set_attr_i64_all(dir, "n_forests_total", 1);
}

static int corrupt_forest_index_negative(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return set_i64_element(path, "ForestIndex", 0, -1);
}

static int corrupt_halo_rank_negative(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return set_i64_element(path, "HaloRankInForest", 0, -1);
}

static int corrupt_extra_root_object(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 2);
  return create_root_group(path, "bogus");
}

static int corrupt_extra_header_attr(const char *dir) {
  char path[MAX_STRING_LEN];
  int32_t value = 0;
  snapshot_path(path, sizeof(path), dir, 1);
  return add_header_attr(path, "bogus_attr", H5T_NATIVE_INT32, &value);
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
    {"box_size_mpc_h disagrees with the configured BoxSize", corrupt_box_size, "snapshot_002.h5",
     "'box_size_mpc_h' is", "is 90 but the configured simulation value is 100"},
    {"omega_matter disagrees with the configured cosmology", corrupt_omega_matter,
     "snapshot_003.h5", "'omega_matter' is",
     "is 0.5 but the configured simulation value is 0.30890000000000001"},
    {"omega_lambda disagrees with the configured cosmology", corrupt_omega_lambda,
     "snapshot_004.h5", "'omega_lambda' is",
     "is 0.5 but the configured simulation value is 0.69110000000000005"},
    {"hubble_h disagrees with the configured cosmology", corrupt_hubble_h, "snapshot_001.h5",
     "'hubble_h' is", "is 0.5 but the configured simulation value is 0.6774"},
    {"particle_mass_msun_h disagrees with the configured PartMass * 1e10", corrupt_particle_mass,
     "snapshot_002.h5", "'particle_mass_msun_h' is",
     "is 400000000 but the configured simulation value is 325000000"},
    {"particle_mass_msun_h equals PartMass without the 1e10 factor (naive-comparison trap)",
     corrupt_particle_mass_missing_unit_factor, "snapshot_003.h5", "'particle_mass_msun_h' is",
     "is 0.032500000000000001 but the configured simulation value is 325000000"},
    {"NaN in a compared physical attribute", corrupt_box_size_nan, "snapshot_004.h5",
     "'box_size_mpc_h' is", "is nan but the configured simulation value is 100"},
    {"infinity in a compared physical attribute", corrupt_hubble_h_infinite, "snapshot_001.h5",
     "'hubble_h' is", "is inf but the configured simulation value is 0.6774"},
    {"physical header mismatch in the last snapshot, not only snapshot 0",
     corrupt_box_size_last_snapshot, "snapshot_005.h5", "'box_size_mpc_h' is",
     "is 90 but the configured simulation value is 100"},
    {"SnapNum disagrees with the header", corrupt_snapnum_value, "snapshot_005.h5",
     "'/halos/SnapNum' is 3 at halo 0"},
    {"max_halo_rank_in_forest above the measured maximum", corrupt_max_rank_too_large, NULL,
     "declares max_halo_rank_in_forest 9 but the measured maximum"},
    {"max_halo_rank_in_forest below the measured maximum", corrupt_max_rank_too_small, NULL,
     "declares max_halo_rank_in_forest 2 but the measured maximum"},
    {"n_forests_total disagrees with the measured ForestIndex range",
     corrupt_n_forests_total_vs_data, NULL, "declares n_forests_total 5 but the measured maximum"},
    {"n_forests_total below measured maximum", corrupt_n_forests_total_below_data,
     "snapshot_001.h5", "the permitted range is [0, 0]"},
    {"negative ForestIndex value", corrupt_forest_index_negative, "snapshot_004.h5",
     "'/halos/ForestIndex' is -1 at halo 0"},
    {"negative HaloRankInForest value", corrupt_halo_rank_negative, "snapshot_004.h5",
     "'/halos/HaloRankInForest' is -1 at halo 0"},
    {"unexpected root object", corrupt_extra_root_object, "snapshot_002.h5",
     "unexpected root object"},
    {"unexpected header attribute", corrupt_extra_header_attr, "snapshot_001.h5",
     "unexpected attribute"},
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
    char captured[16384];

    TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
    if (test_case->mutate(dir) != 0) {
      fprintf(stderr, "  could not apply corruption: %s\n", test_case->description);
      remove_staged_fixture(dir);
      TEST_ASSERT(0, "fixture corruption helper failed");
    }

    const int aborted = expect_fatal_capture(dir, child_open_run, test_case->needle_file,
                                             test_case->needle_detail, captured, sizeof(captured));
    remove_staged_fixture(dir);

    if (aborted != 1) {
      fprintf(stderr, "  case: %s\n", test_case->description);
      TEST_ASSERT(0, "corrupt dataset should abort with a naming message");
    }
    if (test_case->needle_extra != NULL && strstr(captured, test_case->needle_extra) == NULL) {
      fprintf(stderr, "  case: %s\n  wanted additionally: '%s'\n  got:\n%s\n",
              test_case->description, test_case->needle_extra, captured);
      TEST_ASSERT(0, "corrupt dataset abort message should also carry the expected detail");
    }
  }
  return TEST_PASS;
}

/**
 * @test  test_physical_value_tolerance_boundary
 * Pins the physical-header rounding tolerance from both sides: a
 * 4 * DBL_EPSILON relative perturbation is inside it and open_run succeeds; a
 * 1e-9 relative perturbation is outside it and open_run aborts.
 */
int test_physical_value_tolerance_boundary(void) {
  char dir[MAX_STRING_LEN];
  char path[MAX_STRING_LEN];
  double stored = 0.0;

  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  TEST_ASSERT(perturb_box_size_within_tolerance(dir) == 0,
              "should apply the within-tolerance perturbation");
  /* Confirm the perturbation is genuinely representable before relying on it:
     if it ever rounded back to the exact configured value, the acceptance
     below would prove nothing about the tolerance. */
  snapshot_path(path, sizeof(path), dir, 2);
  TEST_ASSERT(read_attr_f64(path, "box_size_mpc_h", &stored) == 0,
              "should read back the perturbed box_size_mpc_h");
  TEST_ASSERT(stored != FIXTURE_BOX_SIZE,
              "the within-tolerance perturbation must not round-trip back to the exact "
              "configured value");
  TEST_ASSERT(expect_success(dir, child_open_run_close) == 1,
              "a 4 * DBL_EPSILON relative perturbation should be accepted");
  remove_staged_fixture(dir);

  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  TEST_ASSERT(perturb_box_size_beyond_tolerance(dir) == 0,
              "should apply the beyond-tolerance perturbation");
  TEST_ASSERT(expect_fatal(dir, child_open_run, "snapshot_002.h5", "'box_size_mpc_h' is") == 1,
              "a 1e-9 relative perturbation should be rejected");
  remove_staged_fixture(dir);

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

/* ---------------------------------------------------------------------------
 * Slab loading
 * ------------------------------------------------------------------------- */

/**
 * @brief   Compare one loaded slab against a direct read of its fixture file.
 * @return  0 when every field of every halo matches, -1 otherwise.
 *
 * Every one of the sixteen contract datasets is checked: the three vec3 fields
 * component by component, MostBoundID as a struct RawHalo field, and the two
 * identity datasets against the slab's own forest_index/halo_rank_in_forest
 * arrays. Floats are compared by their bytes, so the check is bit-for-bit
 * rather than numeric.
 */
static int slab_matches_fixture(const char *file_path, const struct SnapshotSlab *slab) {
  static int32_t i32[FIXTURE_MAX_HALOS];
  static int64_t i64[FIXTURE_MAX_HALOS];
  static float f32[FIXTURE_MAX_HALOS * NDIM];
  const int64_t n = slab->nhalos;

#define CHECK_I32(dataset, field)                                                                  \
  do {                                                                                             \
    if (read_halo_column(file_path, dataset, H5T_NATIVE_INT32, i32) != 0) {                        \
      fprintf(stderr, "  could not read '/halos/%s'\n", dataset);                                  \
      return -1;                                                                                   \
    }                                                                                              \
    for (int64_t h = 0; h < n; h++) {                                                              \
      if (slab->halos[h].field != i32[h]) {                                                        \
        fprintf(stderr, "  %s halo %" PRId64 ": slab %d, fixture %" PRId32 "\n", dataset, h,       \
                slab->halos[h].field, i32[h]);                                                     \
        return -1;                                                                                 \
      }                                                                                            \
    }                                                                                              \
  } while (0)

#define CHECK_I64(dataset, field)                                                                  \
  do {                                                                                             \
    if (read_halo_column(file_path, dataset, H5T_NATIVE_INT64, i64) != 0) {                        \
      fprintf(stderr, "  could not read '/halos/%s'\n", dataset);                                  \
      return -1;                                                                                   \
    }                                                                                              \
    for (int64_t h = 0; h < n; h++) {                                                              \
      if ((int64_t)slab->halos[h].field != i64[h]) {                                               \
        fprintf(stderr, "  %s halo %" PRId64 ": slab %lld, fixture %" PRId64 "\n", dataset, h,     \
                slab->halos[h].field, i64[h]);                                                     \
        return -1;                                                                                 \
      }                                                                                            \
    }                                                                                              \
  } while (0)

#define CHECK_F32(dataset, field)                                                                  \
  do {                                                                                             \
    if (read_halo_column(file_path, dataset, H5T_NATIVE_FLOAT, f32) != 0) {                        \
      fprintf(stderr, "  could not read '/halos/%s'\n", dataset);                                  \
      return -1;                                                                                   \
    }                                                                                              \
    for (int64_t h = 0; h < n; h++) {                                                              \
      if (memcmp(&slab->halos[h].field, &f32[h], sizeof(float)) != 0) {                            \
        fprintf(stderr, "  %s halo %" PRId64 ": slab %.9g, fixture %.9g\n", dataset, h,            \
                (double)slab->halos[h].field, (double)f32[h]);                                     \
        return -1;                                                                                 \
      }                                                                                            \
    }                                                                                              \
  } while (0)

#define CHECK_VEC3(dataset, field)                                                                 \
  do {                                                                                             \
    if (read_halo_column(file_path, dataset, H5T_NATIVE_FLOAT, f32) != 0) {                        \
      fprintf(stderr, "  could not read '/halos/%s'\n", dataset);                                  \
      return -1;                                                                                   \
    }                                                                                              \
    for (int64_t h = 0; h < n; h++) {                                                              \
      for (int d = 0; d < NDIM; d++) {                                                             \
        if (memcmp(&slab->halos[h].field[d], &f32[h * NDIM + d], sizeof(float)) != 0) {            \
          fprintf(stderr, "  %s halo %" PRId64 "[%d]: slab %.9g, fixture %.9g\n", dataset, h, d,   \
                  (double)slab->halos[h].field[d], (double)f32[h * NDIM + d]);                     \
          return -1;                                                                               \
        }                                                                                          \
      }                                                                                            \
    }                                                                                              \
  } while (0)

/* ForestIndex and HaloRankInForest are reader-owned slab arrays, not
   struct RawHalo members (Slice 2), so they are compared against
   slab->array[h] directly rather than through the CHECK_I64 field-access
   pattern above. */
#define CHECK_I64_ARRAY(dataset, array)                                                            \
  do {                                                                                             \
    if (read_halo_column(file_path, dataset, H5T_NATIVE_INT64, i64) != 0) {                        \
      fprintf(stderr, "  could not read '/halos/%s'\n", dataset);                                  \
      return -1;                                                                                   \
    }                                                                                              \
    for (int64_t h = 0; h < n; h++) {                                                              \
      if (slab->array[h] != i64[h]) {                                                              \
        fprintf(stderr, "  %s halo %" PRId64 ": slab %" PRId64 ", fixture %" PRId64 "\n", dataset, \
                h, slab->array[h], i64[h]);                                                        \
        return -1;                                                                                 \
      }                                                                                            \
    }                                                                                              \
  } while (0)

  CHECK_I32("Descendant", Descendant);
  CHECK_I32("FirstProgenitor", FirstProgenitor);
  CHECK_I32("NextProgenitor", NextProgenitor);
  CHECK_I32("FirstHaloInFOFgroup", FirstHaloInFOFgroup);
  CHECK_I32("NextHaloInFOFgroup", NextHaloInFOFgroup);
  CHECK_I32("Len", Len);
  CHECK_I32("SnapNum", SnapNum);
  CHECK_F32("M_Crit200", M_Crit200);
  CHECK_F32("VelDisp", VelDisp);
  CHECK_F32("Vmax", Vmax);
  CHECK_VEC3("Pos", Pos);
  CHECK_VEC3("Vel", Vel);
  CHECK_VEC3("Spin", Spin);
  CHECK_I64("MostBoundID", MostBoundID);
  CHECK_I64_ARRAY("ForestIndex", forest_index);
  CHECK_I64_ARRAY("HaloRankInForest", halo_rank_in_forest);

#undef CHECK_I32
#undef CHECK_I64
#undef CHECK_I64_ARRAY
#undef CHECK_F32
#undef CHECK_VEC3
  return 0;
}

/**
 * @test  test_load_slab_matches_fixture
 * Every fixture snapshot loads with the header's halo count and field values
 * identical to the file's, and the empty snapshot loads and releases cleanly.
 */
int test_load_slab_matches_fixture(void) {
  char dir[MAX_STRING_LEN];
  struct SnapshotRunInfo info;

  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  configure_for_fixture(dir);

  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  snapshot_reader_open_run(reader, &info);

  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    char path[MAX_STRING_LEN];
    struct SnapshotSlab slab = snapshot_slab_empty();
    snapshot_path(path, sizeof(path), dir, snap);

    snapshot_reader_load_slab(reader, snap, &slab);

    TEST_ASSERT_EQUAL(slab.snapnum, snap, "a loaded slab should carry its snapshot number");
    TEST_ASSERT_EQUAL(slab.nhalos, FIXTURE_HALO_COUNTS[snap],
                      "slab nhalos should match the fixture header");
    TEST_ASSERT(!snapshot_slab_is_empty(&slab), "a loaded slab should not report itself empty");
    if (FIXTURE_HALO_COUNTS[snap] == 0) {
      TEST_ASSERT(slab.halos == NULL, "a zero-halo snapshot should allocate no halo array");
      TEST_ASSERT(slab.forest_index == NULL,
                  "a zero-halo snapshot should allocate no forest_index array");
      TEST_ASSERT(slab.halo_rank_in_forest == NULL,
                  "a zero-halo snapshot should allocate no halo_rank_in_forest array");
    } else {
      TEST_ASSERT(slab.halos != NULL, "a populated snapshot should carry a halo array");
      TEST_ASSERT(slab.forest_index != NULL,
                  "a populated snapshot should carry a forest_index array");
      TEST_ASSERT(slab.halo_rank_in_forest != NULL,
                  "a populated snapshot should carry a halo_rank_in_forest array");
      TEST_ASSERT(slab_matches_fixture(path, &slab) == 0,
                  "every slab field should equal the fixture bit-for-bit");
    }

    snapshot_reader_release_slab(reader, &slab);
    TEST_ASSERT(snapshot_slab_is_empty(&slab), "release_slab should empty the handle");
    TEST_ASSERT(slab.halos == NULL, "release_slab should clear the halo pointer");
    TEST_ASSERT(slab.forest_index == NULL, "release_slab should clear the forest_index pointer");
    TEST_ASSERT(slab.halo_rank_in_forest == NULL,
                "release_slab should clear the halo_rank_in_forest pointer");
    TEST_ASSERT_EQUAL(slab.nhalos, 0, "release_slab should clear the halo count");
  }

  snapshot_reader_close_run(reader);
  remove_staged_fixture(dir);
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Link-range abort cases
 * ------------------------------------------------------------------------- */

struct link_case {
  const char *description;
  int (*mutate)(const char *dir);
  int snapshot; /* snapshot the child loads */
  const char *needle_file;
  const char *needle_detail;
};

static int corrupt_first_progenitor_range(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  /* Snapshot 3 holds one halo, so the only valid non-null value is 0. */
  return set_i32_element(path, "FirstProgenitor", 0, 5);
}

static int corrupt_next_progenitor_range(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return set_i32_element(path, "NextProgenitor", 0, 6);
}

static int corrupt_first_fof_range(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return set_i32_element(path, "FirstHaloInFOFgroup", 0, 6);
}

static int corrupt_first_fof_null(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return set_i32_element(path, "FirstHaloInFOFgroup", 0, -1);
}

static int corrupt_next_fof_range(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  return set_i32_element(path, "NextHaloInFOFgroup", 1, 9);
}

static int corrupt_descendant_range(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 4);
  /* Snapshot 5 holds four halos, so 4 is one past the end. */
  return set_i32_element(path, "Descendant", 0, 4);
}

static int corrupt_descendant_in_final_snapshot(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, FIXTURE_SNAPSHOTS - 1);
  return set_i32_element(path, "Descendant", 0, 0);
}

static int corrupt_first_progenitor_in_snapshot_zero(const char *dir) {
  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), dir, 0);
  if (promote_snapshot_zero(dir) != 0) {
    return -1;
  }
  return set_i32_element(path, "FirstProgenitor", 0, 0);
}

static const struct link_case LINK_CASES[] = {
    {"FirstProgenitor outside [-1, n_halos(N-1))", corrupt_first_progenitor_range, 4,
     "snapshot_004.h5", "'FirstProgenitor' is 5 at halo 0, outside -1 or [0, 1)"},
    {"NextProgenitor outside [-1, n_halos(N))", corrupt_next_progenitor_range, 4, "snapshot_004.h5",
     "'NextProgenitor' is 6 at halo 0, outside -1 or [0, 6)"},
    {"FirstHaloInFOFgroup outside [0, n_halos(N))", corrupt_first_fof_range, 4, "snapshot_004.h5",
     "'FirstHaloInFOFgroup' is 6 at halo 0, outside [0, 6)"},
    {"FirstHaloInFOFgroup of -1", corrupt_first_fof_null, 4, "snapshot_004.h5",
     "'FirstHaloInFOFgroup' is -1 at halo 0, outside [0, 6)"},
    {"NextHaloInFOFgroup outside [-1, n_halos(N))", corrupt_next_fof_range, 4, "snapshot_004.h5",
     "'NextHaloInFOFgroup' is 9 at halo 1, outside -1 or [0, 6)"},
    {"Descendant outside [-1, n_halos(N+1))", corrupt_descendant_range, 4, "snapshot_004.h5",
     "'Descendant' is 4 at halo 0, outside -1 or [0, 4)"},
    {"non-null Descendant in the final snapshot", corrupt_descendant_in_final_snapshot,
     FIXTURE_SNAPSHOTS - 1, "snapshot_005.h5", "'Descendant' is 0 at halo 0, outside -1 or [0, 0)"},
    {"non-null FirstProgenitor in snapshot 0", corrupt_first_progenitor_in_snapshot_zero, 0,
     "snapshot_000.h5", "'FirstProgenitor' is 0 at halo 0, outside -1 or [0, 0)"},
};
#define LINK_CASE_COUNT (sizeof(LINK_CASES) / sizeof(LINK_CASES[0]))

/* Which snapshot the load-path child bodies load. Set by the parent before
   fork(), so the child inherits it. */
static int load_target_snapshot = 0;

static void child_load_slab(const char *dir) {
  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  struct SnapshotRunInfo info;
  struct SnapshotSlab slab = snapshot_slab_empty();
  configure_for_fixture(dir);
  snapshot_reader_open_run(reader, &info);
  snapshot_reader_load_slab(reader, load_target_snapshot, &slab);
}

/**
 * @test  test_corrupt_links_abort
 * Every out-of-range link the format forbids aborts, naming the field, the
 * halo index and the offending value.
 */
int test_corrupt_links_abort(void) {
  for (size_t i = 0; i < LINK_CASE_COUNT; i++) {
    const struct link_case *test_case = &LINK_CASES[i];
    char dir[MAX_STRING_LEN];

    TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
    if (test_case->mutate(dir) != 0) {
      fprintf(stderr, "  could not apply corruption: %s\n", test_case->description);
      remove_staged_fixture(dir);
      TEST_ASSERT(0, "fixture corruption helper failed");
    }

    load_target_snapshot = test_case->snapshot;
    const int aborted =
        expect_fatal(dir, child_load_slab, test_case->needle_file, test_case->needle_detail);
    remove_staged_fixture(dir);

    if (aborted != 1) {
      fprintf(stderr, "  case: %s\n", test_case->description);
      TEST_ASSERT(0, "an out-of-range link should abort naming field, halo and value");
    }
  }
  return TEST_PASS;
}

/**
 * @test  test_link_diagnostics_are_bounded
 * A systematically broken snapshot produces one counted summary per field, not
 * one line per halo.
 */
int test_link_diagnostics_are_bounded(void) {
  char dir[MAX_STRING_LEN];
  char path[MAX_STRING_LEN];
  char captured[16384];

  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  snapshot_path(path, sizeof(path), dir, 4);

  /* Every halo in snapshot 4 offends in two different fields at once. */
  TEST_ASSERT(set_i32_column(path, "Descendant", FIXTURE_HALO_COUNTS[4], 99) == 0,
              "should corrupt every Descendant in snapshot 4");
  TEST_ASSERT(set_i32_column(path, "NextProgenitor", FIXTURE_HALO_COUNTS[4], 77) == 0,
              "should corrupt every NextProgenitor in snapshot 4");

  load_target_snapshot = 4;
  const int aborted =
      expect_fatal_capture(dir, child_load_slab, "snapshot_004.h5", "has 2 invalid link field(s)",
                           captured, sizeof(captured));
  remove_staged_fixture(dir);
  if (aborted != 1) {
    fprintf(stderr, "  captured:\n%s\n", captured);
    TEST_ASSERT(0, "a systematically broken snapshot should abort");
  }

  TEST_ASSERT(strstr(captured, "has 6 halo(s) whose 'Descendant'") != NULL,
              "the Descendant summary should carry the offence count, not one line per halo");
  TEST_ASSERT(strstr(captured, "has 6 halo(s) whose 'NextProgenitor'") != NULL,
              "the NextProgenitor summary should carry the offence count");

  /* Twelve offending halos, but the diagnostics must stay per-field: one
     summary line for each of the two fields plus the abort. */
  int lines = 0;
  for (const char *c = captured; *c != '\0'; c++) {
    if (*c == '\n') {
      lines++;
    }
  }
  if (lines > 6) {
    fprintf(stderr, "  %d diagnostic lines for 12 offending halos:\n%s\n", lines, captured);
    TEST_ASSERT(0, "load-path diagnostics should be bounded per field, not per halo");
  }
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Slab lifecycle
 * ------------------------------------------------------------------------- */

static void child_load_into_loaded_slab(const char *dir) {
  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  struct SnapshotRunInfo info;
  struct SnapshotSlab slab = snapshot_slab_empty();
  configure_for_fixture(dir);
  snapshot_reader_open_run(reader, &info);
  snapshot_reader_load_slab(reader, 4, &slab);
  snapshot_reader_load_slab(reader, 5, &slab);
}

static void child_close_run_with_loaded_slab(const char *dir) {
  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  struct SnapshotRunInfo info;
  struct SnapshotSlab slab = snapshot_slab_empty();
  configure_for_fixture(dir);
  snapshot_reader_open_run(reader, &info);
  snapshot_reader_load_slab(reader, 4, &slab);
  snapshot_reader_close_run(reader);
}

static void child_load_below_range(const char *dir) {
  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  struct SnapshotRunInfo info;
  struct SnapshotSlab slab = snapshot_slab_empty();
  configure_for_fixture(dir);
  snapshot_reader_open_run(reader, &info);
  snapshot_reader_load_slab(reader, -1, &slab);
}

static void child_load_above_range(const char *dir) {
  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  struct SnapshotRunInfo info;
  struct SnapshotSlab slab = snapshot_slab_empty();
  configure_for_fixture(dir);
  snapshot_reader_open_run(reader, &info);
  snapshot_reader_load_slab(reader, info.snapshot_count, &slab);
}

/**
 * @test  test_slab_lifecycle
 * The slab handle contract: an out-of-range index aborts, loading into a
 * non-empty handle aborts, releasing an empty handle is a no-op, a second
 * release is safe, and close_run under a loaded slab aborts.
 */
int test_slab_lifecycle(void) {
  char dir[MAX_STRING_LEN];
  struct SnapshotRunInfo info;

  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");

  TEST_ASSERT(expect_fatal(dir, child_load_below_range, "snapshot -1 is outside", "[0, 6)") == 1,
              "load_slab(-1) should abort");
  TEST_ASSERT(expect_fatal(dir, child_load_above_range, "snapshot 6 is outside", "[0, 6)") == 1,
              "load_slab(snapshot_count) should abort");
  TEST_ASSERT(expect_fatal(dir, child_load_into_loaded_slab, "already holding snapshot 4",
                           "release it before loading snapshot 5") == 1,
              "load_slab into a non-empty handle should abort");
  TEST_ASSERT(expect_fatal(dir, child_close_run_with_loaded_slab, "close_run called with 1 slab(s)",
                           "must be released first") == 1,
              "close_run under a loaded slab should abort");

  /* The in-process half: releasing an empty handle, and releasing twice, are
     both defined as safe, so they must not abort. */
  configure_for_fixture(dir);
  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  snapshot_reader_open_run(reader, &info);

  struct SnapshotSlab empty = snapshot_slab_empty();
  TEST_ASSERT(empty.forest_index == NULL, "snapshot_slab_empty should carry a NULL forest_index");
  TEST_ASSERT(empty.halo_rank_in_forest == NULL,
              "snapshot_slab_empty should carry a NULL halo_rank_in_forest");
  snapshot_reader_release_slab(reader, &empty);
  TEST_ASSERT(snapshot_slab_is_empty(&empty), "releasing an empty slab should be a no-op");

  struct SnapshotSlab slab = snapshot_slab_empty();
  snapshot_reader_load_slab(reader, 4, &slab);
  snapshot_reader_release_slab(reader, &slab);
  snapshot_reader_release_slab(reader, &slab);
  TEST_ASSERT(snapshot_slab_is_empty(&slab), "a second release should leave the handle empty");
  TEST_ASSERT(slab.forest_index == NULL, "a second release should leave forest_index NULL");
  TEST_ASSERT(slab.halo_rank_in_forest == NULL,
              "a second release should leave halo_rank_in_forest NULL");

  /* A released slab leaves close_run unblocked. */
  snapshot_reader_close_run(reader);
  remove_staged_fixture(dir);
  return TEST_PASS;
}

/**
 * @test  test_load_release_leaves_no_leak
 * Loading and releasing every fixture snapshot releases every allocation.
 */
int test_load_release_leaves_no_leak(void) {
  char dir[MAX_STRING_LEN];
  struct SnapshotRunInfo info;

  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  configure_for_fixture(dir);

  const struct SnapshotReader *reader = snapshot_reader_lookup("snapshot_hdf5");
  snapshot_reader_open_run(reader, &info);
  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    struct SnapshotSlab slab = snapshot_slab_empty();
    snapshot_reader_load_slab(reader, snap, &slab);
    snapshot_reader_release_slab(reader, &slab);
  }
  snapshot_reader_close_run(reader);
  remove_staged_fixture(dir);

  char log_template[] = "/tmp/mimic_snapshot_slab_leak_XXXXXX";
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
              "load_slab followed by release_slab should leave no tracked allocation");

  /* Re-emit for the captured unit-run output. */
  check_memory_leaks();
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Registry disjointness and the run-scoped identity bounds
 * ------------------------------------------------------------------------- */

/**
 * @test  test_registries_are_disjoint
 * No name resolves in both the tree and the snapshot registry.
 *
 * The two-registry tree_type lookup in read_parameter_file.c tries the tree
 * registry first, so a name in both sets would silently resolve to the tree
 * reader. Enumerating the snapshot registry and probing the tree registry for
 * each name settles the intersection: it is empty iff no snapshot name is also a
 * tree name. tree_reader_lookup() is case-insensitive, so case variants are
 * covered too.
 */
int test_registries_are_disjoint(void) {
  const size_t count = snapshot_reader_count();
  TEST_ASSERT(count > 0, "an HDF5 build should register at least one snapshot reader");

  for (size_t i = 0; i < count; i++) {
    const struct SnapshotReader *reader = snapshot_reader_at(i);
    TEST_ASSERT(reader != NULL, "every index below the count should resolve");
    TEST_ASSERT(reader->name != NULL, "every registered snapshot reader should be named");
    TEST_ASSERT(tree_reader_lookup(reader->name) == NULL,
                "a snapshot reader name must not also resolve in the tree registry");
    TEST_ASSERT(snapshot_reader_lookup(reader->name) == reader,
                "a registered name should resolve to its own reader");
  }

  TEST_ASSERT(snapshot_reader_at(count) == NULL, "an out-of-range index should return NULL");
  return TEST_PASS;
}

/** @brief Identity bounds as a run-info value, for the predicate tests. */
static struct SnapshotRunInfo bounds(int64_t n_forests_total, int64_t max_halo_rank_in_forest) {
  struct SnapshotRunInfo info;
  info.snapshot_count = FIXTURE_SNAPSHOTS;
  info.format_version = FIXTURE_FORMAT_VERSION;
  info.n_forests_total = n_forests_total;
  info.max_halo_rank_in_forest = max_halo_rank_in_forest;
  return info;
}

/**
 * @test  test_identity_bounds_predicate
 * snapshot_identity_bounds_valid accepts encodable bounds and rejects the rest.
 */
int test_identity_bounds_predicate(void) {
  const struct SnapshotRunInfo fixture = bounds(FIXTURE_N_FORESTS_TOTAL, FIXTURE_MAX_RANK);
  const struct SnapshotRunInfo sentinel = bounds(SNAPSHOT_EMPTY_N_FORESTS, SNAPSHOT_EMPTY_MAX_RANK);

  TEST_ASSERT(snapshot_identity_bounds_valid(&fixture, (int64_t)TREE_MUL_FAC) != 0,
              "the fixture bounds should be encodable with the default multiplier");
  TEST_ASSERT(snapshot_identity_bounds_valid(&sentinel, (int64_t)TREE_MUL_FAC) != 0,
              "the empty-dataset sentinel should be accepted");
  TEST_ASSERT(snapshot_identity_bounds_valid(&sentinel, 1) != 0,
              "the empty-dataset sentinel should be accepted for any positive multiplier");

  TEST_ASSERT(snapshot_identity_bounds_valid(NULL, (int64_t)TREE_MUL_FAC) == 0,
              "a missing run info should be rejected");
  TEST_ASSERT(snapshot_identity_bounds_valid(&fixture, 0) == 0,
              "a zero multiplier should be rejected");
  TEST_ASSERT(snapshot_identity_bounds_valid(&fixture, -1) == 0,
              "a negative multiplier should be rejected");
  TEST_ASSERT(snapshot_identity_bounds_valid(&sentinel, 0) == 0,
              "a zero multiplier should be rejected even for the sentinel");

  const struct SnapshotRunInfo negative_forests = bounds(-1, FIXTURE_MAX_RANK);
  TEST_ASSERT(snapshot_identity_bounds_valid(&negative_forests, (int64_t)TREE_MUL_FAC) == 0,
              "a negative n_forests_total should be rejected");

  const struct SnapshotRunInfo negative_rank = bounds(FIXTURE_N_FORESTS_TOTAL, -1);
  TEST_ASSERT(snapshot_identity_bounds_valid(&negative_rank, (int64_t)TREE_MUL_FAC) == 0,
              "a negative max_halo_rank_in_forest outside the sentinel should be rejected");

  TEST_ASSERT(snapshot_identity_bounds_valid(&fixture, FIXTURE_MAX_RANK) == 0,
              "a multiplier equal to max_halo_rank_in_forest should be rejected");
  TEST_ASSERT(snapshot_identity_bounds_valid(&fixture, FIXTURE_MAX_RANK - 1) == 0,
              "a multiplier below max_halo_rank_in_forest should be rejected");
  TEST_ASSERT(snapshot_identity_bounds_valid(&fixture, FIXTURE_MAX_RANK + 1) != 0,
              "a multiplier one above max_halo_rank_in_forest should be accepted");

  return TEST_PASS;
}

/**
 * @test  test_identity_bounds_division_boundary
 * The forest-count ceiling is exact at every tested multiplier.
 *
 * The bound is n_forests_total <= INT64_MAX / multiplier - 1, expressed as a
 * division precisely so the check cannot overflow while performing it. These
 * cases pin both sides of that boundary, including multiplier = INT64_MAX, where
 * the ceiling collapses to zero.
 */
int test_identity_bounds_division_boundary(void) {
  const int64_t multipliers[] = {1, (int64_t)TREE_MUL_FAC, INT64_MAX};

  for (size_t i = 0; i < sizeof(multipliers) / sizeof(multipliers[0]); i++) {
    const int64_t multiplier = multipliers[i];
    const int64_t ceiling = INT64_MAX / multiplier - 1;
    /* max_halo_rank_in_forest 0 keeps every multiplier above the rank bound, so
       only the forest-count boundary is under test. */
    const struct SnapshotRunInfo accepted = bounds(ceiling, 0);
    const struct SnapshotRunInfo rejected = bounds(ceiling + 1, 0);

    TEST_ASSERT(snapshot_identity_bounds_valid(&accepted, multiplier) != 0,
                "n_forests_total = INT64_MAX / multiplier - 1 should be accepted");
    TEST_ASSERT(snapshot_identity_bounds_valid(&rejected, multiplier) == 0,
                "n_forests_total = INT64_MAX / multiplier should be rejected");
  }

  return TEST_PASS;
}

/** @brief Child body: open the fixture with a multiplier below the fixture rank. */
static void child_open_run_unencodable_multiplier(const char *dir) {
  struct SnapshotRunInfo info;
  configure_for_fixture(dir);
  MimicConfig.UniqueGalaxyIDMultiplier = FIXTURE_MAX_RANK;
  snapshot_reader_open_run(snapshot_reader_lookup("snapshot_hdf5"), &info);
}

/** @brief Child body: open the fixture with an unset (zero) multiplier. */
static void child_open_run_zero_multiplier(const char *dir) {
  struct SnapshotRunInfo info;
  configure_for_fixture(dir);
  MimicConfig.UniqueGalaxyIDMultiplier = 0;
  snapshot_reader_open_run(snapshot_reader_lookup("snapshot_hdf5"), &info);
}

/**
 * @test  test_open_run_checks_identity_bounds
 * open_run aborts when the dataset's identity bounds are not encodable.
 *
 * The abort is proof that the check runs before anything is published: nothing
 * downstream of the check executes.
 */
int test_open_run_checks_identity_bounds(void) {
  char dir[MAX_STRING_LEN];
  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");

  TEST_ASSERT(expect_fatal(dir, child_open_run_unencodable_multiplier, "not encodable",
                           "unique_galaxy_id_multiplier") == 1,
              "open_run should abort for a multiplier below max_halo_rank_in_forest");
  TEST_ASSERT(expect_fatal(dir, child_open_run_zero_multiplier, "not encodable",
                           "unique_galaxy_id_multiplier") == 1,
              "open_run should abort for a non-positive multiplier");

  remove_staged_fixture(dir);
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Snapshot Reader\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_registry_lookup);
  TEST_RUN(test_open_run_publishes_run_metadata);
  TEST_RUN(test_snapshot_halo_count_range);
  TEST_RUN(test_corrupt_inputs_abort);
  TEST_RUN(test_physical_value_tolerance_boundary);
  TEST_RUN(test_missing_hook_aborts);
  TEST_RUN(test_open_close_leaves_no_leak);
  TEST_RUN(test_load_slab_matches_fixture);
  TEST_RUN(test_corrupt_links_abort);
  TEST_RUN(test_link_diagnostics_are_bounded);
  TEST_RUN(test_slab_lifecycle);
  TEST_RUN(test_load_release_leaves_no_leak);
  TEST_RUN(test_registries_are_disjoint);
  TEST_RUN(test_identity_bounds_predicate);
  TEST_RUN(test_identity_bounds_division_boundary);
  TEST_RUN(test_open_run_checks_identity_bounds);

  TEST_SUMMARY();
  return TEST_RESULT();
}
