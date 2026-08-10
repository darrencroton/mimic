/**
 * @file    snapshot/read_snapshot_hdf5.c
 * @brief   snapshot_hdf5 reader: run lifecycle, validation, and count table.
 *
 * Reads the frozen snapshot-ordered HDF5 contract described in
 * docs/dev/SNAPSHOT-HDF5-FORMAT.md (format_version = 1): one
 * `snapshot_NNN.h5` file per snapshot under MimicConfig.SimulationDir, each
 * holding exactly the `/header` and `/halos` groups.
 *
 * open_run validates the whole dataset and publishes run-scoped metadata plus a
 * per-snapshot halo-count table, snapshot_halo_count serves that table, and
 * close_run releases it. load_slab reads one snapshot into a reader-owned
 * struct RawHalo array plus the reader-owned ForestIndex/HaloRankInForest
 * identity arrays, and validates the RawHalo links; release_slab returns the
 * handle to its empty state.
 *
 * Validation order per file is structure first, data second: object set, header
 * attribute set and dtypes, header values, dataset set with dtypes and shapes,
 * and only then the bounded data scans. A file whose shape disagrees with its
 * header is therefore rejected rather than read. Every failure aborts naming
 * the file and the offending object, attribute or value; nothing is repaired.
 *
 * The open-time data scans (format invariant 5) are fixed-size hyperslab reads
 * accumulating running maxima: no buffer there is proportional to n_halos. The
 * slab load necessarily allocates per halo, since a slab *is* the snapshot's
 * halo population.
 *
 * Link validation at load checks index ranges only. Chain topology --
 * cycle-freedom, FoF self-reference, progenitor round-trip closure -- is a
 * producer obligation the converter's validation battery and the topology gate
 * already discharge, and is deliberately not re-derived here.
 *
 * The small HDF5 helpers below are local by design rather than lifted out of
 * tree/read_ctrees_hdf5.c, whose byte-identical output is this phase's gate and
 * which is therefore left untouched.
 */

#ifdef HDF5

#include <hdf5.h>

#include <float.h>
#include <inttypes.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "config.h"
#include "constants.h"
#include "error.h"
#include "memory.h"
#include "snapshot/reader.h"
#include "types.h"

/* Supported on-disk contract version (docs/dev/SNAPSHOT-HDF5-FORMAT.md). */
#define SNAPSHOT_HDF5_FORMAT_VERSION 1

/* Halos read per hyperslab during the data scans. Fixed by construction so scan
   memory is bounded independently of snapshot size. */
#define SNAPSHOT_HDF5_SCAN_BLOCK 8192

/* Room for "<SimulationDir>/snapshot_NNN.h5". */
#define SNAPSHOT_HDF5_PATH_LEN (MAX_STRING_LEN + 32)

/* The empty-dataset sentinel the converter stamps when a dataset holds no halos
   in any snapshot (scripts/convert/links.py). Local names for the shared
   contract values in snapshot/reader.h, which the identity-bounds check also
   consults. */
#define SNAPSHOT_HDF5_EMPTY_N_FORESTS SNAPSHOT_EMPTY_N_FORESTS
#define SNAPSHOT_HDF5_EMPTY_MAX_RANK SNAPSHOT_EMPTY_MAX_RANK

/* ---------------------------------------------------------------------------
 * Contract tables
 *
 * The names, dtypes and shapes below are the normative format_version = 1
 * record. They are stated here rather than derived from the compiled-in
 * simulation package on purpose: the reader validates a file against the
 * format, not against whatever a package happens to declare.
 * ------------------------------------------------------------------------- */

enum snapshot_h5_scalar_type {
  SNAPSHOT_H5_I32 = 0,
  SNAPSHOT_H5_I64,
  SNAPSHOT_H5_F32,
  SNAPSHOT_H5_F64,
};

/** One file's header, as read. Every contract attribute is read and
    dtype-checked; the five physical values (box_size_mpc_h,
    particle_mass_msun_h, omega_matter, omega_lambda, hubble_h) are also
    compared against the configured simulation in open_run_snapshot_hdf5(). */
struct snapshot_h5_header {
  int32_t format_version;
  int32_t links_adjacent;
  int32_t snapshot_number;
  double scale_factor;
  int64_t n_halos;
  int64_t n_forests_total;
  int64_t max_halo_rank_in_forest;
  double box_size_mpc_h;
  double particle_mass_msun_h;
  double omega_matter;
  double omega_lambda;
  double hubble_h;
};

struct snapshot_h5_attr_spec {
  const char *name;
  enum snapshot_h5_scalar_type type;
  size_t offset; /* destination field within struct snapshot_h5_header */
};

