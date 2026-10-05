/**
 * @file    test_horizontal_v3_reader.c
 * @brief   Unit tests for the horizontal_hdf5 reader's format_version 3 path.
 *
 * Validates, against the committed converter-produced fixture in
 * tests/data/horizontal_v3/dataset (a gap across an empty snapshot, a
 * cross-snapshot NextProgenitor and one selected extra the package does not
 * declare): the run metadata open_run publishes; slab contents, target-snapshot
 * arrays and SourceHaloID field by field; that the undeclared extra is
 * validated but never materialised; one abort per open-time and
 * load-time invariant a consumer checks; rejection of soft and external links
 * under /halos and /schema whether or not the package declares the field; that
 * n_halos above INT32_MAX passes the header and shape checks; bounded link
 * diagnostics; row-range loads against whole loads, range-bound aborts, the
 * ForestIndex scan and open_run without its column scans (on this fixture and
 * the mini-millennium-horizontal worked graph); range reads and the ForestIndex
 * scan across the reader's 8,192-row block boundary (the wide_slab fixture); and
 * freedom from leaks.
 *
 * The fixture's /schema declares mini-Millennium L-Halo payload units, which
 * only the mini-millennium-horizontal package declares, so every test that
 * opens it successfully runs only when that package is compiled in
 * (MIMIC_COMPILED_SIMULATION) and skips otherwise. Under any package whose
 * links are `int`, test_int_link_package_rejects_v3 instead pins the refusal
 * to narrow version 3's int64 links.
 *
 * Corrupt-input cases use the pattern of the version 2 reader tests
 * (simulations/micro-uchuu-ascii-horizontal/_tests/unit/test_unit_horizontal_reader_open.c):
 * stage a scratch copy, mutate it with the HDF5 C API, then fork a child that
 * opens or loads it, since FATAL_ERROR ends the process.
 */

#include "../framework/test_framework.h"
#include "../framework/child_capture.h"

#include "../../src/include/constants.h"
#include "../../src/include/proto.h"
#include "../../src/include/types.h"
#include "../../src/io/horizontal/reader.h"
#include "../../src/util/error.h"
#include "../../src/util/memory.h"

#include <hdf5.h>

#include <inttypes.h>
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
 * Fixture facts (tests/data/horizontal_v3; see tests/data/README.md)
 * ------------------------------------------------------------------------- */

#define FIXTURE_DIR "tests/data/horizontal_v3/dataset"
#define FIXTURE_A_LIST "fixture.a_list"
#define FIXTURE_PACKAGE "mini-millennium-horizontal"
#define FIXTURE_SNAPSHOTS 4
#define FIXTURE_EMPTY_SNAPSHOT 2
#define FIXTURE_N_FORESTS_TOTAL 2
#define FIXTURE_MAX_RANK 3
#define FIXTURE_MAX_HALOS 3

static const int64_t FIXTURE_HALO_COUNTS[FIXTURE_SNAPSHOTS] = {2, 2, 0, 3};

/* simulations/mini-millennium-horizontal/simulation_info.yaml, which repeats
   mini-Millennium's own values; the fixture's headers were stamped from
   simulations/mini-millennium/simulation_info.yaml by the converter. */
#define FIXTURE_BOX_SIZE 62.5
#define FIXTURE_OMEGA_MATTER 0.25
#define FIXTURE_OMEGA_LAMBDA 0.75
#define FIXTURE_HUBBLE_H 0.73
#define FIXTURE_PART_MASS 0.0860657

/**
 * Expected per-row topology, hand-derived from the source trees
 * (tests/data/horizontal_v3/source/generate_source.py) after the converter's
 * SourceHaloID row ordering. Tree 0's halo A (snapshot 0, row 0) descends
 * across the empty snapshot 2 to D (snapshot 3, row 0), and names as its
 * NextProgenitor B (snapshot 1, row 0), which descends to the same D.
 */
struct expected_row {
  int64_t descendant, first_progenitor, next_progenitor, first_fof, next_fof;
  int32_t descendant_snap, first_progenitor_snap, next_progenitor_snap;
  int64_t source_halo_id, forest_index, halo_rank;
  int len;
  long long most_bound_id;
  float m_crit200;
};

static const struct expected_row EXPECTED_SNAP0[] = {
    {0, -1, 0, 0, -1, 3, -1, 1, 1, 0, 0, 21, 700, 1.25f},
    {1, -1, -1, 1, -1, 1, -1, -1, 5, 1, 0, 31, 800, 13.75f},
};
static const struct expected_row EXPECTED_SNAP1[] = {
    {0, -1, -1, 0, -1, 3, -1, -1, 2, 0, 1, 22, -701, 2.5f},
    {-1, 1, -1, 1, -1, -1, 0, -1, 6, 1, 1, 32, 801, 15.0f},
};
static const struct expected_row EXPECTED_SNAP3[] = {
    {-1, 0, -1, 0, 1, -1, 0, -1, 3, 0, 2, 23, 702, 3.75f},
    {-1, -1, -1, 0, -1, -1, -1, -1, 4, 0, 3, 24, 702, 5.0f},
    {-1, -1, -1, 2, -1, -1, -1, -1, 7, 1, 2, 33, 802, 16.25f},
};

static int compiled_for_fixture_package(void) {
  return strcmp(MIMIC_COMPILED_SIMULATION, FIXTURE_PACKAGE) == 0;
}

#define REQUIRE_FIXTURE_PACKAGE()                                                                  \
  do {                                                                                             \
    if (!compiled_for_fixture_package()) {                                                         \
      return TEST_SKIP_WITH("the v3 fixture's /schema matches only SIMULATION=" FIXTURE_PACKAGE);  \
    }                                                                                              \
  } while (0)

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
  static char buffer[1 << 20];
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

/** @brief Copy the committed fixture into a fresh scratch directory. */
static int stage_fixture(char *dir, size_t dir_size) {
  char template_path[] = "/tmp/mimic_horizontal_v3_XXXXXX";
  char *made = mkdtemp(template_path);
  if (made == NULL) {
    return -1;
  }
  snprintf(dir, dir_size, "%s", made);

  char src[MAX_STRING_LEN];
  char dst[MAX_STRING_LEN];
  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    snapshot_path(src, sizeof(src), FIXTURE_DIR, snap);
    snapshot_path(dst, sizeof(dst), dir, snap);
    if (copy_file(src, dst) != 0) {
      remove_staged_fixture(dir);
      return -1;
    }
  }
  snprintf(src, sizeof(src), "%s/%s", FIXTURE_DIR, FIXTURE_A_LIST);
  snprintf(dst, sizeof(dst), "%s/%s", dir, FIXTURE_A_LIST);
  if (copy_file(src, dst) != 0) {
    remove_staged_fixture(dir);
    return -1;
  }
  return 0;
}

/**
 * @brief   Point MimicConfig at a version 3 dataset directory and its snapshot
 *          list, as a run of the mini-millennium-horizontal package would
 *          configure it.
 */
static void configure_for_dataset_dir(const char *dir, const char *a_list) {
  memset(&MimicConfig, 0, sizeof(MimicConfig));
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s", dir);
  snprintf(MimicConfig.FileWithSnapList, sizeof(MimicConfig.FileWithSnapList), "%s/%s", dir,
           a_list);
  read_snap_list();
  MimicConfig.MAXSNAPS = MimicConfig.Snaplistlen;
  MimicConfig.UniqueGalaxyIDMultiplier = (int64_t)TREE_MUL_FAC;
  MimicConfig.BoxSize = FIXTURE_BOX_SIZE;
  MimicConfig.Omega = FIXTURE_OMEGA_MATTER;
  MimicConfig.OmegaLambda = FIXTURE_OMEGA_LAMBDA;
  MimicConfig.Hubble_h = FIXTURE_HUBBLE_H;
  MimicConfig.PartMass = FIXTURE_PART_MASS;
}

/** @brief Point MimicConfig at a staged copy of this suite's fixture. */
static void configure_for_fixture(const char *dir) {
  configure_for_dataset_dir(dir, FIXTURE_A_LIST);
}

/* ---------------------------------------------------------------------------
 * Fixture mutation helpers (HDF5 C API)
 *
 * Each returns 0 on success and -1 on failure; callers assert on the result so
 * a broken mutation can never masquerade as a passing abort.
 * ------------------------------------------------------------------------- */

static int write_header_attr(const char *file_path, const char *name, hid_t type,
                             const void *value) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  /* Opened through the group, not by path: HDF5 cannot write an attribute
     opened by path from dense attribute storage ("can't locate open
     attribute"), which the converter's libver="latest" headers use. */
  int rc = 0;
  hid_t group = H5Gopen2(file, "/header", H5P_DEFAULT);
  hid_t attr = group < 0 ? H5I_INVALID_HID : H5Aopen(group, name, H5P_DEFAULT);
  if (attr < 0 || H5Awrite(attr, type, value) < 0) {
    rc = -1;
  }
  if (attr >= 0) {
    H5Aclose(attr);
  }
  if (group >= 0) {
    H5Gclose(group);
  }
  H5Fclose(file);
  return rc;
}

static int set_attr_i32(const char *file_path, const char *name, int32_t value) {
  return write_header_attr(file_path, name, H5T_NATIVE_INT32, &value);
}

static int set_attr_i64(const char *file_path, const char *name, int64_t value) {
  return write_header_attr(file_path, name, H5T_NATIVE_INT64, &value);
}

static int set_attr_f64(const char *file_path, const char *name, double value) {
  return write_header_attr(file_path, name, H5T_NATIVE_DOUBLE, &value);
}

static int set_attr_i32_all(const char *dir, const char *name, int32_t value) {
  char path[MAX_STRING_LEN];
  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    snapshot_path(path, sizeof(path), dir, snap);
    if (set_attr_i32(path, name, value) != 0) {
      return -1;
    }
  }
  return 0;
}

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

/**
 * @brief   Replace an attribute of `object` with a string attribute.
 * @param   fixed_size  Byte length of a fixed-length string, or 0 for a
 *                      variable-length one.
 */
static int replace_string_attr(const char *file_path, const char *object, const char *name,
                               const char *value, size_t fixed_size, H5T_cset_t cset) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t obj = H5Oopen(file, object, H5P_DEFAULT);
  int rc = obj < 0 ? -1 : 0;
  if (rc == 0 && H5Aexists(obj, name) > 0 && H5Adelete(obj, name) < 0) {
    rc = -1;
  }
  hid_t type = H5Tcopy(H5T_C_S1);
  hid_t space = H5Screate(H5S_SCALAR);
  if (rc == 0) {
    if (H5Tset_size(type, fixed_size > 0 ? fixed_size : H5T_VARIABLE) < 0 ||
        H5Tset_cset(type, cset) < 0 ||
        (fixed_size > 0 && H5Tset_strpad(type, H5T_STR_NULLPAD) < 0)) {
      rc = -1;
    }
  }
  if (rc == 0) {
    hid_t attr = H5Acreate2(obj, name, type, space, H5P_DEFAULT, H5P_DEFAULT);
    if (attr < 0) {
      rc = -1;
    } else {
      if (fixed_size > 0) {
        char buffer[256];
        memset(buffer, 0, sizeof(buffer));
        snprintf(buffer, sizeof(buffer), "%s", value);
        rc = H5Awrite(attr, type, buffer) < 0 ? -1 : 0;
      } else {
        rc = H5Awrite(attr, type, &value) < 0 ? -1 : 0;
      }
      H5Aclose(attr);
    }
  }
  H5Sclose(space);
  H5Tclose(type);
  if (obj >= 0) {
    H5Oclose(obj);
  }
  H5Fclose(file);
  return rc;
}

static int set_header_string(const char *file_path, const char *name, const char *value,
                             size_t size) {
  return replace_string_attr(file_path, "/header", name, value, size, H5T_CSET_ASCII);
}

static int set_schema_attr(const char *file_path, const char *field, const char *name,
                           const char *value) {
  char object[MAX_STRING_LEN];
  snprintf(object, sizeof(object), "/schema/%s", field);
  return replace_string_attr(file_path, object, name, value, 0, H5T_CSET_UTF8);
}

static int set_schema_attr_all(const char *dir, const char *field, const char *name,
                               const char *value) {
  char path[MAX_STRING_LEN];
  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    snapshot_path(path, sizeof(path), dir, snap);
    if (set_schema_attr(path, field, name, value) != 0) {
      return -1;
    }
  }
  return 0;
}