#define SNAPSHOT_H5_HEADER_ATTR(attr, dtype)                                                       \
  {#attr, dtype, offsetof(struct snapshot_h5_header, attr)}

static const struct snapshot_h5_attr_spec SNAPSHOT_H5_HEADER_ATTRS[] = {
    SNAPSHOT_H5_HEADER_ATTR(format_version, SNAPSHOT_H5_I32),
    SNAPSHOT_H5_HEADER_ATTR(links_adjacent, SNAPSHOT_H5_I32),
    SNAPSHOT_H5_HEADER_ATTR(snapshot_number, SNAPSHOT_H5_I32),
    SNAPSHOT_H5_HEADER_ATTR(scale_factor, SNAPSHOT_H5_F64),
    SNAPSHOT_H5_HEADER_ATTR(n_halos, SNAPSHOT_H5_I64),
    SNAPSHOT_H5_HEADER_ATTR(n_forests_total, SNAPSHOT_H5_I64),
    SNAPSHOT_H5_HEADER_ATTR(max_halo_rank_in_forest, SNAPSHOT_H5_I64),
    SNAPSHOT_H5_HEADER_ATTR(box_size_mpc_h, SNAPSHOT_H5_F64),
    SNAPSHOT_H5_HEADER_ATTR(particle_mass_msun_h, SNAPSHOT_H5_F64),
    SNAPSHOT_H5_HEADER_ATTR(omega_matter, SNAPSHOT_H5_F64),
    SNAPSHOT_H5_HEADER_ATTR(omega_lambda, SNAPSHOT_H5_F64),
    SNAPSHOT_H5_HEADER_ATTR(hubble_h, SNAPSHOT_H5_F64),
};
#define SNAPSHOT_H5_HEADER_ATTR_COUNT                                                              \
  (sizeof(SNAPSHOT_H5_HEADER_ATTRS) / sizeof(SNAPSHOT_H5_HEADER_ATTRS[0]))

struct snapshot_h5_dataset_spec {
  const char *name;
  enum snapshot_h5_scalar_type type;
  int ncols; /* 0 = rank-1 column, 3 = [n_halos, 3] vector */
};

static const struct snapshot_h5_dataset_spec SNAPSHOT_H5_HALO_DATASETS[] = {
    {"Descendant", SNAPSHOT_H5_I32, 0},
    {"FirstProgenitor", SNAPSHOT_H5_I32, 0},
    {"NextProgenitor", SNAPSHOT_H5_I32, 0},
    {"FirstHaloInFOFgroup", SNAPSHOT_H5_I32, 0},
    {"NextHaloInFOFgroup", SNAPSHOT_H5_I32, 0},
    {"Len", SNAPSHOT_H5_I32, 0},
    {"SnapNum", SNAPSHOT_H5_I32, 0},
    {"M_Crit200", SNAPSHOT_H5_F32, 0},
    {"Pos", SNAPSHOT_H5_F32, 3},
    {"Vel", SNAPSHOT_H5_F32, 3},
    {"Spin", SNAPSHOT_H5_F32, 3},
    {"VelDisp", SNAPSHOT_H5_F32, 0},
    {"Vmax", SNAPSHOT_H5_F32, 0},
    {"MostBoundID", SNAPSHOT_H5_I64, 0},
    {"ForestIndex", SNAPSHOT_H5_I64, 0},
    {"HaloRankInForest", SNAPSHOT_H5_I64, 0},
};
#define SNAPSHOT_H5_HALO_DATASET_COUNT                                                             \
  (sizeof(SNAPSHOT_H5_HALO_DATASETS) / sizeof(SNAPSHOT_H5_HALO_DATASETS[0]))

/** Run-scoped reader state. One reader instance per process, so a file-static
    record is sufficient (mirrors the tree readers). */
struct snapshot_hdf5_run {
  int is_open;
  int64_t snapshot_count;
  int64_t *halo_counts; /* [snapshot_count] */
  int64_t loaded_slabs; /* slabs handed out and not yet released */
  struct SnapshotRunInfo info;
};
static struct snapshot_hdf5_run SNAP;

/* Fixed-size scan buffers, never resized: the scans below read in blocks of
   SNAPSHOT_HDF5_SCAN_BLOCK halos regardless of snapshot size. */
static int32_t snapshot_h5_scan_i32[SNAPSHOT_HDF5_SCAN_BLOCK];
static int64_t snapshot_h5_scan_i64[SNAPSHOT_HDF5_SCAN_BLOCK];

/* ---------------------------------------------------------------------------
 * Local HDF5 helpers
 * ------------------------------------------------------------------------- */

/** @brief Human-readable name of a contract dtype, for diagnostics. */
static const char *snapshot_h5_type_name(enum snapshot_h5_scalar_type type) {
  switch (type) {
  case SNAPSHOT_H5_I32:
    return "int32";
  case SNAPSHOT_H5_I64:
    return "int64";
  case SNAPSHOT_H5_F32:
    return "float32";
  case SNAPSHOT_H5_F64:
    return "float64";
  }
  return "unknown";
}

/**
 * @brief   Does an on-disk datatype match a contract dtype?
 *
 * Compared by class, size and signedness rather than by H5Tequal against a
 * native type, so the check is byte-order agnostic.
 */
static int snapshot_h5_type_matches(hid_t dtype, enum snapshot_h5_scalar_type expected) {
  const H5T_class_t cls = H5Tget_class(dtype);
  const size_t size = H5Tget_size(dtype);

  switch (expected) {
  case SNAPSHOT_H5_I32:
    return cls == H5T_INTEGER && size == 4 && H5Tget_sign(dtype) == H5T_SGN_2;
  case SNAPSHOT_H5_I64:
    return cls == H5T_INTEGER && size == 8 && H5Tget_sign(dtype) == H5T_SGN_2;
  case SNAPSHOT_H5_F32:
    return cls == H5T_FLOAT && size == 4;
  case SNAPSHOT_H5_F64:
    return cls == H5T_FLOAT && size == 8;
  }
  return 0;
}

/**
 * @brief   Native memory datatype to read a contract dtype into.
 *
 * Reads go through the native type, never the on-disk type, so HDF5 performs
 * the byte-order conversion. Passing the file type as the memory type would
 * copy file-order bytes straight into native fields and silently byte-swap
 * every value of a conforming file written on the other endianness -- which
 * snapshot_h5_type_matches() accepts by design.
 */
static hid_t snapshot_h5_native_type(enum snapshot_h5_scalar_type type) {
  switch (type) {
  case SNAPSHOT_H5_I32:
    return H5T_NATIVE_INT32;
  case SNAPSHOT_H5_I64:
    return H5T_NATIVE_INT64;
  case SNAPSHOT_H5_F32:
    return H5T_NATIVE_FLOAT;
  case SNAPSHOT_H5_F64:
    return H5T_NATIVE_DOUBLE;
  }
  return H5I_INVALID_HID;
}

/** @brief Build "<SimulationDir>/snapshot_NNN.h5" with a fixed format string. */
static void snapshot_h5_format_path(char *path, size_t path_size, int64_t snapnum) {
  /* The format string is a literal by construction: configured text
     (SimulationDir) is an argument, never a printf format. */
  const int written =
      snprintf(path, path_size, "%s/snapshot_%03d.h5", MimicConfig.SimulationDir, (int)snapnum);
  if (written < 0 || (size_t)written >= path_size) {
    FATAL_ERROR("Snapshot file path for snapshot %" PRId64
                " under simulation directory '%s' does not fit in %zu bytes",
                snapnum, MimicConfig.SimulationDir, path_size);
  }
}

/** Iteration state for snapshot_h5_reject_unknown_attr(). */
struct snapshot_h5_attr_scan {
  const char *path;
};

/** @brief H5Aiterate2 callback rejecting any attribute the contract omits. */
static herr_t snapshot_h5_reject_unknown_attr(hid_t location_id, const char *attr_name,
                                              const H5A_info_t *ainfo, void *op_data) {
  const struct snapshot_h5_attr_scan *scan = (const struct snapshot_h5_attr_scan *)op_data;
  (void)location_id;
  (void)ainfo;

  for (size_t i = 0; i < SNAPSHOT_H5_HEADER_ATTR_COUNT; i++) {
    if (strcmp(SNAPSHOT_H5_HEADER_ATTRS[i].name, attr_name) == 0) {
      return 0;
    }
  }
  FATAL_ERROR("%s: '/header' carries unexpected attribute '%s'; format_version %d defines exactly "
              "%zu header attributes",
              scan->path, attr_name, SNAPSHOT_HDF5_FORMAT_VERSION,
              (size_t)SNAPSHOT_H5_HEADER_ATTR_COUNT);
}

/** @brief Validate the root object set: exactly the groups /header and /halos. */
static void snapshot_h5_validate_object_set(hid_t file, const char *path) {
  static const char *const expected[] = {"halos", "header"};
  const size_t expected_count = sizeof(expected) / sizeof(expected[0]);

  hid_t root = H5Gopen2(file, "/", H5P_DEFAULT);
  if (root < 0) {
    FATAL_ERROR("%s: could not open the root group", path);
  }

  H5G_info_t ginfo;
  if (H5Gget_info(root, &ginfo) < 0) {
    FATAL_ERROR("%s: could not read the root group link count", path);
  }

  for (hsize_t i = 0; i < ginfo.nlinks; i++) {
    char name[MAX_STRING_LEN];
    const ssize_t len = H5Lget_name_by_idx(root, ".", H5_INDEX_NAME, H5_ITER_INC, i, name,
                                           sizeof(name), H5P_DEFAULT);
    if (len < 0 || (size_t)len >= sizeof(name)) {
      FATAL_ERROR("%s: could not read the name of root object %" PRIu64, path, (uint64_t)i);
    }
    int known = 0;
    for (size_t e = 0; e < expected_count; e++) {
      if (strcmp(expected[e], name) == 0) {
        known = 1;
        break;
      }
    }
    if (!known) {
      FATAL_ERROR("%s: unexpected root object '%s'; format_version %d defines exactly the groups "
                  "'/header' and '/halos'",
                  path, name, SNAPSHOT_HDF5_FORMAT_VERSION);
    }
  }

  for (size_t e = 0; e < expected_count; e++) {
    if (H5Lexists(root, expected[e], H5P_DEFAULT) <= 0) {
      FATAL_ERROR("%s: required group '/%s' is missing", path, expected[e]);
    }
    hid_t obj = H5Gopen2(root, expected[e], H5P_DEFAULT);
    if (obj < 0) {
      FATAL_ERROR("%s: object '/%s' is not a group", path, expected[e]);
    }
    if (H5Gclose(obj) < 0) {
      FATAL_ERROR("%s: could not close group '/%s'", path, expected[e]);
    }
  }

  if (H5Gclose(root) < 0) {
    FATAL_ERROR("%s: could not close the root group", path);
  }
}

/**
 * @brief   Read one scalar header attribute, validating its rank and dtype.
 * @param   dst   Destination sized for the contract dtype.
 */
static void snapshot_h5_read_header_attr(hid_t file, const char *path,
                                         const struct snapshot_h5_attr_spec *spec, void *dst) {
  hid_t attr = H5Aopen_by_name(file, "/header", spec->name, H5P_DEFAULT, H5P_DEFAULT);
  if (attr < 0) {
    FATAL_ERROR("%s: required header attribute '%s' is missing", path, spec->name);
  }

  hid_t space = H5Aget_space(attr);
  if (space < 0) {
    FATAL_ERROR("%s: could not read the dataspace of header attribute '%s'", path, spec->name);
  }
  if (H5Sget_simple_extent_type(space) != H5S_SCALAR) {
    FATAL_ERROR("%s: header attribute '%s' must be a scalar", path, spec->name);
  }
  if (H5Sclose(space) < 0) {
    FATAL_ERROR("%s: could not close the dataspace of header attribute '%s'", path, spec->name);
  }

  hid_t dtype = H5Aget_type(attr);
  if (dtype < 0) {
    FATAL_ERROR("%s: could not read the datatype of header attribute '%s'", path, spec->name);
  }
  if (!snapshot_h5_type_matches(dtype, spec->type)) {
    FATAL_ERROR("%s: header attribute '%s' must be %s on disk; found HDF5 type class %d of %zu "
                "bytes",
                path, spec->name, snapshot_h5_type_name(spec->type), (int)H5Tget_class(dtype),
                H5Tget_size(dtype));
  }

  if (H5Aread(attr, snapshot_h5_native_type(spec->type), dst) < 0) {
    FATAL_ERROR("%s: could not read header attribute '%s'", path, spec->name);
  }
  if (H5Tclose(dtype) < 0 || H5Aclose(attr) < 0) {
    FATAL_ERROR("%s: could not close header attribute '%s'", path, spec->name);
  }
}

/**
 * @brief   Validate the header attribute set and read every attribute.
 *
 * An extra attribute is rejected by the iteration; a missing one is reported by
 * the read that needs it, naming it.
 */
static void snapshot_h5_read_header(hid_t file, const char *path,
                                    struct snapshot_h5_header *header) {
  hid_t group = H5Gopen2(file, "/header", H5P_DEFAULT);
  if (group < 0) {
    FATAL_ERROR("%s: could not open group '/header'", path);
  }
  struct snapshot_h5_attr_scan scan = {path};
  hsize_t idx = 0;
  if (H5Aiterate2(group, H5_INDEX_NAME, H5_ITER_INC, &idx, snapshot_h5_reject_unknown_attr, &scan) <
      0) {
    FATAL_ERROR("%s: could not enumerate the attributes of '/header'", path);
  }
  if (H5Gclose(group) < 0) {
    FATAL_ERROR("%s: could not close group '/header'", path);
  }

  for (size_t i = 0; i < SNAPSHOT_H5_HEADER_ATTR_COUNT; i++) {
    snapshot_h5_read_header_attr(file, path, &SNAPSHOT_H5_HEADER_ATTRS[i],
                                 (char *)header + SNAPSHOT_H5_HEADER_ATTRS[i].offset);
  }
}

/**
 * @brief   Validate the /halos dataset set, dtypes, ranks and shapes.
 *
 * Runs before any bulk read, so a dataset of the wrong shape is rejected rather
 * than read into a buffer sized from the header.
 */
static void snapshot_h5_validate_halo_datasets(hid_t file, const char *path, int64_t n_halos) {
  hid_t group = H5Gopen2(file, "/halos", H5P_DEFAULT);
  if (group < 0) {
    FATAL_ERROR("%s: could not open group '/halos'", path);
  }

  H5G_info_t ginfo;
  if (H5Gget_info(group, &ginfo) < 0) {
    FATAL_ERROR("%s: could not read the link count of '/halos'", path);
  }

  for (hsize_t i = 0; i < ginfo.nlinks; i++) {
    char name[MAX_STRING_LEN];
    const ssize_t len = H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, i, name,
                                           sizeof(name), H5P_DEFAULT);
    if (len < 0 || (size_t)len >= sizeof(name)) {
      FATAL_ERROR("%s: could not read the name of '/halos' member %" PRIu64, path, (uint64_t)i);
    }
    int known = 0;
    for (size_t s = 0; s < SNAPSHOT_H5_HALO_DATASET_COUNT; s++) {
      if (strcmp(SNAPSHOT_H5_HALO_DATASETS[s].name, name) == 0) {
        known = 1;
        break;
      }
    }
    if (!known) {
      FATAL_ERROR("%s: unexpected dataset '/halos/%s'; format_version %d defines exactly %zu halo "
                  "datasets",
                  path, name, SNAPSHOT_HDF5_FORMAT_VERSION, (size_t)SNAPSHOT_H5_HALO_DATASET_COUNT);
    }
  }

  for (size_t s = 0; s < SNAPSHOT_H5_HALO_DATASET_COUNT; s++) {
    const struct snapshot_h5_dataset_spec *spec = &SNAPSHOT_H5_HALO_DATASETS[s];

    if (H5Lexists(group, spec->name, H5P_DEFAULT) <= 0) {
      FATAL_ERROR("%s: required dataset '/halos/%s' is missing", path, spec->name);
    }

    hid_t dset = H5Dopen2(group, spec->name, H5P_DEFAULT);
    if (dset < 0) {
      FATAL_ERROR("%s: could not open dataset '/halos/%s'", path, spec->name);
    }

    hid_t dtype = H5Dget_type(dset);
    if (dtype < 0) {
      FATAL_ERROR("%s: could not read the datatype of '/halos/%s'", path, spec->name);
    }
    if (!snapshot_h5_type_matches(dtype, spec->type)) {
      FATAL_ERROR("%s: dataset '/halos/%s' must be %s on disk; found HDF5 type class %d of %zu "
                  "bytes",
                  path, spec->name, snapshot_h5_type_name(spec->type), (int)H5Tget_class(dtype),
                  H5Tget_size(dtype));
    }
    if (H5Tclose(dtype) < 0) {
      FATAL_ERROR("%s: could not close the datatype of '/halos/%s'", path, spec->name);
    }

    hid_t space = H5Dget_space(dset);
    if (space < 0) {
      FATAL_ERROR("%s: could not read the dataspace of '/halos/%s'", path, spec->name);
    }
    const int expected_rank = spec->ncols == 0 ? 1 : 2;
    const int rank = H5Sget_simple_extent_ndims(space);
    if (rank != expected_rank) {
      FATAL_ERROR("%s: dataset '/halos/%s' must have rank %d; found rank %d", path, spec->name,
                  expected_rank, rank);
    }
    hsize_t dims[2] = {0, 0};
    if (H5Sget_simple_extent_dims(space, dims, NULL) != expected_rank) {
      FATAL_ERROR("%s: could not read the extent of '/halos/%s'", path, spec->name);
    }
    if ((int64_t)dims[0] != n_halos) {
      FATAL_ERROR("%s: dataset '/halos/%s' has length %" PRIu64 " but header n_halos is %" PRId64,
                  path, spec->name, (uint64_t)dims[0], n_halos);
    }
    /* Compared in the wide type: narrowing dims[1] to int would let a logical
       second dimension of 2^32 + 3 truncate to 3 and pass. */
    if (expected_rank == 2 && dims[1] != (hsize_t)spec->ncols) {
      FATAL_ERROR("%s: dataset '/halos/%s' must have shape [%" PRId64
                  ", %d]; found second dimension %" PRIu64,
                  path, spec->name, n_halos, spec->ncols, (uint64_t)dims[1]);
    }
    if (H5Sclose(space) < 0) {
      FATAL_ERROR("%s: could not close the dataspace of '/halos/%s'", path, spec->name);
    }
    if (H5Dclose(dset) < 0) {
      FATAL_ERROR("%s: could not close dataset '/halos/%s'", path, spec->name);
    }
  }

  if (H5Gclose(group) < 0) {
    FATAL_ERROR("%s: could not close group '/halos'", path);
  }
}

/** @brief Read [offset, offset+count) of a validated rank-1 dataset into buf. */
static void snapshot_h5_read_block(hid_t dset, hid_t space, const char *path,
                                   const char *dataset_name, hid_t mem_type, hsize_t offset,
                                   hsize_t count, void *buf) {
  const hsize_t start[1] = {offset};
  const hsize_t block[1] = {count};

  if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, block, NULL) < 0) {
    FATAL_ERROR("%s: could not select halos [%" PRIu64 ", %" PRIu64 ") of '/halos/%s'", path,
                (uint64_t)offset, (uint64_t)(offset + count), dataset_name);
  }
  hid_t memspace = H5Screate_simple(1, block, NULL);
  if (memspace < 0) {
    FATAL_ERROR("%s: could not create a read buffer dataspace for '/halos/%s'", path, dataset_name);
  }
  if (H5Dread(dset, mem_type, memspace, space, H5P_DEFAULT, buf) < 0) {
    FATAL_ERROR("%s: could not read halos [%" PRIu64 ", %" PRIu64 ") of '/halos/%s'", path,
                (uint64_t)offset, (uint64_t)(offset + count), dataset_name);
  }
  if (H5Sclose(memspace) < 0) {
    FATAL_ERROR("%s: could not close the read buffer dataspace for '/halos/%s'", path,
                dataset_name);
  }
}