/** @brief Add a /schema subgroup with the four required string attributes. */
static int add_schema_group(const char *file_path, const char *name, const char *type) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  char object[MAX_STRING_LEN];
  snprintf(object, sizeof(object), "/schema/%s", name);
  hid_t group = H5Gcreate2(file, object, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  const int rc = group < 0 ? -1 : 0;
  if (group >= 0) {
    H5Gclose(group);
  }
  H5Fclose(file);
  if (rc != 0) {
    return -1;
  }
  if (set_schema_attr(file_path, name, "type", type) != 0 ||
      set_schema_attr(file_path, name, "units", "dimensionless") != 0 ||
      set_schema_attr(file_path, name, "h_convention", "none") != 0 ||
      set_schema_attr(file_path, name, "description", "test declaration") != 0) {
    return -1;
  }
  return 0;
}

static int add_int_attr(const char *file_path, const char *object, const char *name) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  const int32_t value = 1;
  hid_t space = H5Screate(H5S_SCALAR);
  hid_t attr = H5Acreate_by_name(file, object, name, H5T_STD_I32LE, space, H5P_DEFAULT, H5P_DEFAULT,
                                 H5P_DEFAULT);
  int rc = 0;
  if (attr < 0 || H5Awrite(attr, H5T_NATIVE_INT32, &value) < 0) {
    rc = -1;
  }
  if (attr >= 0) {
    H5Aclose(attr);
  }
  H5Sclose(space);
  H5Fclose(file);
  return rc;
}

static int delete_header_attr(const char *file_path, const char *name) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  const int rc = H5Adelete_by_name(file, "/header", name, H5P_DEFAULT) < 0 ? -1 : 0;
  H5Fclose(file);
  return rc;
}

static int delete_link(const char *file_path, const char *name) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  const int rc = H5Ldelete(file, name, H5P_DEFAULT) < 0 ? -1 : 0;
  H5Fclose(file);
  return rc;
}

static int create_group(const char *file_path, const char *name) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t group = H5Gcreate2(file, name, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  const int rc = group < 0 ? -1 : 0;
  if (group >= 0) {
    H5Gclose(group);
  }
  H5Fclose(file);
  return rc;
}

/**
 * @brief   Move object `name` to `holder` and leave a soft (or external) link
 *          named `name` pointing at it.
 *
 * `holder` sorts after `name` in every case below, so the reader's
 * name-ordered iteration meets the link before any unexpected holder object.
 */
static int relink(const char *file_path, const char *name, const char *holder, int external) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  int rc = H5Lmove(file, name, file, holder, H5P_DEFAULT, H5P_DEFAULT) < 0 ? -1 : 0;
  if (rc == 0) {
    if (external) {
      rc = H5Lcreate_external(file_path, holder, file, name, H5P_DEFAULT, H5P_DEFAULT) < 0 ? -1 : 0;
    } else {
      rc = H5Lcreate_soft(holder, file, name, H5P_DEFAULT, H5P_DEFAULT) < 0 ? -1 : 0;
    }
  }
  H5Fclose(file);
  return rc;
}

/**
 * @brief   Overwrite a fixed-length header string with `size` raw bytes, through
 *          the attribute's own datatype so no string conversion rewrites the
 *          padding or the bytes after a NUL.
 */
static int set_header_raw_bytes(const char *file_path, const char *name, const char *bytes) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  int rc = 0;
  hid_t group = H5Gopen2(file, "/header", H5P_DEFAULT);
  hid_t attr = group < 0 ? H5I_INVALID_HID : H5Aopen(group, name, H5P_DEFAULT);
  hid_t dtype = attr < 0 ? H5I_INVALID_HID : H5Aget_type(attr);
  if (dtype < 0 || H5Awrite(attr, dtype, bytes) < 0) {
    rc = -1;
  }
  if (dtype >= 0) {
    H5Tclose(dtype);
  }
  if (attr >= 0) {
    H5Aclose(attr);
  }
  if (group >= 0) {
    H5Gclose(group);
  }
  H5Fclose(file);
  return rc;
}

/** @brief Rename attribute `name` of `object` to `new_name`, keeping its value and type. */
static int rename_attr(const char *file_path, const char *object, const char *name,
                       const char *new_name) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  const int rc = H5Arename_by_name(file, object, name, new_name, H5P_DEFAULT) < 0 ? -1 : 0;
  H5Fclose(file);
  return rc;
}

/** @brief Replace `/header` attribute `name` with a scalar int64 holding `value`. */
static int replace_header_attr_with_i64(const char *file_path, const char *name, int64_t value) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  int rc = 0;
  if (H5Adelete_by_name(file, "/header", name, H5P_DEFAULT) < 0) {
    rc = -1;
  } else {
    hid_t space = H5Screate(H5S_SCALAR);
    hid_t attr = H5Acreate_by_name(file, "/header", name, H5T_STD_I64LE, space, H5P_DEFAULT,
                                   H5P_DEFAULT, H5P_DEFAULT);
    if (attr < 0 || H5Awrite(attr, H5T_NATIVE_INT64, &value) < 0) {
      rc = -1;
    }
    if (attr >= 0) {
      H5Aclose(attr);
    }
    H5Sclose(space);
  }
  H5Fclose(file);
  return rc;
}

/** @brief Create a scalar int32 dataset at `name` (a member that is not a group). */
static int create_scalar_dataset(const char *file_path, const char *name) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t space = H5Screate(H5S_SCALAR);
  hid_t dset = H5Dcreate2(file, name, H5T_STD_I32LE, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT);
  const int rc = dset < 0 ? -1 : 0;
  if (dset >= 0) {
    H5Dclose(dset);
  }
  H5Sclose(space);
  H5Fclose(file);
  return rc;
}

static int write_element(const char *file_path, const char *dataset, hsize_t index, hid_t mem_type,
                         const void *value) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  char name[MAX_STRING_LEN];
  snprintf(name, sizeof(name), "/halos/%s", dataset);
  int rc = 0;
  hid_t dset = H5Dopen2(file, name, H5P_DEFAULT);
  if (dset < 0) {
    rc = -1;
  } else {
    hid_t space = H5Dget_space(dset);
    const hsize_t start[1] = {index};
    const hsize_t count[1] = {1};
    hid_t memspace = H5Screate_simple(1, count, NULL);
    if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, count, NULL) < 0 ||
        H5Dwrite(dset, mem_type, memspace, space, H5P_DEFAULT, value) < 0) {
      rc = -1;
    }
    H5Sclose(memspace);
    H5Sclose(space);
    H5Dclose(dset);
  }
  H5Fclose(file);
  return rc;
}

static int set_i32_element(const char *file_path, const char *dataset, hsize_t index,
                           int32_t value) {
  return write_element(file_path, dataset, index, H5T_NATIVE_INT32, &value);
}

static int set_i64_element(const char *file_path, const char *dataset, hsize_t index,
                           int64_t value) {
  return write_element(file_path, dataset, index, H5T_NATIVE_INT64, &value);
}

/**
 * @brief   Replace /halos/<name> with an unwritten chunked dataset of `type`
 *          and shape [n] (ncols 0) or [n, ncols].
 *
 * Chunked and never written, so HDF5 allocates no storage: a logical length
 * far above INT32_MAX costs a few bytes on disk and reads back its fill value.
 */
static int recreate_dataset(hid_t file, const char *name, hid_t type, hsize_t n, int ncols) {
  char full[MAX_STRING_LEN];
  snprintf(full, sizeof(full), "/halos/%s", name);
  if (H5Lexists(file, full, H5P_DEFAULT) > 0 && H5Ldelete(file, full, H5P_DEFAULT) < 0) {
    return -1;
  }
  const int rank = ncols == 0 ? 1 : 2;
  const hsize_t dims[2] = {n, (hsize_t)ncols};
  const hsize_t chunk[2] = {n > 0 && n < 65536 ? n : 65536, (hsize_t)ncols};
  hid_t space = H5Screate_simple(rank, dims, NULL);
  hid_t dcpl = H5Pcreate(H5P_DATASET_CREATE);
  int rc = 0;
  if (space < 0 || dcpl < 0 || H5Pset_chunk(dcpl, rank, chunk) < 0) {
    rc = -1;
  } else {
    hid_t dset = H5Dcreate2(file, full, type, space, H5P_DEFAULT, dcpl, H5P_DEFAULT);
    if (dset < 0) {
      rc = -1;
    } else {
      H5Dclose(dset);
    }
  }
  if (dcpl >= 0) {
    H5Pclose(dcpl);
  }
  if (space >= 0) {
    H5Sclose(space);
  }
  return rc;
}

static int recreate_dataset_in(const char *file_path, const char *name, hid_t type, hsize_t n,
                               int ncols) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  const int rc = recreate_dataset(file, name, type, n, ncols);
  H5Fclose(file);
  return rc;
}

/**
 * @brief   Give one snapshot file a logical halo count of `n`: its header and
 *          every /halos dataset, each keeping its dtype and column count.
 */
static int widen_snapshot(const char *file_path, int64_t n) {
  if (set_attr_i64(file_path, "n_halos", n) != 0) {
    return -1;
  }
  hid_t file = H5Fopen(file_path, H5F_ACC_RDWR, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  hid_t group = H5Gopen2(file, "/halos", H5P_DEFAULT);
  H5G_info_t info;
  int rc = (group < 0 || H5Gget_info(group, &info) < 0) ? -1 : 0;
  char names[64][MAX_STRING_LEN];
  hsize_t count = 0;
  for (hsize_t i = 0; rc == 0 && i < info.nlinks && i < 64; i++) {
    if (H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, i, names[count], MAX_STRING_LEN,
                           H5P_DEFAULT) < 0) {
      rc = -1;
    }
    count++;
  }
  for (hsize_t i = 0; rc == 0 && i < count; i++) {
    hid_t dset = H5Dopen2(group, names[i], H5P_DEFAULT);
    hid_t type = H5Dget_type(dset);
    hid_t space = H5Dget_space(dset);
    const int rank = H5Sget_simple_extent_ndims(space);
    H5Sclose(space);
    H5Dclose(dset);
    if (recreate_dataset(file, names[i], type, (hsize_t)n, rank == 2 ? 3 : 0) != 0) {
      rc = -1;
    }
    H5Tclose(type);
  }
  if (group >= 0) {
    H5Gclose(group);
  }
  H5Fclose(file);
  return rc;
}

/** @brief Read one whole /halos column of a fixture file into `out`. */
static int read_halo_column(const char *file_path, const char *dataset, hid_t mem_type, void *out) {
  hid_t file = H5Fopen(file_path, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) {
    return -1;
  }
  char name[MAX_STRING_LEN];
  snprintf(name, sizeof(name), "/halos/%s", dataset);
  int rc = 0;
  hid_t dset = H5Dopen2(file, name, H5P_DEFAULT);
  if (dset < 0 || H5Dread(dset, mem_type, H5S_ALL, H5S_ALL, H5P_DEFAULT, out) < 0) {
    rc = -1;
  }
  if (dset >= 0) {
    H5Dclose(dset);
  }
  H5Fclose(file);
  return rc;
}

static const struct HorizontalReader *reader(void) {
  return horizontal_reader_lookup("horizontal_hdf5");
}

/* open_run's options for a full validation, as the serial driver opens a run. */
static const struct HorizontalOpenOptions FULL_SCAN = {.validate_columns = 1};

/** @brief Load the whole of one snapshot, as the serial driver does. */
static void load_whole_slab(int64_t snapnum, struct SnapshotSlab *slab) {
  horizontal_reader_load_slab(reader(), snapnum, 0, horizontal_reader_halo_count(reader(), snapnum),
                              slab);
}

static void child_open_run(const char *dir) {
  struct HorizontalRunInfo info;
  configure_for_fixture(dir);
  horizontal_reader_open_run(reader(), &FULL_SCAN, &info);
  horizontal_reader_close_run(reader());
}

/* Snapshot the load-time children load; set by the parent before forking. */
static int64_t child_load_snapnum = 0;

static void child_load_slab(const char *dir) {
  struct HorizontalRunInfo info;
  configure_for_fixture(dir);
  horizontal_reader_open_run(reader(), &FULL_SCAN, &info);
  struct SnapshotSlab slab = snapshot_slab_empty();
  load_whole_slab(child_load_snapnum, &slab);
  horizontal_reader_release_slab(reader(), &slab);
  horizontal_reader_close_run(reader());
}