/** @brief Open one validated /halos dataset and its dataspace for scanning. */
static void snapshot_h5_open_scan(hid_t file, const char *path, const char *dataset_name,
                                  hid_t *dset, hid_t *space) {
  char full_name[MAX_STRING_LEN];
  const int written = snprintf(full_name, sizeof(full_name), "/halos/%s", dataset_name);
  if (written < 0 || (size_t)written >= sizeof(full_name)) {
    FATAL_ERROR("%s: dataset name '/halos/%s' is too long", path, dataset_name);
  }

  *dset = H5Dopen2(file, full_name, H5P_DEFAULT);
  if (*dset < 0) {
    FATAL_ERROR("%s: could not open dataset '%s' for validation", path, full_name);
  }
  *space = H5Dget_space(*dset);
  if (*space < 0) {
    FATAL_ERROR("%s: could not read the dataspace of '%s'", path, full_name);
  }
}

static void snapshot_h5_close_scan(const char *path, const char *dataset_name, hid_t dset,
                                   hid_t space) {
  if (H5Sclose(space) < 0 || H5Dclose(dset) < 0) {
    FATAL_ERROR("%s: could not close dataset '/halos/%s' after validation", path, dataset_name);
  }
}

/**
 * @brief   Invariant 5: every SnapNum equals the file's snapshot_number.
 */
static void snapshot_h5_scan_snapnum(hid_t file, const char *path, int64_t n_halos,
                                     int32_t snapshot_number) {
  hid_t dset, space;
  snapshot_h5_open_scan(file, path, "SnapNum", &dset, &space);

  for (int64_t offset = 0; offset < n_halos; offset += SNAPSHOT_HDF5_SCAN_BLOCK) {
    const int64_t remaining = n_halos - offset;
    const int64_t count =
        remaining < SNAPSHOT_HDF5_SCAN_BLOCK ? remaining : (int64_t)SNAPSHOT_HDF5_SCAN_BLOCK;
    snapshot_h5_read_block(dset, space, path, "SnapNum", H5T_NATIVE_INT32, (hsize_t)offset,
                           (hsize_t)count, snapshot_h5_scan_i32);
    for (int64_t i = 0; i < count; i++) {
      if (snapshot_h5_scan_i32[i] != snapshot_number) {
        FATAL_ERROR("%s: '/halos/SnapNum' is %" PRId32 " at halo %" PRId64
                    " but the header snapshot_number is %" PRId32,
                    path, snapshot_h5_scan_i32[i], offset + i, snapshot_number);
      }
    }
  }

  snapshot_h5_close_scan(path, "SnapNum", dset, space);
}

/**
 * @brief   Running maximum of an int64 /halos column, with a range check.
 * @param   lower_bound  Inclusive lower bound; a smaller value aborts.
 * @param   upper_bound  Inclusive upper bound; a larger value aborts. Pass
 *                       (INT64_MIN, INT64_MAX) to accept any value.
 * @return  Maximum over this file, or INT64_MIN if it holds no halos.
 */
static int64_t snapshot_h5_scan_i64_max(hid_t file, const char *path, const char *dataset_name,
                                        int64_t n_halos, int64_t lower_bound, int64_t upper_bound) {
  hid_t dset, space;
  int64_t measured = INT64_MIN;

  snapshot_h5_open_scan(file, path, dataset_name, &dset, &space);

  for (int64_t offset = 0; offset < n_halos; offset += SNAPSHOT_HDF5_SCAN_BLOCK) {
    const int64_t remaining = n_halos - offset;
    const int64_t count =
        remaining < SNAPSHOT_HDF5_SCAN_BLOCK ? remaining : (int64_t)SNAPSHOT_HDF5_SCAN_BLOCK;
    snapshot_h5_read_block(dset, space, path, dataset_name, H5T_NATIVE_INT64, (hsize_t)offset,
                           (hsize_t)count, snapshot_h5_scan_i64);
    for (int64_t i = 0; i < count; i++) {
      const int64_t value = snapshot_h5_scan_i64[i];
      if (value < lower_bound || value > upper_bound) {
        FATAL_ERROR("%s: '/halos/%s' is %" PRId64 " at halo %" PRId64
                    "; the permitted range is [%" PRId64 ", %" PRId64 "]",
                    path, dataset_name, value, offset + i, lower_bound, upper_bound);
      }
      if (value > measured) {
        measured = value;
      }
    }
  }

  snapshot_h5_close_scan(path, dataset_name, dset, space);
  return measured;
}

/* ---------------------------------------------------------------------------
 * Slab loading
 * ------------------------------------------------------------------------- */

/**
 * @brief   Read one whole validated /halos dataset into buf.
 *
 * Extents are carried in hsize_t widened from int64_t; nothing on this path is
 * narrowed to int. The file dataspace is used whole (H5S_ALL) because
 * snapshot_h5_validate_halo_datasets() has already proven it is exactly
 * [n_halos] or [n_halos, ncols].
 *
 * The memory type is the native type for the destination field, never the
 * on-disk type: HDF5 performs the byte-order and width conversion, so a
 * conforming file written on the other endianness reads correctly.
 */
static void snapshot_h5_read_column(hid_t file, const char *path, const char *dataset_name,
                                    hid_t mem_type, int64_t n_halos, int64_t ncols, void *buf) {
  char full_name[MAX_STRING_LEN];
  const int written = snprintf(full_name, sizeof(full_name), "/halos/%s", dataset_name);
  if (written < 0 || (size_t)written >= sizeof(full_name)) {
    FATAL_ERROR("%s: dataset name '/halos/%s' is too long", path, dataset_name);
  }

  hid_t dset = H5Dopen2(file, full_name, H5P_DEFAULT);
  if (dset < 0) {
    FATAL_ERROR("%s: could not open dataset '%s'", path, full_name);
  }

  const hsize_t dims[2] = {(hsize_t)n_halos, (hsize_t)ncols};
  const int rank = ncols > 1 ? 2 : 1;
  hid_t memspace = H5Screate_simple(rank, dims, NULL);
  if (memspace < 0) {
    FATAL_ERROR("%s: could not create a read buffer dataspace for '%s' (%" PRId64 " halos)", path,
                full_name, n_halos);
  }
  if (H5Dread(dset, mem_type, memspace, H5S_ALL, H5P_DEFAULT, buf) < 0) {
    FATAL_ERROR("%s: could not read dataset '%s' (%" PRId64 " halos)", path, full_name, n_halos);
  }
  if (H5Sclose(memspace) < 0) {
    FATAL_ERROR("%s: could not close the read buffer dataspace for '%s'", path, full_name);
  }
  if (H5Dclose(dset) < 0) {
    FATAL_ERROR("%s: could not close dataset '%s'", path, full_name);
  }
}