static void child_open_run_small_multiplier(const char *dir) {
  struct HorizontalRunInfo info;
  configure_for_fixture(dir);
  /* max_halo_rank_in_forest is 3, so a multiplier of 3 cannot encode rank 3. */
  MimicConfig.UniqueGalaxyIDMultiplier = FIXTURE_MAX_RANK;
  horizontal_reader_open_run(reader(), &FULL_SCAN, &info);
}

/** @brief check_memory_leaks() output, captured through the log stream. */
static int no_tracked_leaks(void) {
  char log_template[] = "/tmp/mimic_horizontal_v3_leak_XXXXXX";
  const int fd = mkstemp(log_template);
  if (fd < 0) {
    return 0;
  }
  FILE *log = fdopen(fd, "w+");
  if (log == NULL) {
    close(fd);
    unlink(log_template);
    return 0;
  }
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
  return strstr(captured, "Memory leak detected") == NULL;
}

/* ---------------------------------------------------------------------------
 * Acceptance and slab contents
 * ------------------------------------------------------------------------- */

/**
 * @test  test_v3_open_run_publishes_run_metadata
 * The unmodified converter fixture opens, publishing version 3, links_adjacent
 * 0 (it has gaps), the identity bounds and every snapshot's count -- including
 * the empty snapshot 2 -- and open/close leaves no tracked allocation.
 */
int test_v3_open_run_publishes_run_metadata(void) {
  REQUIRE_FIXTURE_PACKAGE();
  char dir[MAX_STRING_LEN];
  struct HorizontalRunInfo info;
  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  configure_for_fixture(dir);

  horizontal_reader_open_run(reader(), &FULL_SCAN, &info);
  TEST_ASSERT_EQUAL(info.format_version, 3, "open_run should publish format_version 3");
  TEST_ASSERT_EQUAL(info.links_adjacent, 0, "a gapped dataset should publish links_adjacent 0");
  TEST_ASSERT_EQUAL(info.snapshot_count, FIXTURE_SNAPSHOTS, "snapshot_count should be 4");
  TEST_ASSERT_EQUAL(info.n_forests_total, FIXTURE_N_FORESTS_TOTAL, "n_forests_total should be 2");
  TEST_ASSERT_EQUAL(info.max_halo_rank_in_forest, FIXTURE_MAX_RANK,
                    "max_halo_rank_in_forest should be 3");
  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    TEST_ASSERT_EQUAL(horizontal_reader_halo_count(reader(), snap), FIXTURE_HALO_COUNTS[snap],
                      "each snapshot's halo count should match the fixture");
  }
  horizontal_reader_close_run(reader());
  remove_staged_fixture(dir);
  TEST_ASSERT(no_tracked_leaks(), "open_run followed by close_run should leave no allocation");
  return TEST_PASS;
}

/** @brief Compare one loaded slab with the hand-derived expected rows. */
static int slab_matches_expected(const struct SnapshotSlab *slab, const struct expected_row *rows,
                                 int64_t n) {
  if (slab->nhalos != n) {
    return 0;
  }
  for (int64_t i = 0; i < n; i++) {
    const struct RawHalo *h = &slab->halos[i];
    const struct expected_row *e = &rows[i];
    if (h->Descendant != e->descendant || h->FirstProgenitor != e->first_progenitor ||
        h->NextProgenitor != e->next_progenitor || h->FirstHaloInFOFgroup != e->first_fof ||
        h->NextHaloInFOFgroup != e->next_fof ||
        slab->descendant_snapshot[i] != e->descendant_snap ||
        slab->first_progenitor_snapshot[i] != e->first_progenitor_snap ||
        slab->next_progenitor_snapshot[i] != e->next_progenitor_snap ||
        slab->source_halo_id[i] != e->source_halo_id || slab->forest_index[i] != e->forest_index ||
        slab->halo_rank_in_forest[i] != e->halo_rank || h->Len != e->len ||
        h->MostBoundID != e->most_bound_id || h->M_Crit200 != e->m_crit200 ||
        h->SnapNum != (int)slab->snapnum) {
      fprintf(stderr, "  snapshot %" PRId64 " row %" PRId64 " differs from the expected graph\n",
              slab->snapnum, i);
      return 0;
    }
  }
  return 1;
}

/**
 * @brief   Compare every declared field of one loaded slab with a direct read
 *          of its fixture file, bit for bit.
 */
static int slab_matches_file(const char *file_path, const struct SnapshotSlab *slab) {
  static int64_t i64[FIXTURE_MAX_HALOS];
  static int32_t i32[FIXTURE_MAX_HALOS];
  static float f32[FIXTURE_MAX_HALOS * NDIM];
  const int64_t n = slab->nhalos;

  static const char *const links[] = {"Descendant", "FirstProgenitor", "NextProgenitor",
                                      "FirstHaloInFOFgroup", "NextHaloInFOFgroup"};
  for (int l = 0; l < 5; l++) {
    if (read_halo_column(file_path, links[l], H5T_NATIVE_INT64, i64) != 0) {
      return 0;
    }
    for (int64_t i = 0; i < n; i++) {
      const struct RawHalo *h = &slab->halos[i];
      const long long got = l == 0   ? h->Descendant
                            : l == 1 ? h->FirstProgenitor
                            : l == 2 ? h->NextProgenitor
                            : l == 3 ? h->FirstHaloInFOFgroup
                                     : h->NextHaloInFOFgroup;
      if (got != i64[i]) {
        return 0;
      }
    }
  }

  const char *const targets[] = {"DescendantSnapshot", "FirstProgenitorSnapshot",
                                 "NextProgenitorSnapshot"};
  const int32_t *const target_arrays[] = {
      slab->descendant_snapshot, slab->first_progenitor_snapshot, slab->next_progenitor_snapshot};
  for (int t = 0; t < 3; t++) {
    if (read_halo_column(file_path, targets[t], H5T_NATIVE_INT32, i32) != 0 ||
        memcmp(i32, target_arrays[t], sizeof(int32_t) * (size_t)n) != 0) {
      return 0;
    }
  }
  if (read_halo_column(file_path, "SourceHaloID", H5T_NATIVE_INT64, i64) != 0 ||
      memcmp(i64, slab->source_halo_id, sizeof(int64_t) * (size_t)n) != 0) {
    return 0;
  }
  if (read_halo_column(file_path, "ForestIndex", H5T_NATIVE_INT64, i64) != 0 ||
      memcmp(i64, slab->forest_index, sizeof(int64_t) * (size_t)n) != 0) {
    return 0;
  }
  if (read_halo_column(file_path, "HaloRankInForest", H5T_NATIVE_INT64, i64) != 0 ||
      memcmp(i64, slab->halo_rank_in_forest, sizeof(int64_t) * (size_t)n) != 0) {
    return 0;
  }

  if (read_halo_column(file_path, "Len", H5T_NATIVE_INT32, i32) != 0) {
    return 0;
  }
  for (int64_t i = 0; i < n; i++) {
    if (slab->halos[i].Len != i32[i]) {
      return 0;
    }
  }
  if (read_halo_column(file_path, "SnapNum", H5T_NATIVE_INT32, i32) != 0) {
    return 0;
  }
  for (int64_t i = 0; i < n; i++) {
    if (slab->halos[i].SnapNum != i32[i]) {
      return 0;
    }
  }
  if (read_halo_column(file_path, "MostBoundID", H5T_NATIVE_INT64, i64) != 0) {
    return 0;
  }
  for (int64_t i = 0; i < n; i++) {
    if (slab->halos[i].MostBoundID != i64[i]) {
      return 0;
    }
  }

  static const char *const scalars[] = {"M_Crit200", "VelDisp", "Vmax"};
  for (int s = 0; s < 3; s++) {
    if (read_halo_column(file_path, scalars[s], H5T_NATIVE_FLOAT, f32) != 0) {
      return 0;
    }
    for (int64_t i = 0; i < n; i++) {
      const float got = s == 0   ? slab->halos[i].M_Crit200
                        : s == 1 ? slab->halos[i].VelDisp
                                 : slab->halos[i].Vmax;
      if (memcmp(&got, &f32[i], sizeof(float)) != 0) {
        return 0;
      }
    }
  }
  static const char *const vectors[] = {"Pos", "Vel", "Spin"};
  for (int v = 0; v < 3; v++) {
    if (read_halo_column(file_path, vectors[v], H5T_NATIVE_FLOAT, f32) != 0) {
      return 0;
    }
    for (int64_t i = 0; i < n; i++) {
      const float *got = v == 0   ? slab->halos[i].Pos
                         : v == 1 ? slab->halos[i].Vel
                                  : slab->halos[i].Spin;
      if (memcmp(got, &f32[i * NDIM], sizeof(float) * NDIM) != 0) {
        return 0;
      }
    }
  }
  return 1;
}

/**
 * @test  test_v3_load_slab_matches_fixture
 * Every snapshot loads: the gap across the empty snapshot, the cross-snapshot
 * NextProgenitor and every payload field land in the int64 RawHalo links and the
 * reader-owned target-snapshot and SourceHaloID arrays, field by field against both the
 * hand-derived graph and a direct read of each file; the empty snapshot loads as a zero-halo slab
 * with every array NULL; and load/release leaves no tracked allocation.
 */
int test_v3_load_slab_matches_fixture(void) {
  REQUIRE_FIXTURE_PACKAGE();
  char dir[MAX_STRING_LEN];
  struct HorizontalRunInfo info;
  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  configure_for_fixture(dir);
  horizontal_reader_open_run(reader(), &FULL_SCAN, &info);

  for (int snap = 0; snap < FIXTURE_SNAPSHOTS; snap++) {
    struct SnapshotSlab slab = snapshot_slab_empty();
    load_whole_slab(snap, &slab);
    TEST_ASSERT_EQUAL(slab.snapnum, snap, "the slab should carry its snapshot number");
    TEST_ASSERT_EQUAL(slab.nhalos, FIXTURE_HALO_COUNTS[snap], "the slab should hold n_halos");

    if (snap == FIXTURE_EMPTY_SNAPSHOT) {
      TEST_ASSERT(!snapshot_slab_is_empty(&slab), "an empty snapshot is still a loaded slab");
      TEST_ASSERT(slab.halos == NULL && slab.forest_index == NULL &&
                      slab.halo_rank_in_forest == NULL && slab.descendant_snapshot == NULL &&
                      slab.first_progenitor_snapshot == NULL &&
                      slab.next_progenitor_snapshot == NULL && slab.source_halo_id == NULL,
                  "an empty snapshot's slab should carry only NULL arrays");
    } else {
      const struct expected_row *rows = snap == 0   ? EXPECTED_SNAP0
                                        : snap == 1 ? EXPECTED_SNAP1
                                                    : EXPECTED_SNAP3;
      TEST_ASSERT(slab_matches_expected(&slab, rows, FIXTURE_HALO_COUNTS[snap]),
                  "slab links, target snapshots, identity and payload should match the graph");
      char path[MAX_STRING_LEN];
      snapshot_path(path, sizeof(path), dir, snap);
      TEST_ASSERT(slab_matches_file(path, &slab),
                  "every declared field should match a direct read of the file bit for bit");
    }
    horizontal_reader_release_slab(reader(), &slab);
    TEST_ASSERT(snapshot_slab_is_empty(&slab), "release should return the handle to empty");
    TEST_ASSERT(slab.descendant_snapshot == NULL && slab.source_halo_id == NULL,
                "release should clear the version 3 slab arrays");
  }

  horizontal_reader_close_run(reader());
  remove_staged_fixture(dir);
  TEST_ASSERT(no_tracked_leaks(), "load/release of every snapshot should leave no allocation");
  return TEST_PASS;
}

/* Count the generated read-list entries naming the fixture's undeclared extra. */
static int undeclared_extra_reads = 0;
#define READ_TREE_PROPERTY(field_name, hdf5_name, type_int, data_type)                             \
  undeclared_extra_reads += strcmp(hdf5_name, "SubHalfMass") == 0
#define READ_TREE_PROPERTY_MULTIPLEDIM(field_name, hdf5_name, type_int, data_type)                 \
  READ_TREE_PROPERTY(field_name, hdf5_name, type_int, data_type)
static void count_undeclared_extra_reads(void) {
#include "../../src/include/generated/read_tree_hdf5_properties.inc"
}
#undef READ_TREE_PROPERTY
#undef READ_TREE_PROPERTY_MULTIPLEDIM

/**
 * @test  test_v3_undeclared_extra_is_not_materialised
 * The fixture's selected extra SubHalfMass, which the package
 * does not declare, does not stop open_run (the acceptance test above) and is
 * absent from the read list that fills struct RawHalo, so no slab carries it.
 * Its declaration is still validated: see the undeclared-field cases in
 * test_v3_corrupt_inputs_abort.
 */
int test_v3_undeclared_extra_is_not_materialised(void) {
  REQUIRE_FIXTURE_PACKAGE();
  undeclared_extra_reads = 0;
  count_undeclared_extra_reads();
  TEST_ASSERT_EQUAL(undeclared_extra_reads, 0,
                    "the package's read list should not name the undeclared extra");

  char path[MAX_STRING_LEN];
  snapshot_path(path, sizeof(path), FIXTURE_DIR, 0);
  hid_t file = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
  TEST_ASSERT(file >= 0, "should open the committed fixture");
  const htri_t declared = H5Lexists(file, "/schema/SubHalfMass", H5P_DEFAULT);
  const htri_t present = H5Lexists(file, "/halos/SubHalfMass", H5P_DEFAULT);
  H5Fclose(file);
  TEST_ASSERT(declared > 0 && present > 0,
              "the fixture should carry SubHalfMass in both /schema and /halos");
  return TEST_PASS;
}

/**
 * @test  test_int_link_package_rejects_v3
 * Under a package whose links are `int` (every package but
 * mini-millennium-horizontal today), a version 3 file is refused at open
 * rather than narrowing its int64 links.
 */