/* Native memory type for each READ_AS_* token the property generator emits
   (scripts/generate_properties.py:_read_type_for_catalog). A token the
   generator gains without a mapping here is a compile error, not a silent
   misread. */
#define SNAPSHOT_H5_MEMTYPE_READ_AS_INT H5T_NATIVE_INT
#define SNAPSHOT_H5_MEMTYPE_READ_AS_FLOAT H5T_NATIVE_FLOAT
#define SNAPSHOT_H5_MEMTYPE_READ_AS_LLONG H5T_NATIVE_LLONG
#define SNAPSHOT_H5_MEMTYPE(read_as) SNAPSHOT_H5_MEMTYPE_##read_as

/* Widest native element the property list can request, and so the per-element
   stride of the staging buffers below. */
#define SNAPSHOT_H5_MAX_ELEMENT_SIZE 8

/* Snapshot-flavoured counterparts of the tree reader's macros
   (src/io/tree/hdf5.c:116-143), used to include the same generated property
   list. The differences are the dataset path ("/halos/<name>" rather than
   "tree_NNN/<name>"), the destination array (the slab rather than
   InputTreeHalos), int64_t counts, and that values are copied out of the
   staging buffer with memcpy rather than through a cast of the buffer pointer
   to the field's type -- the tree reader's cast puns a double * as an int * or
   float *, which is an aliasing violation cppcheck flags. */
#define READ_TREE_PROPERTY(field_name, hdf5_name, type_int, data_type)                             \
  {                                                                                                \
    snapshot_h5_read_column(file, path, hdf5_name, SNAPSHOT_H5_MEMTYPE(type_int), n_halos, 1,      \
                            buffer);                                                               \
    for (int64_t halo_idx = 0; halo_idx < n_halos; ++halo_idx) {                                   \
      memcpy(&halos[halo_idx].field_name, buffer + halo_idx * sizeof(data_type),                   \
             sizeof(data_type));                                                                   \
    }                                                                                              \
  }

#define READ_TREE_PROPERTY_MULTIPLEDIM(field_name, hdf5_name, type_int, data_type)                 \
  {                                                                                                \
    snapshot_h5_read_column(file, path, hdf5_name, SNAPSHOT_H5_MEMTYPE(type_int), n_halos, NDIM,   \
                            buffer_multipledim);                                                   \
    for (int64_t halo_idx = 0; halo_idx < n_halos; ++halo_idx) {                                   \
      for (int64_t dim = 0; dim < NDIM; ++dim) {                                                   \
        memcpy(&halos[halo_idx].field_name[dim],                                                   \
               buffer_multipledim + (halo_idx * NDIM + dim) * sizeof(data_type),                   \
               sizeof(data_type));                                                                 \
      }                                                                                            \
    }                                                                                              \
  }

/**
 * @brief   Fill a slab array from one snapshot file's /halos datasets.
 *
 * Every field of struct RawHalo is populated from the generated property list,
 * so a package that gains or renames a catalog field is followed automatically
 * with no edit here.
 */
static void snapshot_h5_fill_halos(hid_t file, const char *path, int64_t n_halos,
                                   struct RawHalo *halos) {
  /* One scalar and one vector staging buffer, each strided for the widest native
     type the property list can request. HDF5 packs the elements it reads at the
     front of the buffer, so the macros above address them at the field's own
     stride and copy them out by value. */
  char *buffer = mymalloc_cat(SNAPSHOT_H5_MAX_ELEMENT_SIZE * (size_t)n_halos, MEM_TREES);
  char *buffer_multipledim =
      mymalloc_cat(SNAPSHOT_H5_MAX_ELEMENT_SIZE * (size_t)n_halos * NDIM, MEM_TREES);

#include "../../include/generated/read_tree_hdf5_properties.inc"

  myfree(buffer_multipledim);
  myfree(buffer);
}

#undef READ_TREE_PROPERTY
#undef READ_TREE_PROPERTY_MULTIPLEDIM

/**
 * @brief   Schema-table entry by dataset name, or NULL if not declared.
 *
 * The same lookup pattern snapshot_h5_validate_halo_datasets() already uses to
 * check a dataset's membership, reused here so a dataset's name and on-disk
 * scalar type come from SNAPSHOT_H5_HALO_DATASETS rather than being retyped as
 * literals at the call site. The caller (snapshot_h5_fill_identity() below)
 * still hard-types its destination buffers as int64_t and passes a literal
 * column count of 1; that is not derived from this lookup, and is safe only
 * because format_version 1 fixes ForestIndex and HaloRankInForest as scalar
 * (ncols 0) int64 columns -- this function does not itself enforce that.
 */
static const struct snapshot_h5_dataset_spec *snapshot_h5_dataset_spec_by_name(const char *name) {
  for (size_t s = 0; s < SNAPSHOT_H5_HALO_DATASET_COUNT; s++) {
    if (strcmp(SNAPSHOT_H5_HALO_DATASETS[s].name, name) == 0) {
      return &SNAPSHOT_H5_HALO_DATASETS[s];
    }
  }
  return NULL;
}

/**
 * @brief   Fill the reader-owned identity arrays from one snapshot file.
 *
 * ForestIndex and HaloRankInForest are snapshot-format identity metadata
 * (docs/dev/SNAPSHOT-HDF5-FORMAT.md), not struct RawHalo members: they are
 * read directly by dataset name into slab-owned arrays, independent of
 * halo_properties.yaml and the generated property list that fills struct
 * RawHalo above.
 */
static void snapshot_h5_fill_identity(hid_t file, const char *path, int64_t n_halos,
                                      int64_t *forest_index, int64_t *halo_rank_in_forest) {
  const struct snapshot_h5_dataset_spec *forest_spec =
      snapshot_h5_dataset_spec_by_name("ForestIndex");
  const struct snapshot_h5_dataset_spec *rank_spec =
      snapshot_h5_dataset_spec_by_name("HaloRankInForest");

  snapshot_h5_read_column(file, path, forest_spec->name, snapshot_h5_native_type(forest_spec->type),
                          n_halos, 1, forest_index);
  snapshot_h5_read_column(file, path, rank_spec->name, snapshot_h5_native_type(rank_spec->type),
                          n_halos, 1, halo_rank_in_forest);
}

/* ---------------------------------------------------------------------------
 * Link validation
 *
 * Index ranges only (design decision 9). Each link field points either within
 * the loaded snapshot or into an immediately adjacent one, which is what
 * `links_adjacent = 1` promises.
 * ------------------------------------------------------------------------- */

/** Which snapshot's halo count bounds a link field. */
enum snapshot_h5_link_domain {
  SNAPSHOT_H5_LINK_PREV = 0, /* snapshot N-1: progenitors */
  SNAPSHOT_H5_LINK_THIS,     /* snapshot N: siblings and FoF membership */
  SNAPSHOT_H5_LINK_NEXT,     /* snapshot N+1: descendants */
};

struct snapshot_h5_link_spec {
  const char *name;
  size_t offset; /* int field within struct RawHalo */
  int allow_null_link;
  enum snapshot_h5_link_domain domain;
};

#define SNAPSHOT_H5_LINK(field, allow_null, domain)                                                \
  {#field, offsetof(struct RawHalo, field), allow_null, domain}

static const struct snapshot_h5_link_spec SNAPSHOT_H5_LINKS[] = {
    SNAPSHOT_H5_LINK(FirstProgenitor, 1, SNAPSHOT_H5_LINK_PREV),
    SNAPSHOT_H5_LINK(NextProgenitor, 1, SNAPSHOT_H5_LINK_THIS),
    /* Never -1: every halo belongs to a FoF group, at minimum its own. */
    SNAPSHOT_H5_LINK(FirstHaloInFOFgroup, 0, SNAPSHOT_H5_LINK_THIS),
    SNAPSHOT_H5_LINK(NextHaloInFOFgroup, 1, SNAPSHOT_H5_LINK_THIS),
    SNAPSHOT_H5_LINK(Descendant, 1, SNAPSHOT_H5_LINK_NEXT),
};
#define SNAPSHOT_H5_LINK_COUNT (sizeof(SNAPSHOT_H5_LINKS) / sizeof(SNAPSHOT_H5_LINKS[0]))

/* The link validator below reads each of these members through offsetof() and
   a `*(const int *)` cast; nothing in that path re-checks the member's type, so
   a package widening a link field would otherwise validate a silently
   truncated value instead of failing to compile. */
_Static_assert(sizeof(((struct RawHalo *)0)->FirstProgenitor) == sizeof(int),
               "FirstProgenitor must stay int-sized for the *(const int *) link validator");
_Static_assert(sizeof(((struct RawHalo *)0)->NextProgenitor) == sizeof(int),
               "NextProgenitor must stay int-sized for the *(const int *) link validator");
_Static_assert(sizeof(((struct RawHalo *)0)->FirstHaloInFOFgroup) == sizeof(int),
               "FirstHaloInFOFgroup must stay int-sized for the *(const int *) link validator");
_Static_assert(sizeof(((struct RawHalo *)0)->NextHaloInFOFgroup) == sizeof(int),
               "NextHaloInFOFgroup must stay int-sized for the *(const int *) link validator");
_Static_assert(sizeof(((struct RawHalo *)0)->Descendant) == sizeof(int),
               "Descendant must stay int-sized for the *(const int *) link validator");

/**
 * @brief   Halo count bounding one link field, and the snapshot it comes from.
 *
 * Snapshot 0 has no snapshot -1 and the final snapshot has no successor; both
 * are treated as holding zero halos, which is what makes a non-null
 * FirstProgenitor in snapshot 0, and a non-null Descendant in the final
 * snapshot, out of range rather than a special case.
 */
static int64_t snapshot_h5_link_limit(enum snapshot_h5_link_domain domain, int64_t snapnum,
                                      int64_t *bounding_snap) {
  switch (domain) {
  case SNAPSHOT_H5_LINK_PREV:
    *bounding_snap = snapnum - 1;
    return snapnum > 0 ? SNAP.halo_counts[snapnum - 1] : 0;
  case SNAPSHOT_H5_LINK_THIS:
    *bounding_snap = snapnum;
    return SNAP.halo_counts[snapnum];
  case SNAPSHOT_H5_LINK_NEXT:
    *bounding_snap = snapnum + 1;
    return snapnum + 1 < SNAP.snapshot_count ? SNAP.halo_counts[snapnum + 1] : 0;
  }
  *bounding_snap = snapnum;
  return 0;
}

/**
 * @brief   Validate every link field of a loaded slab, aborting on any offence.
 *
 * Diagnostics are bounded: each field contributes at most one counted summary
 * line carrying the offence count and the first offending halo index and value,
 * whatever the number of bad halos. A systematically broken snapshot therefore
 * costs five lines, not n_halos lines.
 */
static void snapshot_h5_validate_links(const char *path, int64_t snapnum, int64_t n_halos,
                                       const struct RawHalo *halos) {
  int violated_fields = 0;
  const char *first_field = NULL;
  char first_range[64] = "";
  int64_t first_bad_index = 0;
  int64_t first_bad_value = 0;

  for (size_t s = 0; s < SNAPSHOT_H5_LINK_COUNT; s++) {
    const struct snapshot_h5_link_spec *spec = &SNAPSHOT_H5_LINKS[s];
    int64_t bounding_snap = 0;
    const int64_t limit = snapshot_h5_link_limit(spec->domain, snapnum, &bounding_snap);

    char range[64];
    if (spec->allow_null_link) {
      snprintf(range, sizeof(range), "-1 or [0, %" PRId64 ")", limit);
    } else {
      snprintf(range, sizeof(range), "[0, %" PRId64 ")", limit);
    }

    int64_t count = 0;
    int64_t bad_index = 0;
    int64_t bad_value = 0;
    for (int64_t i = 0; i < n_halos; i++) {
      const int64_t value = *(const int *)((const char *)&halos[i] + spec->offset);
      if (value == -1 && spec->allow_null_link) {
        continue;
      }
      if (value >= 0 && value < limit) {
        continue;
      }
      if (count == 0) {
        bad_index = i;
        bad_value = value;
      }
      count++;
    }

    if (count == 0) {
      continue;
    }
    /* One counted summary per field, never one line per halo. */
    ERROR_LOG("%s: snapshot %" PRId64 " has %" PRId64 " halo(s) whose '%s' is outside %s (bounded "
              "by snapshot %" PRId64 "); first at halo %" PRId64 " with value %" PRId64,
              path, snapnum, count, spec->name, range, bounding_snap, bad_index, bad_value);
    if (violated_fields == 0) {
      first_field = spec->name;
      snprintf(first_range, sizeof(first_range), "%s", range);
      first_bad_index = bad_index;
      first_bad_value = bad_value;
    }
    violated_fields++;
  }

  if (violated_fields > 0) {
    FATAL_ERROR("%s: snapshot %" PRId64 " has %d invalid link field(s); '%s' is %" PRId64
                " at halo %" PRId64 ", outside %s",
                path, snapnum, violated_fields, first_field, first_bad_value, first_bad_index,
                first_range);
  }
}

/**
 * @brief   Rounding-tolerance equality between a header value and its
 *          configured counterpart.
 *
 * Asserts the two are the same number, not that they agree scientifically: a
 * non-finite value on either side is rejected outright, an exact-zero pair is
 * accepted outright, and otherwise the relative difference must lie within
 * 16 ULPs of the larger magnitude.
 */
static int snapshot_h5_physical_value_agrees(double header_value, double configured_value) {
  if (!isfinite(header_value) || !isfinite(configured_value)) {
    return 0;
  }
  if (header_value == 0.0 && configured_value == 0.0) {
    return 1;
  }
  const double scale = fmax(fabs(header_value), fabs(configured_value));
  return fabs(header_value - configured_value) <= 16.0 * DBL_EPSILON * scale;
}

/**
 * @brief   Abort if one physical header attribute disagrees with the
 *          configured simulation.
 *
 * Names the file, the attribute and both values so a mismatched dataset is
 * diagnosable without a debugger. See snapshot_h5_physical_value_agrees() for
 * the tolerance this enforces.
 */
static void snapshot_h5_check_physical_value(const char *path, const char *attr_name,
                                             double header_value, double configured_value) {
  if (!snapshot_h5_physical_value_agrees(header_value, configured_value)) {
    FATAL_ERROR("%s: header attribute '%s' is %.17g but the configured simulation value is "
                "%.17g; they must agree to a rounding tolerance",
                path, attr_name, header_value, configured_value);
  }
}

/* ---------------------------------------------------------------------------
 * Reader hooks
 * ------------------------------------------------------------------------- */

/**
 * @brief   Open and fully validate the configured snapshot dataset.
 *
 * Publishes run-scoped metadata and builds the per-snapshot halo-count table
 * served by snapshot_halo_count().
 */
static void open_run_snapshot_hdf5(struct SnapshotRunInfo *info) {
  if (SNAP.is_open) {
    FATAL_ERROR("snapshot_hdf5: open_run called while a run is already open");
  }
  if (MimicConfig.Snaplistlen <= 0) {
    FATAL_ERROR("snapshot_hdf5: the snapshot list is empty (Snaplistlen = %d); there is nothing to "
                "open",
                MimicConfig.Snaplistlen);
  }

  const int64_t snapshot_count = (int64_t)MimicConfig.Snaplistlen;
  int64_t *halo_counts = mymalloc_cat(sizeof(int64_t) * (size_t)snapshot_count, MEM_TREES);

  int32_t format_version = 0;
  int64_t n_forests_total = 0;
  int64_t max_halo_rank_in_forest = 0;
  int64_t total_halos = 0;
  int64_t measured_max_forest_index = INT64_MIN;
  int64_t measured_max_halo_rank = INT64_MIN;
  int is_empty_dataset = 0;

  for (int64_t snap = 0; snap < snapshot_count; snap++) {
    char path[SNAPSHOT_HDF5_PATH_LEN];
    snapshot_h5_format_path(path, sizeof(path), snap);

    if (access(path, R_OK) != 0) {
      FATAL_ERROR("%s: no readable file for configured snapshot %" PRId64
                  "; every snapshot in the snapshot list must have a snapshot_NNN.h5 file",
                  path, snap);
    }

    hid_t file = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0) {
      FATAL_ERROR("%s: could not open the file as HDF5", path);
    }

    /* Structure before values, values before bulk reads. */
    snapshot_h5_validate_object_set(file, path);

    struct snapshot_h5_header header;
    snapshot_h5_read_header(file, path, &header);

    if (header.format_version != SNAPSHOT_HDF5_FORMAT_VERSION) {
      FATAL_ERROR("%s: header attribute 'format_version' is %" PRId32
                  " but this reader supports only version %d",
                  path, header.format_version, SNAPSHOT_HDF5_FORMAT_VERSION);
    }
    if (header.links_adjacent != 1) {
      FATAL_ERROR("%s: header attribute 'links_adjacent' is %" PRId32
                  " but format_version %d requires 1",
                  path, header.links_adjacent, SNAPSHOT_HDF5_FORMAT_VERSION);
    }
    if ((int64_t)header.snapshot_number != snap) {
      FATAL_ERROR("%s: header attribute 'snapshot_number' is %" PRId32
                  " but the filename names snapshot %" PRId64,
                  path, header.snapshot_number, snap);
    }
    if (header.n_halos < 0 || header.n_halos > (int64_t)INT32_MAX) {
      FATAL_ERROR("%s: header attribute 'n_halos' is %" PRId64 "; it must lie in [0, %" PRId64 "]",
                  path, header.n_halos, (int64_t)INT32_MAX);
    }
    if (header.scale_factor != MimicConfig.AA[snap]) {
      FATAL_ERROR("%s: header attribute 'scale_factor' is %.17g but snapshot list entry %" PRId64
                  " is %.17g; they must agree exactly",
                  path, header.scale_factor, snap, MimicConfig.AA[snap]);
    }

    /* Physical header agreement with the configured simulation (dual-driver
       Phase 5 item 8). A rounding tolerance, not a scientific one -- see
       snapshot_h5_physical_value_agrees(). particle_mass_msun_h is compared by
       multiplying the configured value up to native units, matching the
       producer's own operation (scripts/convert/hdf5_writer.py), never by
       dividing the header down. */
    snapshot_h5_check_physical_value(path, "box_size_mpc_h", header.box_size_mpc_h,
                                     MimicConfig.BoxSize);
    snapshot_h5_check_physical_value(path, "omega_matter", header.omega_matter, MimicConfig.Omega);
    snapshot_h5_check_physical_value(path, "omega_lambda", header.omega_lambda,
                                     MimicConfig.OmegaLambda);
    snapshot_h5_check_physical_value(path, "hubble_h", header.hubble_h, MimicConfig.Hubble_h);
    snapshot_h5_check_physical_value(path, "particle_mass_msun_h", header.particle_mass_msun_h,
                                     MimicConfig.PartMass * 1e10);

    if (snap == 0) {
      format_version = header.format_version;
      n_forests_total = header.n_forests_total;
      max_halo_rank_in_forest = header.max_halo_rank_in_forest;
      is_empty_dataset = (n_forests_total == SNAPSHOT_HDF5_EMPTY_N_FORESTS &&
                          max_halo_rank_in_forest == SNAPSHOT_HDF5_EMPTY_MAX_RANK);
      if (!is_empty_dataset && (n_forests_total < 0 || max_halo_rank_in_forest < 0)) {
        FATAL_ERROR("%s: header attributes 'n_forests_total' (%" PRId64
                    ") and 'max_halo_rank_in_forest' (%" PRId64
                    ") must both be non-negative, or exactly the empty-dataset sentinel (%" PRId64
                    ", %" PRId64 ")",
                    path, n_forests_total, max_halo_rank_in_forest, SNAPSHOT_HDF5_EMPTY_N_FORESTS,
                    SNAPSHOT_HDF5_EMPTY_MAX_RANK);
      }
    } else {
      if (header.n_forests_total != n_forests_total) {
        FATAL_ERROR("%s: header attribute 'n_forests_total' is %" PRId64
                    " but snapshot 0 declares %" PRId64 "; it is run-scoped and must be identical "
                    "in every file",
                    path, header.n_forests_total, n_forests_total);
      }
      if (header.max_halo_rank_in_forest != max_halo_rank_in_forest) {
        FATAL_ERROR("%s: header attribute 'max_halo_rank_in_forest' is %" PRId64
                    " but snapshot 0 declares %" PRId64 "; it is run-scoped and must be identical "
                    "in every file",
                    path, header.max_halo_rank_in_forest, max_halo_rank_in_forest);
      }
    }

    if (is_empty_dataset && header.n_halos > 0) {
      FATAL_ERROR("%s: the header carries the empty-dataset sentinel (n_forests_total %" PRId64
                  ", max_halo_rank_in_forest %" PRId64 ") but declares %" PRId64 " halos",
                  path, n_forests_total, max_halo_rank_in_forest, header.n_halos);
    }
    if (!is_empty_dataset && n_forests_total == 0 && header.n_halos > 0) {
      FATAL_ERROR("%s: header attribute 'n_forests_total' is 0 but the file declares %" PRId64
                  " halos",
                  path, header.n_halos);
    }

    snapshot_h5_validate_halo_datasets(file, path, header.n_halos);

    /* Invariant 5's measured-data component. Bounded block scans only. */
    snapshot_h5_scan_snapnum(file, path, header.n_halos, header.snapshot_number);
    if (header.n_halos > 0) {
      /* Upper bound stays open here; it is checked below against the run-scoped measured max. */
      const int64_t file_max_rank =
          snapshot_h5_scan_i64_max(file, path, "HaloRankInForest", header.n_halos, 0, INT64_MAX);
      if (file_max_rank > measured_max_halo_rank) {
        measured_max_halo_rank = file_max_rank;
      }
      const int64_t file_max_forest = snapshot_h5_scan_i64_max(
          file, path, "ForestIndex", header.n_halos, 0, n_forests_total - 1);
      if (file_max_forest > measured_max_forest_index) {
        measured_max_forest_index = file_max_forest;
      }
    }

    halo_counts[snap] = header.n_halos;
    total_halos += header.n_halos;

    if (H5Fclose(file) < 0) {
      FATAL_ERROR("%s: could not close the file", path);
    }
  }

  if (is_empty_dataset) {
    if (total_halos > 0) {
      FATAL_ERROR("The dataset under '%s' carries the empty-dataset sentinel (n_forests_total "
                  "%" PRId64 ", max_halo_rank_in_forest %" PRId64 ") but holds %" PRId64 " halos",
                  MimicConfig.SimulationDir, n_forests_total, max_halo_rank_in_forest, total_halos);
    }
  } else if (total_halos > 0) {
    if (measured_max_halo_rank != max_halo_rank_in_forest) {
      FATAL_ERROR("The dataset under '%s' declares max_halo_rank_in_forest %" PRId64
                  " but the measured maximum of '/halos/HaloRankInForest' is %" PRId64,
                  MimicConfig.SimulationDir, max_halo_rank_in_forest, measured_max_halo_rank);
    }
    if (measured_max_forest_index != n_forests_total - 1) {
      FATAL_ERROR("The dataset under '%s' declares n_forests_total %" PRId64
                  " but the measured maximum of '/halos/ForestIndex' is %" PRId64 "; it must be "
                  "%" PRId64,
                  MimicConfig.SimulationDir, n_forests_total, measured_max_forest_index,
                  n_forests_total - 1);
    }
  }

  /* The identity bounds the format requires to be checked at startup
     (docs/dev/SNAPSHOT-HDF5-FORMAT.md), verified before anything is published so
     an unencodable dataset never reaches a caller. */
  struct SnapshotRunInfo candidate;
  candidate.snapshot_count = snapshot_count;
  candidate.format_version = format_version;
  candidate.n_forests_total = n_forests_total;
  candidate.max_halo_rank_in_forest = max_halo_rank_in_forest;

  if (!snapshot_identity_bounds_valid(&candidate, MimicConfig.UniqueGalaxyIDMultiplier)) {
    FATAL_ERROR("The dataset under '%s' declares identity bounds (n_forests_total %" PRId64
                ", max_halo_rank_in_forest %" PRId64
                ") that are not encodable with simulation.unique_galaxy_id_multiplier %" PRId64,
                MimicConfig.SimulationDir, n_forests_total, max_halo_rank_in_forest,
                MimicConfig.UniqueGalaxyIDMultiplier);
  }

  SNAP.is_open = 1;
  SNAP.snapshot_count = snapshot_count;
  SNAP.halo_counts = halo_counts;
  SNAP.loaded_slabs = 0;
  SNAP.info = candidate;

  *info = SNAP.info;
}