int test_int_link_package_rejects_v3(void) {
  if (sizeof(((struct RawHalo *)0)->Descendant) != sizeof(int32_t)) {
    return TEST_SKIP_WITH("the compiled package stores links as long long");
  }
  char dir[MAX_STRING_LEN];
  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  TEST_ASSERT(expect_fatal(dir, child_open_run, "(dataset '/halos/Descendant') as 'int'",
                           "would narrow it") == 1,
              "an int-link package should refuse a version 3 dataset at open");
  remove_staged_fixture(dir);
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Open-time corruption
 * ------------------------------------------------------------------------- */

struct corrupt_case {
  const char *label;
  int (*corrupt)(const char *dir);
  const char *needle_a;
  const char *needle_b;
};

#define SNAP_FILE(dir, snap, path)                                                                 \
  char path[MAX_STRING_LEN];                                                                       \
  snapshot_path(path, sizeof(path), dir, snap)

static int c_version_1(const char *dir) { return set_attr_i32_all(dir, "format_version", 1); }
static int c_version_unknown(const char *dir) { return set_attr_i32_all(dir, "format_version", 7); }
static int c_version_mixed(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_attr_i32(p, "format_version", 2);
}
static int c_extra_root_object(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return create_group(p, "/bogus");
}
static int c_missing_schema(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return delete_link(p, "/schema");
}
static int c_schema_soft_link(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return relink(p, "/schema", "/zz_schema", 0);
}
static int c_halos_external_link(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return relink(p, "/halos", "/zz_halos", 1);
}
static int c_declared_halos_member_soft(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return relink(p, "/halos/Vmax", "/halos/zz_Vmax", 0);
}
static int c_undeclared_halos_member_external(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return relink(p, "/halos/SubHalfMass", "/halos/zz_SubHalfMass", 1);
}
static int c_declared_schema_member_external(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return relink(p, "/schema/M_Crit200", "/schema/zz_M_Crit200", 1);
}
static int c_undeclared_schema_member_soft(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return relink(p, "/schema/SubHalfMass", "/schema/zz_SubHalfMass", 0);
}
static int c_extra_header_attr(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return add_int_attr(p, "/header", "bogus_attr");
}
static int c_missing_source_format(const char *dir) {
  SNAP_FILE(dir, 2, p);
  return delete_header_attr(p, "source_format");
}
static int c_source_format_vlen(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return replace_string_attr(p, "/header", "source_format", "lhalo_binary", 0, H5T_CSET_ASCII);
}
static int c_source_format_unknown(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_header_string(p, "source_format", "gadget_subfind", 32);
}
static int c_source_format_differs(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return set_header_string(p, "source_format", "consistent_trees_hdf5", 32);
}
static int c_digest_differs(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_header_string(p, "column_mapping_sha256",
                           "0000000000000000000000000000000000000000000000000000000000000000", 64);
}
static int c_digest_not_hex(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_header_string(p, "column_mapping_sha256",
                           "DB55A0B67806DA507789EE60F9057DED4285E05D0D0DF57564A52F765CC43406", 64);
}
static int c_links_adjacent_two(const char *dir) {
  return set_attr_i32_all(dir, "links_adjacent", 2);
}
static int c_links_adjacent_differs(const char *dir) {
  SNAP_FILE(dir, 2, p);
  return set_attr_i32(p, "links_adjacent", 1);
}
static int c_snapshot_number(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_attr_i32(p, "snapshot_number", 5);
}
static int c_n_halos_negative(const char *dir) {
  SNAP_FILE(dir, 2, p);
  return set_attr_i64(p, "n_halos", -1);
}
static int c_n_halos_vs_length(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return set_attr_i64(p, "n_halos", 2);
}
static int c_scale_factor(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_attr_f64(p, "scale_factor", 0.5);
}
static int c_box_size(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return set_attr_f64(p, "box_size_mpc_h", 500.0);
}
static int c_omega_matter(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_attr_f64(p, "omega_matter", 0.3);
}
static int c_omega_lambda(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_attr_f64(p, "omega_lambda", 0.7);
}
static int c_hubble_h(const char *dir) {
  SNAP_FILE(dir, 2, p);
  return set_attr_f64(p, "hubble_h", 0.7);
}
static int c_particle_mass(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return set_attr_f64(p, "particle_mass_msun_h", 1.0e9);
}
static int c_n_forests_differs(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return set_attr_i64(p, "n_forests_total", 5);
}
static int c_max_rank_differs(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_attr_i64(p, "max_halo_rank_in_forest", 9);
}
static int c_max_rank_vs_data(const char *dir) {
  return set_attr_i64_all(dir, "max_halo_rank_in_forest", 5);
}
static int c_snapnum_value(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return set_i32_element(p, "SnapNum", 2, 1);
}
static int c_package_units(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_schema_attr(p, "M_Crit200", "units", "Msun/h");
}
static int c_package_h_convention(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_schema_attr(p, "Pos", "h_convention", "free");
}
static int c_package_type(const char *dir) {
  /* Internally consistent (a float64 dataset declared 'double') but not what
     the package declares. */
  SNAP_FILE(dir, 0, p);
  if (set_schema_attr(p, "Vmax", "type", "double") != 0) {
    return -1;
  }
  return recreate_dataset_in(p, "Vmax", H5T_IEEE_F64LE, 2, 0);
}
static int c_declared_missing_from_schema(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return delete_link(p, "/schema/Vmax");
}
static int c_declared_missing_from_halos(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return delete_link(p, "/halos/Vmax");
}
static int c_declared_dtype(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return recreate_dataset_in(p, "M_Crit200", H5T_IEEE_F64LE, 3, 0);
}
static int c_declared_shape(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return recreate_dataset_in(p, "Pos", H5T_IEEE_F32LE, 3, 4);
}
static int c_schema_differs(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_schema_attr(p, "Vmax", "description", "a different description");
}
static int c_undeclared_type_vs_dataset(const char *dir) {
  return set_schema_attr_all(dir, "SubHalfMass", "type", "int");
}
static int c_undeclared_h_vocabulary(const char *dir) {
  return set_schema_attr_all(dir, "SubHalfMass", "h_convention", "sometimes");
}
static int c_undeclared_type_vocabulary(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_schema_attr(p, "SubHalfMass", "type", "quad");
}
static int c_schema_extra_attr(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return add_int_attr(p, "/schema/Len", "bogus");
}
static int c_schema_attr_fixed_length(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return replace_string_attr(p, "/schema/Len", "units", "particles", 16, H5T_CSET_UTF8);
}
static int c_schema_child(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return create_group(p, "/schema/Len/child");
}
static int c_schema_redeclares_topology(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return add_schema_group(p, "SourceHaloID", "long long");
}
static int c_schema_without_dataset(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return add_schema_group(p, "Bogus", "int");
}
static int c_halos_without_schema(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return recreate_dataset_in(p, "Bogus", H5T_STD_I32LE, 2, 0);
}
static int c_payload_fixed_type(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_schema_attr(p, "Len", "type", "long long");
}
static int c_payload_shape(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_schema_attr(p, "Vmax", "type", "vec3_float");
}
static int c_fixed_dtype(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return recreate_dataset_in(p, "Descendant", H5T_STD_I32LE, 2, 0);
}
static int c_fixed_missing(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return delete_link(p, "/halos/NextProgenitorSnapshot");
}
static int c_header_soft_link(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return relink(p, "/header", "/zz_header", 0);
}
static int c_format_version_int64(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return replace_header_attr_with_i64(p, "format_version", 3);
}
static int c_forest_index_out_of_range(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_i64_element(p, "ForestIndex", 1, 9);
}
static int c_forest_index_max_low(const char *dir) {
  /* Every header agrees on 3 forests, but the data only reaches forest 1. */
  return set_attr_i64_all(dir, "n_forests_total", 3);
}
static int c_halo_rank_negative(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_i64_element(p, "HaloRankInForest", 0, -1);
}
static int c_empty_sentinel_with_halos(const char *dir) {
  return set_attr_i64_all(dir, "n_forests_total", 0) == 0 &&
                 set_attr_i64_all(dir, "max_halo_rank_in_forest", -1) == 0
             ? 0
             : -1;
}
static int c_zero_forests_with_halos(const char *dir) {
  return set_attr_i64_all(dir, "n_forests_total", 0);
}
static int c_schema_member_dataset(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return create_scalar_dataset(p, "/schema/Bogus");
}
static int c_schema_attr_misnamed(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return rename_attr(p, "/schema/Len", "units", "unit");
}
static int c_schema_attr_ascii_vlen(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return replace_string_attr(p, "/schema/Len", "units", "particles", 0, H5T_CSET_ASCII);
}
static int c_digest_wrong_length(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_header_string(p, "column_mapping_sha256",
                           "db55a0b67806da507789ee60f9057ded4285e05d0d0df57564a52f765cc434", 63);
}
static int c_header_string_not_null_padded(const char *dir) {
  /* 'lhalo_binary', a NUL, then a stray 'X': text ends at the NUL but a later byte is set. */
  SNAP_FILE(dir, 0, p);
  char bytes[32];
  memset(bytes, 0, sizeof(bytes));
  memcpy(bytes, "lhalo_binary", 12);
  bytes[20] = 'X';
  return set_header_raw_bytes(p, "source_format", bytes);
}
static int c_header_string_non_printable(const char *dir) {
  SNAP_FILE(dir, 0, p);
  char bytes[32];
  memset(bytes, 0, sizeof(bytes));
  memcpy(bytes, "lha\x01o_binary", 12);
  return set_header_raw_bytes(p, "source_format", bytes);
}
static int c_snapshot_file_missing(const char *dir) {
  SNAP_FILE(dir, 2, p);
  return unlink(p);
}
static int c_wide_n_halos(const char *dir) {
  /* 2^31 + 5 rows in the empty snapshot 2: every structural check passes and
     the bounded SnapNum scan reads the unwritten fill value 0 at halo 0. */
  SNAP_FILE(dir, 2, p);
  return widen_snapshot(p, (int64_t)INT32_MAX + 6);
}

static const struct corrupt_case OPEN_CASES[] = {
    {"version 1 dataset", c_version_1, "snapshot_000.h5: header attribute 'format_version' is 1",
     "must be reconverted from source"},
    {"unknown version", c_version_unknown,
     "snapshot_000.h5: header attribute 'format_version' is 7", "supports only versions 2 and 3"},
    {"mixed-version dataset", c_version_mixed,
     "snapshot_001.h5: header attribute 'format_version' "
     "is 2 but snapshot 0 declares version 3",
     "never mixes format versions"},
    {"unexpected root object", c_extra_root_object, "snapshot_001.h5",
     "unexpected root object 'bogus'"},
    {"missing /schema", c_missing_schema, "snapshot_003.h5", "required group '/schema' is missing"},
    {"/schema is a soft link", c_schema_soft_link, "snapshot_000.h5: '/schema' is a soft link",
     NULL},
    {"/halos is an external link", c_halos_external_link,
     "snapshot_003.h5: '/halos' is an external link", NULL},
    {"declared /halos member is a soft link", c_declared_halos_member_soft,
     "'/halos/Vmax' is a soft link", NULL},
    {"undeclared /halos member is an external link", c_undeclared_halos_member_external,
     "'/halos/SubHalfMass' is an external link", NULL},
    {"declared /schema member is an external link", c_declared_schema_member_external,
     "'/schema/M_Crit200' is an external link", NULL},
    {"undeclared /schema member is a soft link", c_undeclared_schema_member_soft,
     "'/schema/SubHalfMass' is a soft link", NULL},
    {"unexpected header attribute", c_extra_header_attr, "snapshot_001.h5",
     "unexpected attribute 'bogus_attr'"},
    {"missing source_format", c_missing_source_format, "snapshot_002.h5",
     "required header attribute 'source_format' is missing"},
    {"variable-length source_format", c_source_format_vlen, "snapshot_000.h5",
     "must be a fixed-length ASCII string of exactly 32 bytes"},
    {"unknown source_format", c_source_format_unknown, "'source_format' is 'gadget_subfind'", NULL},
    {"source_format differs across files", c_source_format_differs, "snapshot_003.h5",
     "'source_format' is 'consistent_trees_hdf5' but snapshot 0 declares 'lhalo_binary'"},
    {"column_mapping_sha256 differs across files", c_digest_differs, "snapshot_001.h5",
     "'column_mapping_sha256' is '0000"},
    {"column_mapping_sha256 not lowercase hex", c_digest_not_hex, "snapshot_000.h5",
     "lowercase hexadecimal digits"},
    {"links_adjacent outside 0 or 1", c_links_adjacent_two, "'links_adjacent' is 2",
     "requires 0 or 1"},
    {"links_adjacent differs across files", c_links_adjacent_differs, "snapshot_002.h5",
     "'links_adjacent' is 1 but snapshot 0 declares 0"},
    {"snapshot_number disagrees with the filename", c_snapshot_number, "snapshot_001.h5",
     "'snapshot_number' is 5"},
    {"negative n_halos", c_n_halos_negative, "snapshot_002.h5", "'n_halos' is -1"},
    {"n_halos disagrees with a dataset length", c_n_halos_vs_length, "snapshot_003.h5",
     "has length 3 but header n_halos is 2"},
    {"scale_factor disagrees with the a_list", c_scale_factor, "snapshot_001.h5",
     "'scale_factor' is 0.5"},
    {"box_size_mpc_h disagrees with the package", c_box_size, "snapshot_003.h5",
     "'box_size_mpc_h' is 500 but the configured simulation value is 62.5"},
    {"omega_matter disagrees with the package", c_omega_matter, "snapshot_000.h5",
     "'omega_matter' is 0.29999999999999999 but the configured simulation value is 0.25"},
    {"omega_lambda disagrees with the package", c_omega_lambda, "snapshot_001.h5",
     "'omega_lambda' is 0.69999999999999996 but the configured simulation value is 0.75"},
    {"hubble_h disagrees with the package", c_hubble_h, "snapshot_002.h5",
     "'hubble_h' is 0.69999999999999996 but the configured simulation value is "
     "0.72999999999999998"},
    {"particle_mass_msun_h disagrees with the package", c_particle_mass, "snapshot_003.h5",
     "'particle_mass_msun_h' is 1000000000 but the configured simulation value is 860657000"},
    {"n_forests_total differs across files", c_n_forests_differs, "snapshot_003.h5",
     "'n_forests_total' is 5 but snapshot 0 declares 2"},
    {"max_halo_rank_in_forest differs across files", c_max_rank_differs, "snapshot_001.h5",
     "'max_halo_rank_in_forest' is 9 but snapshot 0 declares 3"},
    {"max_halo_rank_in_forest disagrees with the data", c_max_rank_vs_data,
     "declares max_halo_rank_in_forest 5 but the measured maximum", NULL},
    {"SnapNum disagrees with the header", c_snapnum_value, "snapshot_003.h5",
     "'/halos/SnapNum' is 1 at halo 2"},
    {"declared field units disagree with the package", c_package_units, "snapshot_000.h5",
     "'/schema/M_Crit200' declares units 'Msun/h' but simulation package "
     "'mini-millennium-horizontal' declares 'M_Crit200' as '1e10 Msun/h'"},
    {"declared field h_convention disagrees with the package", c_package_h_convention,
     "'/schema/Pos' declares h_convention 'free'", "declares 'Pos' as 'carried'"},
    {"declared field type disagrees with the package", c_package_type,
     "'/schema/Vmax' declares type 'double'", "declares 'Vmax' as 'float'"},
    {"declared field missing from /schema", c_declared_missing_from_schema, "snapshot_000.h5",
     "'/schema' does not declare payload field 'Vmax'"},
    {"declared field missing from /halos", c_declared_missing_from_halos, "snapshot_003.h5",
     "'/schema' declares 'Vmax' but dataset '/halos/Vmax' is missing"},
    {"declared field dataset of the wrong dtype", c_declared_dtype, "snapshot_003.h5",
     "dataset '/halos/M_Crit200' must be float32"},
    {"declared field dataset of the wrong shape", c_declared_shape, "snapshot_003.h5",
     "dataset '/halos/Pos' must have shape [3, 3]"},
    {"/schema differs across files", c_schema_differs, "snapshot_001.h5",
     "'/schema/Vmax' attribute 'description' is 'a different description'"},
    {"undeclared extra whose dataset disagrees with its own type", c_undeclared_type_vs_dataset,
     "dataset '/halos/SubHalfMass' must be int32", "'/schema/SubHalfMass' declares type 'int'"},
    {"undeclared extra with an h_convention outside the vocabulary", c_undeclared_h_vocabulary,
     "'/schema/SubHalfMass' declares h_convention 'sometimes'", NULL},
    {"undeclared extra with a type outside the vocabulary", c_undeclared_type_vocabulary,
     "'/schema/SubHalfMass' declares type 'quad'", NULL},
    {"/schema subgroup with a fifth attribute", c_schema_extra_attr, "'/schema/Len' carries 5",
     NULL},
    {"/schema attribute that is not variable-length", c_schema_attr_fixed_length,
     "attribute 'units' of '/schema/Len' must be a variable-length UTF-8 string", NULL},
    {"/schema subgroup with a child", c_schema_child, "'/schema/Len' has children", NULL},
    {"/schema redeclares an identity field", c_schema_redeclares_topology,
     "'/schema/SourceHaloID' declares a topology or identity field", NULL},
    {"/schema declaration without a dataset", c_schema_without_dataset,
     "'/schema' declares 'Bogus' but dataset '/halos/Bogus' is missing", NULL},
    {"/halos dataset without a declaration", c_halos_without_schema, "snapshot_001.h5",
     "unexpected dataset '/halos/Bogus'"},
    {"payload field with a type other than the format fixes", c_payload_fixed_type,
     "'/schema/Len' declares type 'long long'", "fixes it as 'int'"},
    {"payload field with a shape other than the format fixes", c_payload_shape,
     "'/schema/Vmax' declares type 'vec3_float'", "requires a scalar [n_halos] field"},
    {"fixed-table dataset of the wrong dtype", c_fixed_dtype, "snapshot_001.h5",
     "dataset '/halos/Descendant' must be int64"},
    {"missing fixed-table dataset", c_fixed_missing, "snapshot_003.h5",
     "required dataset '/halos/NextProgenitorSnapshot' is missing"},
    {"snapshot 0 /header is a soft link", c_header_soft_link, "snapshot_000.h5: snapshot 0 carries",
     "'/header' is a soft link"},
    {"snapshot 0 format_version stored as int64", c_format_version_int64, "snapshot_000.h5",
     "header attribute 'format_version' is not an int32"},
    {"ForestIndex outside [0, n_forests_total)", c_forest_index_out_of_range, "snapshot_000.h5",
     "'/halos/ForestIndex' is 9 at halo 1; the permitted range is [0, 1]"},
    {"measured ForestIndex maximum disagrees with n_forests_total", c_forest_index_max_low,
     "declares n_forests_total 3 but the measured maximum of '/halos/ForestIndex' is 1",
     "it must be 2"},
    {"negative HaloRankInForest", c_halo_rank_negative, "snapshot_000.h5",
     "'/halos/HaloRankInForest' is -1 at halo 0; the permitted range is [0, "},
    {"empty-dataset sentinel with halos present", c_empty_sentinel_with_halos, "snapshot_000.h5",
     "carries the empty-dataset sentinel (n_forests_total 0, max_halo_rank_in_forest -1) but "
     "declares 2 halos"},
    {"n_forests_total 0 with halos present", c_zero_forests_with_halos, "snapshot_000.h5",
     "header attribute 'n_forests_total' is 0 but the file declares 2 halos"},
    {"/schema member that is a dataset", c_schema_member_dataset, "snapshot_000.h5",
     "'/schema/Bogus' must be a group"},
    {"/schema attribute with a wrong name", c_schema_attr_misnamed, "snapshot_000.h5",
     "'/schema/Len' is missing its required attribute 'units'"},
    {"/schema attribute that is an ASCII variable-length string", c_schema_attr_ascii_vlen,
     "snapshot_000.h5",
     "attribute 'units' of '/schema/Len' must be a variable-length UTF-8 string"},
    {"column_mapping_sha256 of the wrong fixed length", c_digest_wrong_length, "snapshot_000.h5",
     "header attribute 'column_mapping_sha256' must be a fixed-length ASCII string of exactly 64 "
     "bytes; found HDF5 type class 3 of 63 bytes"},
    {"header string with a byte after its NUL", c_header_string_not_null_padded, "snapshot_000.h5",
     "header attribute 'source_format' has a non-NUL byte after its NUL terminator"},
    {"header string with a non-printable byte", c_header_string_non_printable, "snapshot_000.h5",
     "header attribute 'source_format' contains a non-printable byte 0x01 at offset 3"},
    {"missing snapshot file", c_snapshot_file_missing,
     "snapshot_002.h5: no readable file for configured snapshot 2",
     "every snapshot in the snapshot list must have a snapshot_NNN.h5 file"},
    {"n_halos above INT32_MAX has no int32 ceiling", c_wide_n_halos,
     "snapshot_002.h5: '/halos/SnapNum' is 0 at halo 0 but the header snapshot_number is 2", NULL},
};
#define OPEN_CASE_COUNT (sizeof(OPEN_CASES) / sizeof(OPEN_CASES[0]))

/**
 * @test  test_v3_corrupt_inputs_abort
 * One open-time abort per version 3 invariant a consumer checks, each naming
 * the file and the offending object or value.
 */
int test_v3_corrupt_inputs_abort(void) {
  REQUIRE_FIXTURE_PACKAGE();
  for (size_t i = 0; i < OPEN_CASE_COUNT; i++) {
    const struct corrupt_case *test_case = &OPEN_CASES[i];
    char dir[MAX_STRING_LEN];
    TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
    if (test_case->corrupt(dir) != 0) {
      fprintf(stderr, "  case '%s': the mutation itself failed\n", test_case->label);
      remove_staged_fixture(dir);
      TEST_ASSERT(0, "every corruption should apply");
    }
    const int aborted = expect_fatal(dir, child_open_run, test_case->needle_a, test_case->needle_b);
    remove_staged_fixture(dir);
    if (aborted != 1) {
      fprintf(stderr, "  case '%s' did not abort as expected\n", test_case->label);
    }
    TEST_ASSERT(aborted == 1, "every corrupt version 3 input should abort open_run by name");
  }
  return TEST_PASS;
}

/**
 * @test  test_v3_open_run_checks_identity_bounds
 * The identity-multiplier bound is checked against the version 3 header: a
 * multiplier that cannot encode the maximum rank aborts at open.
 */
int test_v3_open_run_checks_identity_bounds(void) {
  REQUIRE_FIXTURE_PACKAGE();
  char dir[MAX_STRING_LEN];
  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  TEST_ASSERT(expect_fatal(dir, child_open_run_small_multiplier, "not encodable",
                           "unique_galaxy_id_multiplier 3") == 1,
              "an unencodable multiplier should abort open_run");
  remove_staged_fixture(dir);
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Load-time link validation
 * ------------------------------------------------------------------------- */

struct link_case {
  const char *label;
  int (*corrupt)(const char *dir);
  int64_t snapnum;
  const char *needle_a;
  const char *needle_b;
};

static int l_desc_without_target(const char *dir) {
  /* Row 1, which has no NextProgenitor, so only the Descendant field offends. */
  SNAP_FILE(dir, 0, p);
  return set_i32_element(p, "DescendantSnapshot", 1, -1);
}
static int l_target_without_desc(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_i32_element(p, "DescendantSnapshot", 1, 3);
}
static int l_fp_target_without_link(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return set_i32_element(p, "FirstProgenitorSnapshot", 1, 0);
}
static int l_np_link_without_target(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_i32_element(p, "NextProgenitorSnapshot", 0, -1);
}
static int l_desc_outside_target(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_i64_element(p, "Descendant", 0, 3);
}
static int l_fp_outside_target(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return set_i64_element(p, "FirstProgenitor", 0, 2);
}
static int l_np_outside_target(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_i64_element(p, "NextProgenitor", 0, 2);
}
static int l_target_outside_dataset(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_i32_element(p, "DescendantSnapshot", 0, 4);
}
static int l_desc_backward(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_i32_element(p, "DescendantSnapshot", 0, 0);
}
static int l_fp_forward(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_i32_element(p, "FirstProgenitorSnapshot", 1, 3);
}
static int l_np_at_shared_descendant(const char *dir) {
  SNAP_FILE(dir, 0, p);
  return set_i32_element(p, "NextProgenitorSnapshot", 0, 3);
}
static int l_np_without_descendant(const char *dir) {
  SNAP_FILE(dir, 1, p);
  if (set_i64_element(p, "NextProgenitor", 1, 0) != 0) {
    return -1;
  }
  return set_i32_element(p, "NextProgenitorSnapshot", 1, 0);
}
static int l_adjacency_claim(const char *dir) { return set_attr_i32_all(dir, "links_adjacent", 1); }
static int l_fof_first_null(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return set_i64_element(p, "FirstHaloInFOFgroup", 2, -1);
}
static int l_fof_next_outside(const char *dir) {
  SNAP_FILE(dir, 3, p);
  return set_i64_element(p, "NextHaloInFOFgroup", 1, 3);
}
static int l_link_below_null(const char *dir) {
  SNAP_FILE(dir, 1, p);
  return set_i64_element(p, "FirstProgenitor", 1, -2);
}

static const struct link_case LINK_CASES[] = {
    {"Descendant without DescendantSnapshot", l_desc_without_target, 0,
     "snapshot_000.h5: snapshot 0 has 1 invalid link field(s)",
     "'Descendant' is 1 but 'DescendantSnapshot' is -1; each is -1 if and only if the other is"},
    {"DescendantSnapshot without Descendant", l_target_without_desc, 1,
     "'Descendant' is -1 but 'DescendantSnapshot' is 3", NULL},
    {"FirstProgenitorSnapshot without FirstProgenitor", l_fp_target_without_link, 3,
     "'FirstProgenitor' is -1 but 'FirstProgenitorSnapshot' is 0", NULL},
    {"NextProgenitor without NextProgenitorSnapshot", l_np_link_without_target, 0,
     "'NextProgenitor' is 0 but 'NextProgenitorSnapshot' is -1", NULL},
    {"Descendant outside its target file", l_desc_outside_target, 0,
     "'Descendant' is 3, outside [0, 3) of snapshot 3 named by 'DescendantSnapshot'", NULL},
    {"FirstProgenitor outside its target file", l_fp_outside_target, 3,
     "'FirstProgenitor' is 2, outside [0, 2) of snapshot 0 named by 'FirstProgenitorSnapshot'",
     NULL},
    {"NextProgenitor outside its target file", l_np_outside_target, 0,
     "'NextProgenitor' is 2, outside [0, 2) of snapshot 1 named by 'NextProgenitorSnapshot'", NULL},
    {"target snapshot outside the dataset", l_target_outside_dataset, 0,
     "'DescendantSnapshot' is 4, which names no snapshot of the dataset [0, 4)", NULL},
    {"descendant not strictly later", l_desc_backward, 1,
     "'DescendantSnapshot' is 0; a descendant lies in a strictly later snapshot than 1", NULL},
    {"progenitor not strictly earlier", l_fp_forward, 1,
     "'FirstProgenitorSnapshot' is 3; a progenitor lies in a strictly earlier snapshot than 1",
     NULL},
    {"NextProgenitor not before the shared descendant", l_np_at_shared_descendant, 0,
     "'NextProgenitorSnapshot' is 3 but the owner's 'DescendantSnapshot' is 3", NULL},
    {"NextProgenitor on a halo with no descendant", l_np_without_descendant, 1,
     "'NextProgenitor' is 0 but the halo has no descendant", NULL},
    {"links_adjacent = 1 with a gapped descendant", l_adjacency_claim, 0,
     "'DescendantSnapshot' is 3 but links_adjacent = 1 promises every descendant in snapshot 1",
     NULL},
    {"null FirstHaloInFOFgroup", l_fof_first_null, 3,
     "'FirstHaloInFOFgroup' is -1; it is never null", NULL},
    {"NextHaloInFOFgroup outside this snapshot", l_fof_next_outside, 3,
     "'NextHaloInFOFgroup' is 3, outside [0, 3) of snapshot 3", NULL},
    {"negative link other than -1", l_link_below_null, 1,
     "'FirstProgenitor' is -2; only -1 is null", NULL},
};
#define LINK_CASE_COUNT (sizeof(LINK_CASES) / sizeof(LINK_CASES[0]))

/**
 * @test  test_v3_corrupt_links_abort
 * One load-time abort per link invariant: the three biconditionals, each link
 * against the n_halos of the file its target column names, a target outside
 * the dataset, the direction rules, links_adjacent's claim, and the FoF and
 * null-value rules. Open succeeds in every case: these are data, not structure.
 */
int test_v3_corrupt_links_abort(void) {
  REQUIRE_FIXTURE_PACKAGE();
  for (size_t i = 0; i < LINK_CASE_COUNT; i++) {
    const struct link_case *test_case = &LINK_CASES[i];
    char dir[MAX_STRING_LEN];
    TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
    if (test_case->corrupt(dir) != 0) {
      fprintf(stderr, "  case '%s': the mutation itself failed\n", test_case->label);
      remove_staged_fixture(dir);
      TEST_ASSERT(0, "every corruption should apply");
    }
    child_load_snapnum = test_case->snapnum;
    const int aborted =
        expect_fatal(dir, child_load_slab, test_case->needle_a, test_case->needle_b);
    remove_staged_fixture(dir);
    if (aborted != 1) {
      fprintf(stderr, "  case '%s' did not abort as expected\n", test_case->label);
    }
    TEST_ASSERT(aborted == 1, "every invalid version 3 link should abort load_slab by name");
  }
  return TEST_PASS;
}

static int l_desc_in_gapped_target_only(const char *dir) {
  /* Row 1 of snapshot 0 descends to snapshot 1, which holds 2 halos. Moving
     its target to snapshot 3 (3 halos) with index 2 is in range there even
     though index 2 is out of range of N+1: the bound must come from the file
     the target column names. Topology is not re-derived at load. */
  SNAP_FILE(dir, 0, p);
  if (set_i32_element(p, "DescendantSnapshot", 1, 3) != 0) {
    return -1;
  }
  return set_i64_element(p, "Descendant", 1, 2);
}

/**
 * @test  test_v3_links_bound_by_named_target
 * A link index valid in the snapshot its target column names but not in N+1
 * loads: the reader never assumes adjacency when links_adjacent is 0.
 */
int test_v3_links_bound_by_named_target(void) {
  REQUIRE_FIXTURE_PACKAGE();
  char dir[MAX_STRING_LEN];
  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  TEST_ASSERT(l_desc_in_gapped_target_only(dir) == 0, "the mutation should apply");
  child_load_snapnum = 0;
  TEST_ASSERT(expect_success(dir, child_load_slab) == 1,
              "a link in range of its named target snapshot should load");
  remove_staged_fixture(dir);
  return TEST_PASS;
}

/** @brief Occurrences of `needle` in `haystack`. */
static int count_occurrences(const char *haystack, const char *needle) {
  int count = 0;
  for (const char *at = strstr(haystack, needle); at != NULL; at = strstr(at + 1, needle)) {
    count++;
  }
  return count;
}

static int l_every_row_bad(const char *dir) {
  SNAP_FILE(dir, 3, p);
  for (hsize_t row = 0; row < 3; row++) {
    if (set_i64_element(p, "FirstHaloInFOFgroup", row, -1) != 0 ||
        set_i64_element(p, "NextHaloInFOFgroup", row, 7) != 0) {
      return -1;
    }
  }
  return 0;
}

/**
 * @test  test_v3_link_diagnostics_are_bounded
 * Every halo of a snapshot offending on two link fields yields exactly one
 * counted summary line per field and one FATAL naming the field count.
 */
int test_v3_link_diagnostics_are_bounded(void) {
  REQUIRE_FIXTURE_PACKAGE();
  char dir[MAX_STRING_LEN];
  static char captured[65536];
  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  TEST_ASSERT(l_every_row_bad(dir) == 0, "the mutation should apply");
  child_load_snapnum = 3;
  TEST_ASSERT(expect_fatal_capture(dir, child_load_slab, "has 2 invalid link field(s)", NULL,
                                   captured, sizeof(captured)) == 1,
              "the load should abort naming two invalid fields");
  remove_staged_fixture(dir);
  TEST_ASSERT_EQUAL(count_occurrences(captured, "has 3 halo(s) with an invalid "
                                                "'FirstHaloInFOFgroup'"),
                    1, "one counted summary line for FirstHaloInFOFgroup");
  TEST_ASSERT_EQUAL(count_occurrences(captured, "has 3 halo(s) with an invalid "
                                                "'NextHaloInFOFgroup'"),
                    1, "one counted summary line for NextHaloInFOFgroup");
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Row ranges, the ForestIndex scan and open options
 *
 * Run on both committed version 3 fixtures: this suite's gapped two-forest
 * dataset and the one-forest worked graph of the mini-millennium-horizontal
 * package, both converted from L-Halo binary. Both are read in place: these
 * cases only read, so neither needs a scratch copy.
 * ------------------------------------------------------------------------- */

#define WORKED_GRAPH_DIR "simulations/mini-millennium-horizontal/_tests/data/worked_graph"
#define WORKED_GRAPH_A_LIST "worked_graph.a_list"
#define WORKED_GRAPH_N_FORESTS_TOTAL 1
#define WORKED_GRAPH_MAX_RANK 4
#define FIXTURE_SOURCE_FORMAT "lhalo_binary"

struct v3_dataset {
  const char *dir;
  const char *a_list;
  int64_t n_forests_total;
  int64_t max_halo_rank_in_forest;
};

static const struct v3_dataset V3_DATASETS[] = {
    {FIXTURE_DIR, FIXTURE_A_LIST, FIXTURE_N_FORESTS_TOTAL, FIXTURE_MAX_RANK},
    {WORKED_GRAPH_DIR, WORKED_GRAPH_A_LIST, WORKED_GRAPH_N_FORESTS_TOTAL, WORKED_GRAPH_MAX_RANK},
};
#define V3_DATASET_COUNT (sizeof(V3_DATASETS) / sizeof(V3_DATASETS[0]))

/** @brief Point MimicConfig at one of the committed datasets, in place. */
static void configure_for_dataset(const struct v3_dataset *dataset) {
  configure_for_dataset_dir(dataset->dir, dataset->a_list);
}

/* Compare every generated struct RawHalo member of two halos by value, member
   by member, so padding never enters the comparison; vector members compare
   all NDIM components through sizeof. Walks the same generated read list that
   fills the slab, so a package's every catalog field is covered. */
#define READ_TREE_PROPERTY(field_name, hdf5_name, type_int, data_type)                             \
  if (memcmp(&a->field_name, &b->field_name, sizeof(a->field_name)) != 0) {                        \
    fprintf(stderr, "  member '%s' differs\n", #field_name);                                       \
    return 0;                                                                                      \
  }
#define READ_TREE_PROPERTY_MULTIPLEDIM(field_name, hdf5_name, type_int, data_type)                 \
  READ_TREE_PROPERTY(field_name, hdf5_name, type_int, data_type)
static int raw_halos_equal(const struct RawHalo *a, const struct RawHalo *b) {
#include "../../src/include/generated/read_tree_hdf5_properties.inc"
  return 1;
}
#undef READ_TREE_PROPERTY
#undef READ_TREE_PROPERTY_MULTIPLEDIM

/** @brief Does `range` hold exactly rows [lo, lo + range->nhalos) of `whole`, every column? */
static int range_matches_whole(const struct SnapshotSlab *whole, const struct SnapshotSlab *range,
                               int64_t lo) {
  const size_t n = (size_t)range->nhalos;
  for (int64_t i = 0; i < range->nhalos; i++) {
    if (!raw_halos_equal(&whole->halos[lo + i], &range->halos[i])) {
      fprintf(stderr, "  snapshot %" PRId64 " row %" PRId64 " differs from the whole read\n",
              whole->snapnum, lo + i);
      return 0;
    }
  }
  return memcmp(whole->forest_index + lo, range->forest_index, n * sizeof(int64_t)) == 0 &&
         memcmp(whole->halo_rank_in_forest + lo, range->halo_rank_in_forest, n * sizeof(int64_t)) ==
             0 &&
         memcmp(whole->descendant_snapshot + lo, range->descendant_snapshot, n * sizeof(int32_t)) ==
             0 &&
         memcmp(whole->first_progenitor_snapshot + lo, range->first_progenitor_snapshot,
                n * sizeof(int32_t)) == 0 &&
         memcmp(whole->next_progenitor_snapshot + lo, range->next_progenitor_snapshot,
                n * sizeof(int32_t)) == 0 &&
         memcmp(whole->source_halo_id + lo, range->source_halo_id, n * sizeof(int64_t)) == 0;
}

/** @brief Is every array of a loaded slab NULL, as an empty range leaves it? */
static int slab_arrays_null(const struct SnapshotSlab *slab) {
  return slab->halos == NULL && slab->forest_index == NULL && slab->halo_rank_in_forest == NULL &&
         slab->descendant_snapshot == NULL && slab->first_progenitor_snapshot == NULL &&
         slab->next_progenitor_snapshot == NULL && slab->source_halo_id == NULL;
}

/**
 * @test  test_v3_range_reads_match_whole_reads
 * On both fixtures, every row range [lo, hi) of every snapshot -- each empty
 * range [k, k) included -- loads exactly hi - lo halos with row_offset lo, and
 * every column (each generated RawHalo member, the vector members Pos, Vel and
 * Spin among them, the two identity columns, the three target-snapshot columns
 * and SourceHaloID) equals the matching rows of a whole read. Links stay the
 * global rows the file stores. An empty range carries only NULL arrays and
 * releases cleanly; nothing leaks.
 */
int test_v3_range_reads_match_whole_reads(void) {
  REQUIRE_FIXTURE_PACKAGE();
  for (size_t d = 0; d < V3_DATASET_COUNT; d++) {
    struct HorizontalRunInfo info;
    configure_for_dataset(&V3_DATASETS[d]);
    horizontal_reader_open_run(reader(), &FULL_SCAN, &info);

    for (int64_t snap = 0; snap < info.snapshot_count; snap++) {
      const int64_t count = horizontal_reader_halo_count(reader(), snap);
      struct SnapshotSlab whole = snapshot_slab_empty();
      load_whole_slab(snap, &whole);
      TEST_ASSERT_EQUAL(whole.row_offset, 0, "a whole-snapshot slab should have row_offset 0");
      TEST_ASSERT_EQUAL(whole.nhalos, count, "a whole-snapshot slab should hold every halo");

      for (int64_t lo = 0; lo <= count; lo++) {
        for (int64_t hi = lo; hi <= count; hi++) {
          struct SnapshotSlab range = snapshot_slab_empty();
          horizontal_reader_load_slab(reader(), snap, lo, hi, &range);
          TEST_ASSERT_EQUAL(range.snapnum, snap, "a range slab should carry its snapshot number");
          TEST_ASSERT_EQUAL(range.nhalos, hi - lo, "a range slab should hold row_hi - row_lo");
          TEST_ASSERT_EQUAL(range.row_offset, lo, "a range slab should publish row_offset");
          if (hi == lo) {
            TEST_ASSERT(!snapshot_slab_is_empty(&range), "an empty range is still a loaded slab");
            TEST_ASSERT(slab_arrays_null(&range), "an empty range should carry only NULL arrays");
          } else {
            TEST_ASSERT(range_matches_whole(&whole, &range, lo),
                        "every column of a range read should equal the whole read's rows");
          }
          horizontal_reader_release_slab(reader(), &range);
          TEST_ASSERT(snapshot_slab_is_empty(&range) && range.row_offset == 0,
                      "release should return a range slab to the empty state");
        }
      }
      horizontal_reader_release_slab(reader(), &whole);
    }
    horizontal_reader_close_run(reader());
  }
  TEST_ASSERT(no_tracked_leaks(), "range loads and releases should leave no allocation");
  return TEST_PASS;
}

/* Row range the range-bound children load; set by the parent before forking. */
static int64_t child_range_snapnum = 0;
static int64_t child_range_lo = 0;
static int64_t child_range_hi = 0;

static void child_load_range(const char *dir) {
  struct HorizontalRunInfo info;
  configure_for_fixture(dir);
  horizontal_reader_open_run(reader(), &FULL_SCAN, &info);
  struct SnapshotSlab slab = snapshot_slab_empty();
  horizontal_reader_load_slab(reader(), child_range_snapnum, child_range_lo, child_range_hi, &slab);
  horizontal_reader_release_slab(reader(), &slab);
  horizontal_reader_close_run(reader());
}

/**
 * @test  test_v3_load_slab_rejects_bad_ranges
 * A range that is not inside [0, snapshot_halo_count(snapnum)] aborts naming
 * the range and the count: row_hi above the count, row_lo above row_hi, and a
 * negative row_lo. The full range [0, count] is the accepted edge.
 */
int test_v3_load_slab_rejects_bad_ranges(void) {
  REQUIRE_FIXTURE_PACKAGE();
  static const struct {
    const char *label;
    int64_t snapnum, lo, hi;
    const char *needle;
  } cases[] = {
      {"row_hi above the count", 3, 0, 4, "rows [0, 4) of snapshot 3 are not a range of its 3"},
      {"row_hi above an empty snapshot's count", 2, 0, 1,
       "rows [0, 1) of snapshot 2 are not a range of its 0"},
      {"row_lo above row_hi", 3, 2, 1, "rows [2, 1) of snapshot 3 are not a range of its 3"},
      {"negative row_lo", 0, -1, 1, "rows [-1, 1) of snapshot 0 are not a range of its 2"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    child_range_snapnum = cases[i].snapnum;
    child_range_lo = cases[i].lo;
    child_range_hi = cases[i].hi;
    const int aborted = expect_fatal(FIXTURE_DIR, child_load_range, cases[i].needle,
                                     "0 <= row_lo <= row_hi <= the snapshot's halo count");
    if (aborted != 1) {
      fprintf(stderr, "  case '%s' did not abort as expected\n", cases[i].label);
    }
    TEST_ASSERT(aborted == 1, "a range outside the snapshot should abort load_slab");
  }
  child_range_snapnum = 3;
  child_range_lo = 0;
  child_range_hi = FIXTURE_HALO_COUNTS[3];
  TEST_ASSERT(expect_success(FIXTURE_DIR, child_load_range) == 1,
              "the full range [0, count) should load");
  return TEST_PASS;
}

/* What one scan_forest_index pass delivered. */
struct forest_scan_record {
  int64_t values[FIXTURE_MAX_HALOS];
  int64_t rows_seen;
  int calls;
  int out_of_order;
};

static void record_forest_index(int64_t first_row, const int64_t *values, int64_t count,
                                void *user) {
  struct forest_scan_record *record = user;
  record->calls++;
  if (first_row != record->rows_seen || count <= 0 ||
      record->rows_seen + count > FIXTURE_MAX_HALOS) {
    record->out_of_order = 1;
    return;
  }
  memcpy(record->values + first_row, values, (size_t)count * sizeof(int64_t));
  record->rows_seen += count;
}

/**
 * @test  test_v3_scan_forest_index_streams_column
 * On both fixtures, scan_forest_index delivers each snapshot's whole
 * ForestIndex column in ascending row order -- every block starting at the row
 * after the last one delivered, from row 0 -- equal to a whole load's
 * forest_index array; a snapshot with no halos makes no call; nothing leaks.
 */
int test_v3_scan_forest_index_streams_column(void) {
  REQUIRE_FIXTURE_PACKAGE();
  for (size_t d = 0; d < V3_DATASET_COUNT; d++) {
    struct HorizontalRunInfo info;
    configure_for_dataset(&V3_DATASETS[d]);
    horizontal_reader_open_run(reader(), &FULL_SCAN, &info);

    for (int64_t snap = 0; snap < info.snapshot_count; snap++) {
      const int64_t count = horizontal_reader_halo_count(reader(), snap);
      struct forest_scan_record record;
      memset(&record, 0, sizeof(record));
      horizontal_reader_scan_forest_index(reader(), snap, record_forest_index, &record);
      TEST_ASSERT(!record.out_of_order, "blocks should arrive contiguous and in row order");
      TEST_ASSERT_EQUAL(record.rows_seen, count, "the scan should deliver every row once");
      if (count == 0) {
        TEST_ASSERT_EQUAL(record.calls, 0, "a snapshot with no halos should make no call");
        continue;
      }

      struct SnapshotSlab whole = snapshot_slab_empty();
      load_whole_slab(snap, &whole);
      TEST_ASSERT(memcmp(record.values, whole.forest_index, (size_t)count * sizeof(int64_t)) == 0,
                  "the scanned column should equal the loaded ForestIndex column");
      horizontal_reader_release_slab(reader(), &whole);
    }
    horizontal_reader_close_run(reader());
  }
  TEST_ASSERT(no_tracked_leaks(), "scanning should leave no allocation");
  return TEST_PASS;
}

/* ---------------------------------------------------------------------------
 * Multi-block reads (the wide_slab fixture)
 * ------------------------------------------------------------------------- */

#define WIDE_SLAB_DIR "simulations/mini-millennium-horizontal/_tests/data/wide_slab"
#define WIDE_SLAB_A_LIST "wide_slab.a_list"
#define WIDE_SLAB_ROWS 8600
/* The reader's HORIZONTAL_HDF5_SCAN_BLOCK (private to read_horizontal_hdf5.c): the number of
   rows it reads or streams at a time. wide_slab's snapshot 0 is wider than one block. */
#define WIDE_SLAB_SCAN_BLOCK 8192
#define WIDE_SLAB_MAX_BLOCKS 4

/* What one scan_forest_index pass over wide_slab's snapshot 0 delivered. */
struct wide_scan_record {
  const int64_t *expected; /* the whole read's forest_index column */
  int64_t first_rows[WIDE_SLAB_MAX_BLOCKS];
  int64_t rows_seen;
  int calls;
  int mismatched;
};

static void record_wide_forest_index(int64_t first_row, const int64_t *values, int64_t count,
                                     void *user) {
  struct wide_scan_record *record = user;
  if (record->calls < WIDE_SLAB_MAX_BLOCKS) {
    record->first_rows[record->calls] = first_row;
  }
  record->calls++;
  if (first_row != record->rows_seen || count <= 0 || first_row + count > WIDE_SLAB_ROWS ||
      memcmp(values, record->expected + first_row, (size_t)count * sizeof(int64_t)) != 0) {
    record->mismatched = 1;
    return;
  }
  record->rows_seen += count;
}

/**
 * @test  test_v3_multi_block_range_reads_match_whole_reads
 * On the wide_slab fixture, whose snapshot 0 holds 8,600 halos in one forest
 * and so spans two of the reader's 8,192-row blocks, a whole read follows the
 * fixture's known chain (the FoF group and the progenitor chain are both
 * row i -> row i + 1, and the mass falls along it), and a handful of ranges
 * that cross the block boundary or start inside the second block -- [0, count),
 * [8191, 8193), [100, 8300), [8192, count), [count - 5, count) and one range
 * wholly inside the second block -- equal the whole read's rows in every
 * column. scan_forest_index delivers the column in at least two blocks,
 * starting at rows 0 and 8192, to the full count, equal to the loaded column.
 * The whole-slab check matters as much as the range checks: a wrong block
 * offset would corrupt both the same way, so only the known chain tells them
 * apart from correct reads. Nothing leaks.
 */
int test_v3_multi_block_range_reads_match_whole_reads(void) {
  REQUIRE_FIXTURE_PACKAGE();
  static const struct v3_dataset wide_slab = {WIDE_SLAB_DIR, WIDE_SLAB_A_LIST, 1, WIDE_SLAB_ROWS};
  struct HorizontalRunInfo info;
  configure_for_dataset(&wide_slab);
  horizontal_reader_open_run(reader(), &FULL_SCAN, &info);

  const int64_t count = horizontal_reader_halo_count(reader(), 0);
  TEST_ASSERT_EQUAL(count, WIDE_SLAB_ROWS, "wide_slab's snapshot 0 should hold 8,600 halos");
  TEST_ASSERT(count > WIDE_SLAB_SCAN_BLOCK, "snapshot 0 should be wider than one scan block");
  TEST_ASSERT_EQUAL(horizontal_reader_halo_count(reader(), 1), 1,
                    "wide_slab's snapshot 1 should hold one halo");

  struct SnapshotSlab whole = snapshot_slab_empty();
  load_whole_slab(0, &whole);
  TEST_ASSERT_EQUAL(whole.nhalos, count, "the whole read should hold every halo");

  int chain_intact = 1;
  for (int64_t i = 0; i < count && chain_intact; i++) {
    const struct RawHalo *halo = &whole.halos[i];
    const int64_t next = i + 1 < count ? i + 1 : -1;
    chain_intact = halo->Descendant == 0 && halo->FirstProgenitor == -1 &&
                   halo->NextProgenitor == next && halo->FirstHaloInFOFgroup == 0 &&
                   halo->NextHaloInFOFgroup == next && halo->SnapNum == 0 &&
                   whole.forest_index[i] == 0 && whole.descendant_snapshot[i] == 1 &&
                   (next < 0 || halo->M_Crit200 > whole.halos[next].M_Crit200);
    if (!chain_intact) {
      fprintf(stderr, "  row %" PRId64 " departs from the fixture's chain\n", i);
    }
  }
  TEST_ASSERT(chain_intact, "the whole read should follow the fixture's chain across every block");

  const struct {
    int64_t lo, hi;
  } ranges[] = {
      {0, count},         {WIDE_SLAB_SCAN_BLOCK - 1, WIDE_SLAB_SCAN_BLOCK + 1},
      {100, 8300},        {WIDE_SLAB_SCAN_BLOCK, count},
      {count - 5, count}, {8300, 8450},
  };
  for (size_t r = 0; r < sizeof(ranges) / sizeof(ranges[0]); r++) {
    struct SnapshotSlab range = snapshot_slab_empty();
    horizontal_reader_load_slab(reader(), 0, ranges[r].lo, ranges[r].hi, &range);
    TEST_ASSERT_EQUAL(range.nhalos, ranges[r].hi - ranges[r].lo,
                      "a range slab should hold row_hi - row_lo");
    TEST_ASSERT_EQUAL(range.row_offset, ranges[r].lo, "a range slab should publish row_offset");
    TEST_ASSERT(range_matches_whole(&whole, &range, ranges[r].lo),
                "every column of a multi-block range read should equal the whole read's rows");
    horizontal_reader_release_slab(reader(), &range);
  }

  struct wide_scan_record record;
  memset(&record, 0, sizeof(record));
  record.expected = whole.forest_index;
  horizontal_reader_scan_forest_index(reader(), 0, record_wide_forest_index, &record);
  TEST_ASSERT(!record.mismatched, "scanned blocks should be contiguous and equal the column");
  TEST_ASSERT_EQUAL(record.rows_seen, count, "the scan should deliver every row once");
  TEST_ASSERT(record.calls >= 2 && record.calls <= WIDE_SLAB_MAX_BLOCKS,
              "the scan should deliver a wide snapshot in more than one block");
  TEST_ASSERT_EQUAL(record.first_rows[0], 0, "the first block should start at row 0");
  TEST_ASSERT_EQUAL(record.first_rows[1], WIDE_SLAB_SCAN_BLOCK,
                    "the second block should start at the scan block size");

  horizontal_reader_release_slab(reader(), &whole);
  horizontal_reader_close_run(reader());
  TEST_ASSERT(no_tracked_leaks(), "multi-block loads and scans should leave no allocation");
  return TEST_PASS;
}

static const struct HorizontalOpenOptions NO_COLUMN_SCAN = {.validate_columns = 0};

static void child_open_run_unscanned(const char *dir) {
  struct HorizontalRunInfo info;
  configure_for_fixture(dir);
  horizontal_reader_open_run(reader(), &NO_COLUMN_SCAN, &info);
  horizontal_reader_close_run(reader());
}

static void child_open_run_null_options(const char *dir) {
  struct HorizontalRunInfo info;
  configure_for_fixture(dir);
  horizontal_reader_open_run(reader(), NULL, &info);
}

static void child_open_hook_null_options(const char *dir) {
  struct HorizontalRunInfo info;
  configure_for_fixture(dir);
  /* Past the dispatcher, straight into the reader's own hook. */
  reader()->open_run(NULL, &info);
}

/**
 * @test  test_v3_open_without_column_scans
 * On both fixtures, validate_columns = 0 opens a dataset the full scan also
 * accepts and publishes exactly the full scan's run metadata: the headers'
 * max_halo_rank_in_forest and n_forests_total, every snapshot count, and
 * source_format "lhalo_binary". A SnapNum value the column scan rejects
 * passes with the scan skipped, which shows the scan is what was skipped,
 * while a header that disagrees across files still aborts. A NULL options
 * aborts at the dispatcher and in the reader's own hook.
 */
int test_v3_open_without_column_scans(void) {
  REQUIRE_FIXTURE_PACKAGE();
  for (size_t d = 0; d < V3_DATASET_COUNT; d++) {
    const struct v3_dataset *dataset = &V3_DATASETS[d];
    struct HorizontalRunInfo scanned;
    struct HorizontalRunInfo unscanned;
    int64_t counts[FIXTURE_SNAPSHOTS + 1];

    configure_for_dataset(dataset);
    horizontal_reader_open_run(reader(), &FULL_SCAN, &scanned);
    TEST_ASSERT(scanned.snapshot_count <= FIXTURE_SNAPSHOTS + 1, "fixture snapshot count bound");
    for (int64_t snap = 0; snap < scanned.snapshot_count; snap++) {
      counts[snap] = horizontal_reader_halo_count(reader(), snap);
    }
    horizontal_reader_close_run(reader());

    configure_for_dataset(dataset);
    horizontal_reader_open_run(reader(), &NO_COLUMN_SCAN, &unscanned);
    TEST_ASSERT_EQUAL(unscanned.n_forests_total, dataset->n_forests_total,
                      "an unscanned open should publish the header's n_forests_total");
    TEST_ASSERT_EQUAL(unscanned.max_halo_rank_in_forest, dataset->max_halo_rank_in_forest,
                      "an unscanned open should publish the header's max_halo_rank_in_forest");
    TEST_ASSERT(strcmp(scanned.source_format, FIXTURE_SOURCE_FORMAT) == 0 &&
                    strcmp(unscanned.source_format, FIXTURE_SOURCE_FORMAT) == 0,
                "source_format should read lhalo_binary for the L-Halo fixtures");
    TEST_ASSERT(unscanned.snapshot_count == scanned.snapshot_count &&
                    unscanned.format_version == scanned.format_version &&
                    unscanned.links_adjacent == scanned.links_adjacent &&
                    unscanned.slab_row_bytes == scanned.slab_row_bytes &&
                    unscanned.n_forests_total == scanned.n_forests_total &&
                    unscanned.max_halo_rank_in_forest == scanned.max_halo_rank_in_forest,
                "an unscanned open should publish the full scan's run metadata");
    for (int64_t snap = 0; snap < unscanned.snapshot_count; snap++) {
      TEST_ASSERT_EQUAL(horizontal_reader_halo_count(reader(), snap), counts[snap],
                        "an unscanned open should serve the same halo counts");
    }
    horizontal_reader_close_run(reader());
  }

  char dir[MAX_STRING_LEN];
  TEST_ASSERT(stage_fixture(dir, sizeof(dir)) == 0, "should stage a scratch copy of the fixture");
  {
    SNAP_FILE(dir, 1, p);
    TEST_ASSERT(set_i32_element(p, "SnapNum", 0, 0) == 0, "the SnapNum mutation should apply");
  }
  TEST_ASSERT(expect_fatal(dir, child_open_run, "'/halos/SnapNum' is 0 at halo 0",
                           "header snapshot_number is 1") == 1,
              "the column scan should reject a SnapNum disagreeing with its header");
  TEST_ASSERT(expect_success(dir, child_open_run_unscanned) == 1,
              "with validate_columns = 0 the SnapNum scan should be skipped");
  {
    SNAP_FILE(dir, 3, p);
    TEST_ASSERT(set_attr_i64(p, "n_forests_total", FIXTURE_N_FORESTS_TOTAL + 1) == 0,
                "the header mutation should apply");
  }
  TEST_ASSERT(expect_fatal(dir, child_open_run_unscanned, "'n_forests_total' is 3",
                           "must be identical in every file") == 1,
              "header checks should still run with validate_columns = 0");
  remove_staged_fixture(dir);

  TEST_ASSERT(expect_fatal(FIXTURE_DIR, child_open_run_null_options,
                           "open_run requires HorizontalOpenOptions", NULL) == 1,
              "the dispatcher should abort on NULL options");
  TEST_ASSERT(expect_fatal(FIXTURE_DIR, child_open_hook_null_options,
                           "horizontal_hdf5: open_run requires HorizontalOpenOptions", NULL) == 1,
              "the reader's hook should abort on NULL options");
  return TEST_PASS;
}

/* A reader with every hook but scan_forest_index, to pin that dispatcher's
   point-of-use check. */
static const struct HorizontalReader NoScanReader = {
    .name = "no_scan_test_reader",
    .processing_order = INPUT_PROCESSING_ORDER_HORIZONTAL,
    .open_run = NULL,
    .close_run = NULL,
    .snapshot_halo_count = NULL,
    .load_slab = NULL,
    .release_slab = NULL,
    .scan_forest_index = NULL,
};

static void child_scan_missing_hook(const char *dir) {
  struct forest_scan_record record;
  (void)dir;
  horizontal_reader_scan_forest_index(&NoScanReader, 0, record_forest_index, &record);
}

static void child_scan_null_visitor(const char *dir) {
  struct HorizontalRunInfo info;
  configure_for_fixture(dir);
  horizontal_reader_open_run(reader(), &FULL_SCAN, &info);
  horizontal_reader_scan_forest_index(reader(), 0, NULL, NULL);
}

/**
 * @test  test_scan_forest_index_dispatch_fails_fast
 * The scan_forest_index dispatcher aborts by name on a reader without the hook
 * and on a NULL visitor, rather than dereferencing NULL.
 */
int test_scan_forest_index_dispatch_fails_fast(void) {
  TEST_ASSERT(expect_fatal(NULL, child_scan_missing_hook, "no_scan_test_reader",
                           "'scan_forest_index'") == 1,
              "a missing scan_forest_index hook should abort naming the hook");
  REQUIRE_FIXTURE_PACKAGE();
  TEST_ASSERT(expect_fatal(FIXTURE_DIR, child_scan_null_visitor,
                           "scan_forest_index requires a visitor", NULL) == 1,
              "a NULL visitor should abort");
  return TEST_PASS;
}

/** @brief Main test runner */
int main(void) {
  printf("%s", BLUE);
  printf("============================================================\n");
  printf("Test Suite: Horizontal Reader, format_version 3\n");
  printf("============================================================\n");
  printf("%s\n", NC);

  init_memory_system(0);
  initialize_error_handling(LOG_LEVEL_WARNING, NULL);

  TEST_RUN(test_v3_open_run_publishes_run_metadata);
  TEST_RUN(test_v3_load_slab_matches_fixture);
  TEST_RUN(test_v3_undeclared_extra_is_not_materialised);
  TEST_RUN(test_int_link_package_rejects_v3);
  TEST_RUN(test_v3_corrupt_inputs_abort);
  TEST_RUN(test_v3_open_run_checks_identity_bounds);
  TEST_RUN(test_v3_corrupt_links_abort);
  TEST_RUN(test_v3_links_bound_by_named_target);
  TEST_RUN(test_v3_link_diagnostics_are_bounded);
  TEST_RUN(test_v3_range_reads_match_whole_reads);
  TEST_RUN(test_v3_load_slab_rejects_bad_ranges);
  TEST_RUN(test_v3_scan_forest_index_streams_column);
  TEST_RUN(test_v3_multi_block_range_reads_match_whole_reads);
  TEST_RUN(test_v3_open_without_column_scans);
  TEST_RUN(test_scan_forest_index_dispatch_fails_fast);

  TEST_SUMMARY();
  return TEST_RESULT();
}