/** @brief Release everything open_run acquired. */
static void close_run_snapshot_hdf5(void) {
  if (!SNAP.is_open) {
    FATAL_ERROR("snapshot_hdf5: close_run called with no open run");
  }
  /* Slabs are reader-owned but caller-scoped: closing the run underneath a
     loaded slab would leave the caller holding a dangling array. */
  if (SNAP.loaded_slabs > 0) {
    FATAL_ERROR("snapshot_hdf5: close_run called with %" PRId64
                " slab(s) still loaded; every slab must be released first",
                SNAP.loaded_slabs);
  }

  myfree(SNAP.halo_counts);
  SNAP.halo_counts = NULL;
  SNAP.snapshot_count = 0;
  SNAP.is_open = 0;
  memset(&SNAP.info, 0, sizeof(SNAP.info));
}

/** @brief Halo count of one snapshot, from the table open_run built. */
static int64_t snapshot_halo_count_snapshot_hdf5(int64_t snapnum) {
  if (!SNAP.is_open) {
    FATAL_ERROR("snapshot_hdf5: snapshot_halo_count called with no open run");
  }
  if (snapnum < 0 || snapnum >= SNAP.snapshot_count) {
    FATAL_ERROR("snapshot_hdf5: snapshot %" PRId64 " is outside the open run's range [0, %" PRId64
                ")",
                snapnum, SNAP.snapshot_count);
  }
  return SNAP.halo_counts[snapnum];
}

/**
 * @brief   Load one snapshot into a reader-owned slab and validate its links.
 *
 * The destination handle must be empty: overwriting a loaded one would leak the
 * arrays it holds, so that is an abort rather than a silent replacement. A
 * snapshot holding no halos is a legal result -- the handle then carries its
 * snapshot number with three NULL arrays, which snapshot_slab_is_empty()
 * correctly reports as loaded.
 */
static void load_slab_snapshot_hdf5(int64_t snapnum, struct SnapshotSlab *slab) {
  if (!SNAP.is_open) {
    FATAL_ERROR("snapshot_hdf5: load_slab called with no open run");
  }
  if (snapnum < 0 || snapnum >= SNAP.snapshot_count) {
    FATAL_ERROR("snapshot_hdf5: snapshot %" PRId64 " is outside the open run's range [0, %" PRId64
                ")",
                snapnum, SNAP.snapshot_count);
  }
  if (!snapshot_slab_is_empty(slab)) {
    FATAL_ERROR("snapshot_hdf5: load_slab into a slab already holding snapshot %" PRId64
                "; release it before loading snapshot %" PRId64,
                slab->snapnum, snapnum);
  }

  const int64_t n_halos = SNAP.halo_counts[snapnum];
  char path[SNAPSHOT_HDF5_PATH_LEN];
  snapshot_h5_format_path(path, sizeof(path), snapnum);

  struct RawHalo *halos = NULL;
  int64_t *forest_index = NULL;
  int64_t *halo_rank_in_forest = NULL;
  if (n_halos > 0) {
    hid_t file = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0) {
      FATAL_ERROR("%s: could not open the file as HDF5 while loading snapshot %" PRId64, path,
                  snapnum);
    }

    halos = mymalloc_cat(sizeof(struct RawHalo) * (size_t)n_halos, MEM_TREES);
    snapshot_h5_fill_halos(file, path, n_halos, halos);

    forest_index = mymalloc_cat(sizeof(int64_t) * (size_t)n_halos, MEM_TREES);
    halo_rank_in_forest = mymalloc_cat(sizeof(int64_t) * (size_t)n_halos, MEM_TREES);
    snapshot_h5_fill_identity(file, path, n_halos, forest_index, halo_rank_in_forest);

    if (H5Fclose(file) < 0) {
      FATAL_ERROR("%s: could not close the file after loading snapshot %" PRId64, path, snapnum);
    }

    snapshot_h5_validate_links(path, snapnum, n_halos, halos);
  }

  slab->snapnum = snapnum;
  slab->nhalos = n_halos;
  slab->halos = halos;
  slab->forest_index = forest_index;
  slab->halo_rank_in_forest = halo_rank_in_forest;
  SNAP.loaded_slabs++;
}

/** @brief Release a loaded slab. A slab already empty is left untouched. */
static void release_slab_snapshot_hdf5(struct SnapshotSlab *slab) {
  if (snapshot_slab_is_empty(slab)) {
    return;
  }
  if (slab->halos != NULL) {
    myfree(slab->halos);
  }
  if (slab->forest_index != NULL) {
    myfree(slab->forest_index);
  }
  if (slab->halo_rank_in_forest != NULL) {
    myfree(slab->halo_rank_in_forest);
  }
  *slab = snapshot_slab_empty();
  if (SNAP.loaded_slabs > 0) {
    SNAP.loaded_slabs--;
  }
}

/* Snapshot-ordered HDF5: one file per snapshot, validated in full at open, read
   one snapshot at a time into a reader-owned slab. */
const struct SnapshotReader SnapshotHDF5Reader = {
    .name = "snapshot_hdf5",
    .processing_order = INPUT_PROCESSING_ORDER_SNAPSHOT,
    .open_run = open_run_snapshot_hdf5,
    .close_run = close_run_snapshot_hdf5,
    .snapshot_halo_count = snapshot_halo_count_snapshot_hdf5,
    .load_slab = load_slab_snapshot_hdf5,
    .release_slab = release_slab_snapshot_hdf5,
};

#endif /* HDF5 */
