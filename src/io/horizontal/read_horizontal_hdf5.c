/**
 * @file    horizontal/read_horizontal_hdf5.c
 * @brief   horizontal_hdf5 reader: run lifecycle, validation, and count table.
 *
 * Reads two horizontal HDF5 contracts, one `snapshot_NNN.h5` file per snapshot
 * under MimicConfig.SimulationDir, dispatching on each file's `format_version`
 * (runtime plan Gate R0-1(a)):
 *
 *   - version 2, the frozen contract of docs/dev/HORIZONTAL-HDF5-FORMAT.md:
 *     exactly the `/header` and `/halos` groups, int32 adjacent links. Its
 *     validation path is exactly what it was before version 3 was added.
 *   - version 3, docs/dev/HORIZONTAL-HDF5-FORMAT.md (section "Version 3"): `/header`,
 *     `/halos` and `/schema`, int64 links resolved through three explicit
 *     target-snapshot columns (gaps allowed), a `SourceHaloID` row key, and
 *     producer-declared payload units that `/schema` records and the compiled
 *     simulation package must agree with.
 *
 * Version 1 data (from before `fix_flybys` was removed;
 * docs/dev/SHIN-UCHUU-FLYBY-DEFECT-ADDENDUM.md) and any other version are
 * rejected outright, and a dataset never mixes versions: snapshot 0 fixes the
 * version every other file must declare.
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
 * header is therefore rejected rather than read. Header values additionally
 * require the five physical attributes (box_size_mpc_h, particle_mass_msun_h,
 * omega_matter, omega_lambda, hubble_h) to agree with MimicConfig's configured
 * simulation, not only with the format. Every failure aborts naming the file
 * and the offending object, attribute or value; nothing is repaired.
 *
 * The open-time data scans (format invariant 5) are fixed-size hyperslab reads
 * accumulating running maxima: no buffer there is proportional to n_halos. The
 * slab load necessarily allocates per halo, since a slab *is* the snapshot's
 * halo population.
 *
 * Version 3 adds, at open: the `/schema` group's structure and vocabularies,
 * its agreement with every `/halos` dataset's dtype and shape, its identity
 * across files (with `source_format` and `column_mapping_sha256`), and its
 * agreement with the package's compiled declarations (generated
 * catalog_field_metadata.inc) for every field the package declares. `/schema`
 * fields the package does not declare are validated for internal consistency
 * and never materialised (Gate R0-8(a)). No object under the root, `/halos` or
 * `/schema` may be a soft or external link.
 *
 * Link validation at load checks index ranges -- for version 3 against the
 * n_halos of the file each target-snapshot column names, with the -1-iff--1
 * biconditionals and the direction rules of format invariants 1 and 2. Chain
 * topology -- cycle-freedom, FoF self-reference, progenitor round-trip
 * closure -- is a producer obligation the converter's validation battery and
 * the topology gate already discharge, and is deliberately not re-derived here.
 *
 * Slabs are filled through fixed-size hyperslab blocks, so the only
 * allocations proportional to n_halos are the slab arrays themselves.
 *
 * The small HDF5 helpers below are local by design rather than lifted out of
 * vertical/read_ctrees_hdf5.c, whose byte-identical output is this phase's gate and
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
#include "horizontal/reader.h"
#include "types.h"

/* Supported on-disk contract version (docs/dev/HORIZONTAL-HDF5-FORMAT.md).
   Bumped 1 -> 2 when fix_flybys was removed (MostBoundID is always positive
   now; docs/dev/SHIN-UCHUU-FLYBY-DEFECT-ADDENDUM.md, decision D3). There is
   no legacy-read path: a version 1 file is rejected outright. */
#define HORIZONTAL_HDF5_FORMAT_VERSION 2

/* Halos read per hyperslab during the data scans. Fixed by construction so scan
   memory is bounded independently of snapshot size. */
#define HORIZONTAL_HDF5_SCAN_BLOCK 8192

/* Room for "<SimulationDir>/snapshot_NNN.h5". */
#define HORIZONTAL_HDF5_PATH_LEN (MAX_STRING_LEN + 32)

/* The empty-dataset sentinel the converter stamps when a dataset holds no halos
   in any snapshot (scripts/convert/links.py). Local names for the shared
   contract values in horizontal/reader.h, which the identity-bounds check also
   consults. */
#define HORIZONTAL_HDF5_EMPTY_N_FORESTS HORIZONTAL_EMPTY_N_FORESTS
#define HORIZONTAL_HDF5_EMPTY_MAX_RANK HORIZONTAL_EMPTY_MAX_RANK

/* ---------------------------------------------------------------------------
 * Contract tables
 *
 * The names, dtypes and shapes below are the normative format_version = 2
 * record. They are stated here rather than derived from the compiled-in
 * simulation package on purpose: the reader validates a file against the
 * format, not against whatever a package happens to declare.
 * ------------------------------------------------------------------------- */

enum horizontal_h5_scalar_type {
  HORIZONTAL_H5_I32 = 0,
  HORIZONTAL_H5_I64,
  HORIZONTAL_H5_F32,
  HORIZONTAL_H5_F64,
};

/** One file's header, as read. Every contract attribute is read and
    dtype-checked; the five physical values (box_size_mpc_h,
    particle_mass_msun_h, omega_matter, omega_lambda, hubble_h) are also
    compared against the configured simulation in open_run_horizontal_hdf5(). */
struct horizontal_h5_header {
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

struct horizontal_h5_attr_spec {
  const char *name;
  enum horizontal_h5_scalar_type type;
  size_t offset; /* destination field within struct horizontal_h5_header */
};

#define HORIZONTAL_H5_HEADER_ATTR(attr, dtype)                                                     \
  {#attr, dtype, offsetof(struct horizontal_h5_header, attr)}

static const struct horizontal_h5_attr_spec HORIZONTAL_H5_HEADER_ATTRS[] = {
    HORIZONTAL_H5_HEADER_ATTR(format_version, HORIZONTAL_H5_I32),
    HORIZONTAL_H5_HEADER_ATTR(links_adjacent, HORIZONTAL_H5_I32),
    HORIZONTAL_H5_HEADER_ATTR(snapshot_number, HORIZONTAL_H5_I32),
    HORIZONTAL_H5_HEADER_ATTR(scale_factor, HORIZONTAL_H5_F64),
    HORIZONTAL_H5_HEADER_ATTR(n_halos, HORIZONTAL_H5_I64),
    HORIZONTAL_H5_HEADER_ATTR(n_forests_total, HORIZONTAL_H5_I64),
    HORIZONTAL_H5_HEADER_ATTR(max_halo_rank_in_forest, HORIZONTAL_H5_I64),
    HORIZONTAL_H5_HEADER_ATTR(box_size_mpc_h, HORIZONTAL_H5_F64),
    HORIZONTAL_H5_HEADER_ATTR(particle_mass_msun_h, HORIZONTAL_H5_F64),
    HORIZONTAL_H5_HEADER_ATTR(omega_matter, HORIZONTAL_H5_F64),
    HORIZONTAL_H5_HEADER_ATTR(omega_lambda, HORIZONTAL_H5_F64),
    HORIZONTAL_H5_HEADER_ATTR(hubble_h, HORIZONTAL_H5_F64),
};
#define HORIZONTAL_H5_HEADER_ATTR_COUNT                                                            \
  (sizeof(HORIZONTAL_H5_HEADER_ATTRS) / sizeof(HORIZONTAL_H5_HEADER_ATTRS[0]))

struct horizontal_h5_dataset_spec {
  const char *name;
  enum horizontal_h5_scalar_type type;
  int ncols; /* 0 = rank-1 column, 3 = [n_halos, 3] vector */
};

static const struct horizontal_h5_dataset_spec HORIZONTAL_H5_HALO_DATASETS[] = {
    {"Descendant", HORIZONTAL_H5_I32, 0},
    {"FirstProgenitor", HORIZONTAL_H5_I32, 0},
    {"NextProgenitor", HORIZONTAL_H5_I32, 0},
    {"FirstHaloInFOFgroup", HORIZONTAL_H5_I32, 0},
    {"NextHaloInFOFgroup", HORIZONTAL_H5_I32, 0},
    {"Len", HORIZONTAL_H5_I32, 0},
    {"SnapNum", HORIZONTAL_H5_I32, 0},
    {"M_Crit200", HORIZONTAL_H5_F32, 0},
    {"Pos", HORIZONTAL_H5_F32, 3},
    {"Vel", HORIZONTAL_H5_F32, 3},
    {"Spin", HORIZONTAL_H5_F32, 3},
    {"VelDisp", HORIZONTAL_H5_F32, 0},
    {"Vmax", HORIZONTAL_H5_F32, 0},
    {"MostBoundID", HORIZONTAL_H5_I64, 0},
    {"ForestIndex", HORIZONTAL_H5_I64, 0},
    {"HaloRankInForest", HORIZONTAL_H5_I64, 0},
};
#define HORIZONTAL_H5_HALO_DATASET_COUNT                                                           \
  (sizeof(HORIZONTAL_H5_HALO_DATASETS) / sizeof(HORIZONTAL_H5_HALO_DATASETS[0]))

/* ---------------------------------------------------------------------------
 * Version 3 contract tables (docs/dev/HORIZONTAL-HDF5-FORMAT.md, section "Version 3")
 *
 * Like the version 2 tables above, these state the format, not a package: the
 * package's own declarations are compared against a file's `/schema` only for
 * the fields the package declares (horizontal_h5_v3_validate_package()).
 * ------------------------------------------------------------------------- */

#define HORIZONTAL_HDF5_FORMAT_VERSION_V3 3

/* The two header strings version 3 adds, both fixed-length ASCII. */
struct horizontal_h5_string_attr_spec {
  const char *name;
  size_t size; /* exact on-disk length in bytes */
};

#define HORIZONTAL_H5_V3_SOURCE_FORMAT_LEN 32
#define HORIZONTAL_H5_V3_MAPPING_DIGEST_LEN 64

static const struct horizontal_h5_string_attr_spec HORIZONTAL_H5_V3_STRING_ATTRS[] = {
    {"source_format", HORIZONTAL_H5_V3_SOURCE_FORMAT_LEN},
    {"column_mapping_sha256", HORIZONTAL_H5_V3_MAPPING_DIGEST_LEN},
};
#define HORIZONTAL_H5_V3_STRING_ATTR_COUNT                                                         \
  (sizeof(HORIZONTAL_H5_V3_STRING_ATTRS) / sizeof(HORIZONTAL_H5_V3_STRING_ATTRS[0]))

/* The adapters a version 3 `source_format` may name. */
static const char *const HORIZONTAL_H5_V3_SOURCE_FORMATS[] = {
    "consistent_trees_ascii",
    "consistent_trees_hdf5",
    "lhalo_binary",
};
#define HORIZONTAL_H5_V3_SOURCE_FORMAT_COUNT                                                       \
  (sizeof(HORIZONTAL_H5_V3_SOURCE_FORMATS) / sizeof(HORIZONTAL_H5_V3_SOURCE_FORMATS[0]))

/* The fixed topology and identity tables: never declared in `/schema`. */
static const struct horizontal_h5_dataset_spec HORIZONTAL_H5_V3_FIXED_DATASETS[] = {
    {"Descendant", HORIZONTAL_H5_I64, 0},
    {"FirstProgenitor", HORIZONTAL_H5_I64, 0},
    {"NextProgenitor", HORIZONTAL_H5_I64, 0},
    {"FirstHaloInFOFgroup", HORIZONTAL_H5_I64, 0},
    {"NextHaloInFOFgroup", HORIZONTAL_H5_I64, 0},
    {"DescendantSnapshot", HORIZONTAL_H5_I32, 0},
    {"FirstProgenitorSnapshot", HORIZONTAL_H5_I32, 0},
    {"NextProgenitorSnapshot", HORIZONTAL_H5_I32, 0},
    {"SourceHaloID", HORIZONTAL_H5_I64, 0},
    {"ForestIndex", HORIZONTAL_H5_I64, 0},
    {"HaloRankInForest", HORIZONTAL_H5_I64, 0},
};
#define HORIZONTAL_H5_V3_FIXED_DATASET_COUNT                                                       \
  (sizeof(HORIZONTAL_H5_V3_FIXED_DATASETS) / sizeof(HORIZONTAL_H5_V3_FIXED_DATASETS[0]))

/* `/schema` `type` vocabulary: the property generator's own
   (scripts/generate_properties.py TYPE_MAP), each fixing dtype and shape. */
struct horizontal_h5_schema_type_spec {
  const char *name;
  enum horizontal_h5_scalar_type type;
  int ncols; /* 0 = [n_halos], 3 = [n_halos, 3] */
};

static const struct horizontal_h5_schema_type_spec HORIZONTAL_H5_V3_SCHEMA_TYPES[] = {
    {"int", HORIZONTAL_H5_I32, 0},      {"long long", HORIZONTAL_H5_I64, 0},
    {"float", HORIZONTAL_H5_F32, 0},    {"double", HORIZONTAL_H5_F64, 0},
    {"vec3_int", HORIZONTAL_H5_I32, 3}, {"vec3_float", HORIZONTAL_H5_F32, 3},
};
#define HORIZONTAL_H5_V3_SCHEMA_TYPE_COUNT                                                         \
  (sizeof(HORIZONTAL_H5_V3_SCHEMA_TYPES) / sizeof(HORIZONTAL_H5_V3_SCHEMA_TYPES[0]))

/* `/schema` `h_convention` vocabulary, likewise the generator's own. */
static const char *const HORIZONTAL_H5_V3_H_CONVENTIONS[] = {"carried", "free", "none"};
#define HORIZONTAL_H5_V3_H_CONVENTION_COUNT                                                        \
  (sizeof(HORIZONTAL_H5_V3_H_CONVENTIONS) / sizeof(HORIZONTAL_H5_V3_H_CONVENTIONS[0]))

/* The exactly four attributes of every `/schema` subgroup, in storage order of
   struct horizontal_h5_schema_entry.attrs. */
enum horizontal_h5_schema_attr {
  HORIZONTAL_H5_SCHEMA_TYPE = 0,
  HORIZONTAL_H5_SCHEMA_UNITS,
  HORIZONTAL_H5_SCHEMA_H_CONVENTION,
  HORIZONTAL_H5_SCHEMA_DESCRIPTION,
  HORIZONTAL_H5_SCHEMA_ATTR_COUNT,
};
static const char *const HORIZONTAL_H5_V3_SCHEMA_ATTRS[HORIZONTAL_H5_SCHEMA_ATTR_COUNT] = {
    "type",
    "units",
    "h_convention",
    "description",
};

/* The payload fields the format table names. Each must be declared in
   `/schema`; fixed_type is the format-fixed `type` (NULL where the producer
   declares it) and ncols the shape the table fixes either way. */
struct horizontal_h5_payload_spec {
  const char *name;
  const char *fixed_type;
  int ncols;
};

static const struct horizontal_h5_payload_spec HORIZONTAL_H5_V3_PAYLOAD[] = {
    {"SnapNum", "int", 0}, {"Len", "int", 0}, {"M_Crit200", NULL, 0},
    {"Pos", NULL, 3},      {"Vel", NULL, 3},  {"Spin", NULL, 3},
    {"VelDisp", NULL, 0},  {"Vmax", NULL, 0}, {"MostBoundID", "long long", 0},
};
#define HORIZONTAL_H5_V3_PAYLOAD_COUNT                                                             \
  (sizeof(HORIZONTAL_H5_V3_PAYLOAD) / sizeof(HORIZONTAL_H5_V3_PAYLOAD[0]))

/** One parsed `/schema` subgroup. Strings are owned (mymalloc) copies. */
struct horizontal_h5_schema_entry {
  char *name;
  char *attrs[HORIZONTAL_H5_SCHEMA_ATTR_COUNT];
  const struct horizontal_h5_schema_type_spec *storage; /* resolved from attrs[TYPE] */
};

/** One file's parsed `/schema`, entries in HDF5 name order. */
struct horizontal_h5_schema {
  size_t count;
  struct horizontal_h5_schema_entry *entries;
};

/** The two version 3 header strings of one file, NUL-terminated. */
struct horizontal_h5_v3_strings {
  char source_format[HORIZONTAL_H5_V3_SOURCE_FORMAT_LEN + 1];
  char column_mapping_sha256[HORIZONTAL_H5_V3_MAPPING_DIGEST_LEN + 1];
};

/* The compiled package's catalog declarations, one entry per
   halo_properties.yaml field (scripts/generate_properties.py
   generate_catalog_field_metadata_inc). member_size is the struct RawHalo
   storage the reader will fill. */
struct horizontal_h5_catalog_field {
  const char *member;
  const char *dataset;
  const char *type;
  const char *units;
  const char *h_convention;
  const char *core_role; /* "" when the field provides no core role */
  const char *role_kind; /* "tree_link", "index", "count", "mass" or "" */
  size_t member_size;
};

#define CATALOG_FIELD(member, dataset, type, units, h_convention, core_role, role_kind)            \
  {#member,      dataset,   type,      units,                                                      \
   h_convention, core_role, role_kind, sizeof(((struct RawHalo *)0)->member)},
static const struct horizontal_h5_catalog_field HORIZONTAL_H5_CATALOG_FIELDS[] = {
#include "../../include/generated/catalog_field_metadata.inc"
};
#undef CATALOG_FIELD
#define HORIZONTAL_H5_CATALOG_FIELD_COUNT                                                          \
  (sizeof(HORIZONTAL_H5_CATALOG_FIELDS) / sizeof(HORIZONTAL_H5_CATALOG_FIELDS[0]))

/** Run-scoped reader state. One reader instance per process, so a file-static
    record is sufficient (mirrors the vertical readers). */
struct horizontal_hdf5_run {
  int is_open;
  int64_t snapshot_count;
  int64_t *halo_counts; /* [snapshot_count] */
  int64_t loaded_slabs; /* slabs handed out and not yet released */
  struct HorizontalRunInfo info;
};
static struct horizontal_hdf5_run SNAP;

/* Fixed-size scan buffers, never resized: the scans below read in blocks of
   HORIZONTAL_HDF5_SCAN_BLOCK halos regardless of snapshot size. */
static int32_t horizontal_h5_scan_i32[HORIZONTAL_HDF5_SCAN_BLOCK];
static int64_t horizontal_h5_scan_i64[HORIZONTAL_HDF5_SCAN_BLOCK];

/* ---------------------------------------------------------------------------
 * Local HDF5 helpers
 * ------------------------------------------------------------------------- */

/** @brief Human-readable name of a contract dtype, for diagnostics. */
static const char *horizontal_h5_type_name(enum horizontal_h5_scalar_type type) {
  switch (type) {
  case HORIZONTAL_H5_I32:
    return "int32";
  case HORIZONTAL_H5_I64:
    return "int64";
  case HORIZONTAL_H5_F32:
    return "float32";
  case HORIZONTAL_H5_F64:
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
static int horizontal_h5_type_matches(hid_t dtype, enum horizontal_h5_scalar_type expected) {
  const H5T_class_t cls = H5Tget_class(dtype);
  const size_t size = H5Tget_size(dtype);

  switch (expected) {
  case HORIZONTAL_H5_I32:
    return cls == H5T_INTEGER && size == 4 && H5Tget_sign(dtype) == H5T_SGN_2;
  case HORIZONTAL_H5_I64:
    return cls == H5T_INTEGER && size == 8 && H5Tget_sign(dtype) == H5T_SGN_2;
  case HORIZONTAL_H5_F32:
    return cls == H5T_FLOAT && size == 4;
  case HORIZONTAL_H5_F64:
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
 * horizontal_h5_type_matches() accepts by design.
 */
static hid_t horizontal_h5_native_type(enum horizontal_h5_scalar_type type) {
  switch (type) {
  case HORIZONTAL_H5_I32:
    return H5T_NATIVE_INT32;
  case HORIZONTAL_H5_I64:
    return H5T_NATIVE_INT64;
  case HORIZONTAL_H5_F32:
    return H5T_NATIVE_FLOAT;
  case HORIZONTAL_H5_F64:
    return H5T_NATIVE_DOUBLE;
  }
  return H5I_INVALID_HID;
}

/** @brief Build "<SimulationDir>/snapshot_NNN.h5" with a fixed format string. */
static void horizontal_h5_format_path(char *path, size_t path_size, int64_t snapnum) {
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

/** Iteration state for horizontal_h5_reject_unknown_attr(). */
struct horizontal_h5_attr_scan {
  const char *path;
};

/** @brief H5Aiterate2 callback rejecting any attribute the contract omits. */
static herr_t horizontal_h5_reject_unknown_attr(hid_t location_id, const char *attr_name,
                                                const H5A_info_t *ainfo, void *op_data) {
  const struct horizontal_h5_attr_scan *scan = (const struct horizontal_h5_attr_scan *)op_data;
  (void)location_id;
  (void)ainfo;

  for (size_t i = 0; i < HORIZONTAL_H5_HEADER_ATTR_COUNT; i++) {
    if (strcmp(HORIZONTAL_H5_HEADER_ATTRS[i].name, attr_name) == 0) {
      return 0;
    }
  }
  FATAL_ERROR("%s: '/header' carries unexpected attribute '%s'; format_version %d defines exactly "
              "%zu header attributes",
              scan->path, attr_name, HORIZONTAL_HDF5_FORMAT_VERSION,
              (size_t)HORIZONTAL_H5_HEADER_ATTR_COUNT);
}

/** @brief Validate the root object set: exactly the groups /header and /halos. */
static void horizontal_h5_validate_object_set(hid_t file, const char *path) {
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
                  path, name, HORIZONTAL_HDF5_FORMAT_VERSION);
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
static void horizontal_h5_read_header_attr(hid_t file, const char *path,
                                           const struct horizontal_h5_attr_spec *spec, void *dst) {
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
  if (!horizontal_h5_type_matches(dtype, spec->type)) {
    FATAL_ERROR("%s: header attribute '%s' must be %s on disk; found HDF5 type class %d of %zu "
                "bytes",
                path, spec->name, horizontal_h5_type_name(spec->type), (int)H5Tget_class(dtype),
                H5Tget_size(dtype));
  }

  if (H5Aread(attr, horizontal_h5_native_type(spec->type), dst) < 0) {
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
static void horizontal_h5_read_header(hid_t file, const char *path,
                                      struct horizontal_h5_header *header) {
  hid_t group = H5Gopen2(file, "/header", H5P_DEFAULT);
  if (group < 0) {
    FATAL_ERROR("%s: could not open group '/header'", path);
  }
  struct horizontal_h5_attr_scan scan = {path};
  hsize_t idx = 0;
  if (H5Aiterate2(group, H5_INDEX_NAME, H5_ITER_INC, &idx, horizontal_h5_reject_unknown_attr,
                  &scan) < 0) {
    FATAL_ERROR("%s: could not enumerate the attributes of '/header'", path);
  }
  if (H5Gclose(group) < 0) {
    FATAL_ERROR("%s: could not close group '/header'", path);
  }

  for (size_t i = 0; i < HORIZONTAL_H5_HEADER_ATTR_COUNT; i++) {
    horizontal_h5_read_header_attr(file, path, &HORIZONTAL_H5_HEADER_ATTRS[i],
                                   (char *)header + HORIZONTAL_H5_HEADER_ATTRS[i].offset);
  }
}

/**
 * @brief   Validate the /halos dataset set, dtypes, ranks and shapes.
 *
 * Runs before any bulk read, so a dataset of the wrong shape is rejected rather
 * than read into a buffer sized from the header.
 */
static void horizontal_h5_validate_halo_datasets(hid_t file, const char *path, int64_t n_halos) {
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
    for (size_t s = 0; s < HORIZONTAL_H5_HALO_DATASET_COUNT; s++) {
      if (strcmp(HORIZONTAL_H5_HALO_DATASETS[s].name, name) == 0) {
        known = 1;
        break;
      }
    }
    if (!known) {
      FATAL_ERROR("%s: unexpected dataset '/halos/%s'; format_version %d defines exactly %zu halo "
                  "datasets",
                  path, name, HORIZONTAL_HDF5_FORMAT_VERSION,
                  (size_t)HORIZONTAL_H5_HALO_DATASET_COUNT);
    }
  }

  for (size_t s = 0; s < HORIZONTAL_H5_HALO_DATASET_COUNT; s++) {
    const struct horizontal_h5_dataset_spec *spec = &HORIZONTAL_H5_HALO_DATASETS[s];

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
    if (!horizontal_h5_type_matches(dtype, spec->type)) {
      FATAL_ERROR("%s: dataset '/halos/%s' must be %s on disk; found HDF5 type class %d of %zu "
                  "bytes",
                  path, spec->name, horizontal_h5_type_name(spec->type), (int)H5Tget_class(dtype),
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
static void horizontal_h5_read_block(hid_t dset, hid_t space, const char *path,
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
static void horizontal_h5_open_scan(hid_t file, const char *path, const char *dataset_name,
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

static void horizontal_h5_close_scan(const char *path, const char *dataset_name, hid_t dset,
                                     hid_t space) {
  if (H5Sclose(space) < 0 || H5Dclose(dset) < 0) {
    FATAL_ERROR("%s: could not close dataset '/halos/%s' after validation", path, dataset_name);
  }
}

/**
 * @brief   Invariant 5: every SnapNum equals the file's snapshot_number.
 */
static void horizontal_h5_scan_snapnum(hid_t file, const char *path, int64_t n_halos,
                                       int32_t snapshot_number) {
  hid_t dset, space;
  horizontal_h5_open_scan(file, path, "SnapNum", &dset, &space);

  for (int64_t offset = 0; offset < n_halos; offset += HORIZONTAL_HDF5_SCAN_BLOCK) {
    const int64_t remaining = n_halos - offset;
    const int64_t count =
        remaining < HORIZONTAL_HDF5_SCAN_BLOCK ? remaining : (int64_t)HORIZONTAL_HDF5_SCAN_BLOCK;
    horizontal_h5_read_block(dset, space, path, "SnapNum", H5T_NATIVE_INT32, (hsize_t)offset,
                             (hsize_t)count, horizontal_h5_scan_i32);
    for (int64_t i = 0; i < count; i++) {
      if (horizontal_h5_scan_i32[i] != snapshot_number) {
        FATAL_ERROR("%s: '/halos/SnapNum' is %" PRId32 " at halo %" PRId64
                    " but the header snapshot_number is %" PRId32,
                    path, horizontal_h5_scan_i32[i], offset + i, snapshot_number);
      }
    }
  }

  horizontal_h5_close_scan(path, "SnapNum", dset, space);
}

/**
 * @brief   Running maximum of an int64 /halos column, with a range check.
 * @param   lower_bound  Inclusive lower bound; a smaller value aborts.
 * @param   upper_bound  Inclusive upper bound; a larger value aborts. Pass
 *                       (INT64_MIN, INT64_MAX) to accept any value.
 * @return  Maximum over this file, or INT64_MIN if it holds no halos.
 */
static int64_t horizontal_h5_scan_i64_max(hid_t file, const char *path, const char *dataset_name,
                                          int64_t n_halos, int64_t lower_bound,
                                          int64_t upper_bound) {
  hid_t dset, space;
  int64_t measured = INT64_MIN;

  horizontal_h5_open_scan(file, path, dataset_name, &dset, &space);

  for (int64_t offset = 0; offset < n_halos; offset += HORIZONTAL_HDF5_SCAN_BLOCK) {
    const int64_t remaining = n_halos - offset;
    const int64_t count =
        remaining < HORIZONTAL_HDF5_SCAN_BLOCK ? remaining : (int64_t)HORIZONTAL_HDF5_SCAN_BLOCK;
    horizontal_h5_read_block(dset, space, path, dataset_name, H5T_NATIVE_INT64, (hsize_t)offset,
                             (hsize_t)count, horizontal_h5_scan_i64);
    for (int64_t i = 0; i < count; i++) {
      const int64_t value = horizontal_h5_scan_i64[i];
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

  horizontal_h5_close_scan(path, dataset_name, dset, space);
  return measured;
}

/* ---------------------------------------------------------------------------
 * Version dispatch
 * ------------------------------------------------------------------------- */

/** @brief Why horizontal_h5_peek_format_version() could not read a `format_version`. */
enum horizontal_h5_peek_fault {
  HORIZONTAL_H5_PEEK_OK = 0,
  HORIZONTAL_H5_PEEK_HEADER_MISSING,
  HORIZONTAL_H5_PEEK_HEADER_SOFT_LINK,
  HORIZONTAL_H5_PEEK_HEADER_EXTERNAL_LINK,
  HORIZONTAL_H5_PEEK_HEADER_NOT_HARD_LINK, /* any other link type, or an unreadable link */
  HORIZONTAL_H5_PEEK_VERSION_MISSING,
  HORIZONTAL_H5_PEEK_VERSION_NOT_SCALAR,
  HORIZONTAL_H5_PEEK_VERSION_WRONG_TYPE,
  HORIZONTAL_H5_PEEK_VERSION_UNREADABLE
};

/** @brief Human-readable description of a peek fault, for the version 3 diagnostic. */
static const char *horizontal_h5_peek_fault_text(enum horizontal_h5_peek_fault fault) {
  switch (fault) {
  case HORIZONTAL_H5_PEEK_HEADER_MISSING:
    return "'/header' is missing";
  case HORIZONTAL_H5_PEEK_HEADER_SOFT_LINK:
    return "'/header' is a soft link";
  case HORIZONTAL_H5_PEEK_HEADER_EXTERNAL_LINK:
    return "'/header' is an external link";
  case HORIZONTAL_H5_PEEK_HEADER_NOT_HARD_LINK:
    return "'/header' is not a hard link";
  case HORIZONTAL_H5_PEEK_VERSION_MISSING:
    return "header attribute 'format_version' is missing";
  case HORIZONTAL_H5_PEEK_VERSION_NOT_SCALAR:
    return "header attribute 'format_version' is not a scalar";
  case HORIZONTAL_H5_PEEK_VERSION_WRONG_TYPE:
    return "header attribute 'format_version' is not an int32";
  case HORIZONTAL_H5_PEEK_VERSION_UNREADABLE:
    return "header attribute 'format_version' could not be read";
  case HORIZONTAL_H5_PEEK_OK:
    break;
  }
  return "no fault";
}

/**
 * @brief   Read a file's `format_version` before any per-version check runs.
 * @param   fault  Receives HORIZONTAL_H5_PEEK_OK, or the first check that failed.
 * @return  1 with *version set when `/header` is a hard-linked object carrying
 *          a scalar int32 `format_version`; 0 otherwise, with *fault naming why.
 *
 * Existence is tested before anything is opened, so a file without the
 * attribute prints no HDF5 error stack, and a soft or external `/header` is
 * never followed. A 0 return sends a version 2 file down its dataset's version
 * path, whose own checks then name exactly what is missing or malformed; the
 * caller uses *fault to give a version 3 file (one carrying `/schema`) the same
 * precision. Every handle opened here is closed before returning.
 */
static int horizontal_h5_peek_format_version(hid_t file, int32_t *version,
                                             enum horizontal_h5_peek_fault *fault) {
  *fault = HORIZONTAL_H5_PEEK_OK;
  if (H5Lexists(file, "header", H5P_DEFAULT) <= 0) {
    *fault = HORIZONTAL_H5_PEEK_HEADER_MISSING;
    return 0;
  }
  H5L_info_t link_info;
  if (H5Lget_info(file, "header", &link_info, H5P_DEFAULT) < 0) {
    *fault = HORIZONTAL_H5_PEEK_HEADER_NOT_HARD_LINK;
    return 0;
  }
  if (link_info.type != H5L_TYPE_HARD) {
    *fault = link_info.type == H5L_TYPE_SOFT       ? HORIZONTAL_H5_PEEK_HEADER_SOFT_LINK
             : link_info.type == H5L_TYPE_EXTERNAL ? HORIZONTAL_H5_PEEK_HEADER_EXTERNAL_LINK
                                                   : HORIZONTAL_H5_PEEK_HEADER_NOT_HARD_LINK;
    return 0;
  }
  if (H5Aexists_by_name(file, "header", "format_version", H5P_DEFAULT) <= 0) {
    *fault = HORIZONTAL_H5_PEEK_VERSION_MISSING;
    return 0;
  }

  hid_t attr = H5Aopen_by_name(file, "header", "format_version", H5P_DEFAULT, H5P_DEFAULT);
  if (attr < 0) {
    *fault = HORIZONTAL_H5_PEEK_VERSION_UNREADABLE;
    return 0;
  }
  hid_t space = H5Aget_space(attr);
  hid_t dtype = H5Aget_type(attr);
  if (space < 0 || dtype < 0) {
    *fault = HORIZONTAL_H5_PEEK_VERSION_UNREADABLE;
  } else if (H5Sget_simple_extent_type(space) != H5S_SCALAR) {
    *fault = HORIZONTAL_H5_PEEK_VERSION_NOT_SCALAR;
  } else if (!horizontal_h5_type_matches(dtype, HORIZONTAL_H5_I32)) {
    *fault = HORIZONTAL_H5_PEEK_VERSION_WRONG_TYPE;
  } else if (H5Aread(attr, H5T_NATIVE_INT32, version) < 0) {
    *fault = HORIZONTAL_H5_PEEK_VERSION_UNREADABLE;
  }
  if (dtype >= 0) {
    H5Tclose(dtype);
  }
  if (space >= 0) {
    H5Sclose(space);
  }
  H5Aclose(attr);
  return *fault == HORIZONTAL_H5_PEEK_OK;
}

/* ---------------------------------------------------------------------------
 * Version 3 structure
 * ------------------------------------------------------------------------- */

/**
 * @brief   Abort unless the link `name` under `group` is a hard link.
 *
 * Checked before the object is opened: opening a soft link resolves it, and
 * opening an external link opens another file, so the check must come first.
 * Version 3 requires every object to be physically present in the file that
 * names it.
 */
static void horizontal_h5_v3_require_hard_link(hid_t group, const char *path,
                                               const char *group_name, const char *name) {
  H5L_info_t link_info;
  if (H5Lget_info(group, name, &link_info, H5P_DEFAULT) < 0) {
    FATAL_ERROR("%s: could not read the link type of '%s%s%s'", path, group_name,
                strcmp(group_name, "/") == 0 ? "" : "/", name);
  }
  if (link_info.type != H5L_TYPE_HARD) {
    FATAL_ERROR("%s: '%s%s%s' is %s link; format_version %d requires every object to be "
                "physically present in the file that names it (no soft or external links)",
                path, group_name, strcmp(group_name, "/") == 0 ? "" : "/", name,
                link_info.type == H5L_TYPE_SOFT       ? "a soft"
                : link_info.type == H5L_TYPE_EXTERNAL ? "an external"
                                                      : "a user-defined",
                HORIZONTAL_HDF5_FORMAT_VERSION_V3);
  }
}

/** @brief Name of member `index` of an open group, into a caller buffer. */
static void horizontal_h5_member_name(hid_t group, const char *path, const char *group_name,
                                      hsize_t index, char *name, size_t name_size) {
  const ssize_t len = H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, index, name,
                                         name_size, H5P_DEFAULT);
  if (len < 0 || (size_t)len >= name_size) {
    FATAL_ERROR("%s: could not read the name of '%s' member %" PRIu64, path, group_name,
                (uint64_t)index);
  }
}

/** @brief Number of links in an open group. */
static hsize_t horizontal_h5_member_count(hid_t group, const char *path, const char *group_name) {
  H5G_info_t ginfo;
  if (H5Gget_info(group, &ginfo) < 0) {
    FATAL_ERROR("%s: could not read the link count of '%s'", path, group_name);
  }
  return ginfo.nlinks;
}

/**
 * @brief   Validate the version 3 root object set: exactly the hard-linked
 *          groups /header, /halos and /schema.
 */
static void horizontal_h5_v3_validate_object_set(hid_t file, const char *path) {
  static const char *const expected[] = {"halos", "header", "schema"};
  const size_t expected_count = sizeof(expected) / sizeof(expected[0]);

  hid_t root = H5Gopen2(file, "/", H5P_DEFAULT);
  if (root < 0) {
    FATAL_ERROR("%s: could not open the root group", path);
  }

  const hsize_t nlinks = horizontal_h5_member_count(root, path, "/");
  for (hsize_t i = 0; i < nlinks; i++) {
    char name[MAX_STRING_LEN];
    horizontal_h5_member_name(root, path, "/", i, name, sizeof(name));
    int known = 0;
    for (size_t e = 0; e < expected_count; e++) {
      if (strcmp(expected[e], name) == 0) {
        known = 1;
        break;
      }
    }
    if (!known) {
      FATAL_ERROR("%s: unexpected root object '%s'; format_version %d defines exactly the groups "
                  "'/header', '/halos' and '/schema'",
                  path, name, HORIZONTAL_HDF5_FORMAT_VERSION_V3);
    }
    horizontal_h5_v3_require_hard_link(root, path, "/", name);
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

/** @brief H5Aiterate2 callback rejecting any attribute version 3 omits. */
static herr_t horizontal_h5_v3_reject_unknown_attr(hid_t location_id, const char *attr_name,
                                                   const H5A_info_t *ainfo, void *op_data) {
  const struct horizontal_h5_attr_scan *scan = (const struct horizontal_h5_attr_scan *)op_data;
  (void)location_id;
  (void)ainfo;

  for (size_t i = 0; i < HORIZONTAL_H5_HEADER_ATTR_COUNT; i++) {
    if (strcmp(HORIZONTAL_H5_HEADER_ATTRS[i].name, attr_name) == 0) {
      return 0;
    }
  }
  for (size_t i = 0; i < HORIZONTAL_H5_V3_STRING_ATTR_COUNT; i++) {
    if (strcmp(HORIZONTAL_H5_V3_STRING_ATTRS[i].name, attr_name) == 0) {
      return 0;
    }
  }
  FATAL_ERROR("%s: '/header' carries unexpected attribute '%s'; format_version %d defines exactly "
              "%zu header attributes",
              scan->path, attr_name, HORIZONTAL_HDF5_FORMAT_VERSION_V3,
              (size_t)(HORIZONTAL_H5_HEADER_ATTR_COUNT + HORIZONTAL_H5_V3_STRING_ATTR_COUNT));
}

/**
 * @brief   Read one fixed-length ASCII header string of exactly `spec->size`
 *          bytes, validating rank, class, length, padding and character set.
 * @param   dst  Receives the value, NUL-terminated; spec->size + 1 bytes.
 *
 * The on-disk value is null-padded: its text ends at the first NUL, and every
 * byte after that must also be NUL, so two files whose visible text agrees
 * cannot hide different trailing bytes.
 */
static void horizontal_h5_v3_read_string_attr(hid_t file, const char *path,
                                              const struct horizontal_h5_string_attr_spec *spec,
                                              char *dst) {
  hid_t attr = H5Aopen_by_name(file, "/header", spec->name, H5P_DEFAULT, H5P_DEFAULT);
  if (attr < 0) {
    FATAL_ERROR("%s: required header attribute '%s' is missing", path, spec->name);
  }
  hid_t space = H5Aget_space(attr);
  if (space < 0 || H5Sget_simple_extent_type(space) != H5S_SCALAR) {
    FATAL_ERROR("%s: header attribute '%s' must be a scalar", path, spec->name);
  }
  H5Sclose(space);

  hid_t dtype = H5Aget_type(attr);
  if (dtype < 0) {
    FATAL_ERROR("%s: could not read the datatype of header attribute '%s'", path, spec->name);
  }
  if (H5Tget_class(dtype) != H5T_STRING || H5Tis_variable_str(dtype) > 0 ||
      H5Tget_size(dtype) != spec->size || H5Tget_cset(dtype) != H5T_CSET_ASCII) {
    FATAL_ERROR("%s: header attribute '%s' must be a fixed-length ASCII string of exactly %zu "
                "bytes; found HDF5 type class %d of %zu bytes",
                path, spec->name, spec->size, (int)H5Tget_class(dtype), H5Tget_size(dtype));
  }

  hid_t mem_type = H5Tcopy(H5T_C_S1);
  if (mem_type < 0 || H5Tset_size(mem_type, spec->size) < 0 ||
      H5Tset_strpad(mem_type, H5T_STR_NULLPAD) < 0) {
    FATAL_ERROR("%s: could not build a read type for header attribute '%s'", path, spec->name);
  }
  memset(dst, 0, spec->size + 1);
  if (H5Aread(attr, mem_type, dst) < 0) {
    FATAL_ERROR("%s: could not read header attribute '%s'", path, spec->name);
  }
  H5Tclose(mem_type);
  if (H5Tclose(dtype) < 0 || H5Aclose(attr) < 0) {
    FATAL_ERROR("%s: could not close header attribute '%s'", path, spec->name);
  }

  const size_t text_len = strlen(dst);
  for (size_t i = text_len; i < spec->size; i++) {
    if (dst[i] != '\0') {
      FATAL_ERROR("%s: header attribute '%s' has a non-NUL byte after its NUL terminator; it must "
                  "be null-padded",
                  path, spec->name);
    }
  }
  for (size_t i = 0; i < text_len; i++) {
    const unsigned char c = (unsigned char)dst[i];
    if (c < 0x20 || c > 0x7e) {
      FATAL_ERROR("%s: header attribute '%s' contains a non-printable byte 0x%02x at offset %zu",
                  path, spec->name, (unsigned)c, i);
    }
  }
}

/**
 * @brief   Validate the version 3 header attribute set and read every
 *          attribute: the twelve version 2 attributes, then the two strings.
 *
 * The two strings are also checked for form here: `source_format` names a
 * known adapter, and `column_mapping_sha256` is 64 lowercase hex digits.
 */
static void horizontal_h5_v3_read_header(hid_t file, const char *path,
                                         struct horizontal_h5_header *header,
                                         struct horizontal_h5_v3_strings *strings) {
  hid_t group = H5Gopen2(file, "/header", H5P_DEFAULT);
  if (group < 0) {
    FATAL_ERROR("%s: could not open group '/header'", path);
  }
  struct horizontal_h5_attr_scan scan = {path};
  hsize_t idx = 0;
  if (H5Aiterate2(group, H5_INDEX_NAME, H5_ITER_INC, &idx, horizontal_h5_v3_reject_unknown_attr,
                  &scan) < 0) {
    FATAL_ERROR("%s: could not enumerate the attributes of '/header'", path);
  }
  if (H5Gclose(group) < 0) {
    FATAL_ERROR("%s: could not close group '/header'", path);
  }

  for (size_t i = 0; i < HORIZONTAL_H5_HEADER_ATTR_COUNT; i++) {
    horizontal_h5_read_header_attr(file, path, &HORIZONTAL_H5_HEADER_ATTRS[i],
                                   (char *)header + HORIZONTAL_H5_HEADER_ATTRS[i].offset);
  }
  horizontal_h5_v3_read_string_attr(file, path, &HORIZONTAL_H5_V3_STRING_ATTRS[0],
                                    strings->source_format);
  horizontal_h5_v3_read_string_attr(file, path, &HORIZONTAL_H5_V3_STRING_ATTRS[1],
                                    strings->column_mapping_sha256);

  int known_format = 0;
  for (size_t i = 0; i < HORIZONTAL_H5_V3_SOURCE_FORMAT_COUNT; i++) {
    if (strcmp(HORIZONTAL_H5_V3_SOURCE_FORMATS[i], strings->source_format) == 0) {
      known_format = 1;
      break;
    }
  }
  if (!known_format) {
    FATAL_ERROR("%s: header attribute 'source_format' is '%s'; format_version %d names exactly "
                "'consistent_trees_ascii', 'consistent_trees_hdf5' or 'lhalo_binary'",
                path, strings->source_format, HORIZONTAL_HDF5_FORMAT_VERSION_V3);
  }

  const char *digest = strings->column_mapping_sha256;
  int digest_ok = strlen(digest) == HORIZONTAL_H5_V3_MAPPING_DIGEST_LEN;
  for (size_t i = 0; digest_ok && i < HORIZONTAL_H5_V3_MAPPING_DIGEST_LEN; i++) {
    digest_ok = (digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f');
  }
  if (!digest_ok) {
    FATAL_ERROR("%s: header attribute 'column_mapping_sha256' is '%s'; it must be exactly %d "
                "lowercase hexadecimal digits",
                path, digest, HORIZONTAL_H5_V3_MAPPING_DIGEST_LEN);
  }
}

/** @brief mymalloc-owned copy of a NUL-terminated string. */
static char *horizontal_h5_strdup(const char *text) {
  const size_t len = strlen(text);
  char *copy = mymalloc_cat(len + 1, MEM_TREES);
  memcpy(copy, text, len + 1);
  return copy;
}

/**
 * @brief   Read one `/schema` subgroup attribute: a scalar variable-length
 *          UTF-8 string.
 * @return  A mymalloc-owned copy of the value.
 */
static char *horizontal_h5_v3_read_schema_attr(hid_t group, const char *path, const char *field,
                                               const char *attr_name) {
  if (H5Aexists(group, attr_name) <= 0) {
    FATAL_ERROR("%s: '/schema/%s' is missing its required attribute '%s'", path, field, attr_name);
  }
  hid_t attr = H5Aopen(group, attr_name, H5P_DEFAULT);
  if (attr < 0) {
    FATAL_ERROR("%s: could not open attribute '%s' of '/schema/%s'", path, attr_name, field);
  }
  hid_t space = H5Aget_space(attr);
  if (space < 0 || H5Sget_simple_extent_type(space) != H5S_SCALAR) {
    FATAL_ERROR("%s: attribute '%s' of '/schema/%s' must be a scalar", path, attr_name, field);
  }
  H5Sclose(space);

  hid_t dtype = H5Aget_type(attr);
  if (dtype < 0 || H5Tget_class(dtype) != H5T_STRING || H5Tis_variable_str(dtype) <= 0 ||
      H5Tget_cset(dtype) != H5T_CSET_UTF8) {
    FATAL_ERROR("%s: attribute '%s' of '/schema/%s' must be a variable-length UTF-8 string", path,
                attr_name, field);
  }

  hid_t mem_type = H5Tcopy(H5T_C_S1);
  if (mem_type < 0 || H5Tset_size(mem_type, H5T_VARIABLE) < 0 ||
      H5Tset_cset(mem_type, H5T_CSET_UTF8) < 0) {
    FATAL_ERROR("%s: could not build a read type for attribute '%s' of '/schema/%s'", path,
                attr_name, field);
  }
  char *value = NULL;
  if (H5Aread(attr, mem_type, &value) < 0 || value == NULL) {
    FATAL_ERROR("%s: could not read attribute '%s' of '/schema/%s'", path, attr_name, field);
  }
  char *copy = horizontal_h5_strdup(value);
  H5free_memory(value);
  H5Tclose(mem_type);
  if (H5Tclose(dtype) < 0 || H5Aclose(attr) < 0) {
    FATAL_ERROR("%s: could not close attribute '%s' of '/schema/%s'", path, attr_name, field);
  }
  return copy;
}

/** @brief H5Aiterate2 callback counting an object's attributes into a uint64_t. */
static herr_t horizontal_h5_count_attr(hid_t location_id, const char *attr_name,
                                       const H5A_info_t *ainfo, void *op_data) {
  (void)location_id;
  (void)attr_name;
  (void)ainfo;
  (*(uint64_t *)op_data)++;
  return 0;
}

/** @brief `/schema` storage spec for a `type` value, or NULL if outside the vocabulary. */
static const struct horizontal_h5_schema_type_spec *horizontal_h5_v3_schema_type(const char *name) {
  for (size_t i = 0; i < HORIZONTAL_H5_V3_SCHEMA_TYPE_COUNT; i++) {
    if (strcmp(HORIZONTAL_H5_V3_SCHEMA_TYPES[i].name, name) == 0) {
      return &HORIZONTAL_H5_V3_SCHEMA_TYPES[i];
    }
  }
  return NULL;
}

/** @brief Version 3 fixed-table entry by dataset name, or NULL if not fixed. */
static const struct horizontal_h5_dataset_spec *horizontal_h5_v3_fixed_spec(const char *name) {
  for (size_t i = 0; i < HORIZONTAL_H5_V3_FIXED_DATASET_COUNT; i++) {
    if (strcmp(HORIZONTAL_H5_V3_FIXED_DATASETS[i].name, name) == 0) {
      return &HORIZONTAL_H5_V3_FIXED_DATASETS[i];
    }
  }
  return NULL;
}

/** @brief Parsed `/schema` entry by field name, or NULL if not declared. */
static const struct horizontal_h5_schema_entry *
horizontal_h5_v3_schema_find(const struct horizontal_h5_schema *schema, const char *name) {
  for (size_t i = 0; i < schema->count; i++) {
    if (strcmp(schema->entries[i].name, name) == 0) {
      return &schema->entries[i];
    }
  }
  return NULL;
}

/** @brief Release every string and the entry array of a parsed `/schema`. */
static void horizontal_h5_v3_free_schema(struct horizontal_h5_schema *schema) {
  for (size_t i = 0; i < schema->count; i++) {
    for (int a = 0; a < HORIZONTAL_H5_SCHEMA_ATTR_COUNT; a++) {
      myfree(schema->entries[i].attrs[a]);
    }
    myfree(schema->entries[i].name);
  }
  if (schema->entries != NULL) {
    myfree(schema->entries);
  }
  schema->entries = NULL;
  schema->count = 0;
}

/**
 * @brief   Parse and structurally validate one file's `/schema`.
 *
 * Every member must be a hard-linked, childless group carrying exactly the
 * four string attributes, with `type` and `h_convention` inside the property
 * generator's vocabularies. No topology or identity field may be declared, and
 * every payload field the format table names must be, with the type or shape
 * the table fixes. Whether each declaration agrees with its dataset is checked
 * separately, by horizontal_h5_v3_validate_halo_datasets().
 */
static void horizontal_h5_v3_read_schema(hid_t file, const char *path,
                                         struct horizontal_h5_schema *schema) {
  hid_t group = H5Gopen2(file, "/schema", H5P_DEFAULT);
  if (group < 0) {
    FATAL_ERROR("%s: could not open group '/schema'", path);
  }

  const hsize_t nlinks = horizontal_h5_member_count(group, path, "/schema");
  schema->count = 0;
  schema->entries =
      nlinks > 0
          ? mymalloc_cat(sizeof(struct horizontal_h5_schema_entry) * (size_t)nlinks, MEM_TREES)
          : NULL;

  for (hsize_t i = 0; i < nlinks; i++) {
    char name[MAX_STRING_LEN];
    horizontal_h5_member_name(group, path, "/schema", i, name, sizeof(name));
    horizontal_h5_v3_require_hard_link(group, path, "/schema", name);

    if (horizontal_h5_v3_fixed_spec(name) != NULL) {
      FATAL_ERROR("%s: '/schema/%s' declares a topology or identity field; format_version %d "
                  "governs those by its fixed format table and never redeclares them",
                  path, name, HORIZONTAL_HDF5_FORMAT_VERSION_V3);
    }

    /* Group-ness and the attribute count use only API spellings present in
       HDF5 1.10 as well as 1.12+ (H5O_info2_t and H5Oget_info_by_name3 are
       1.12+ only): open the object generically, ask its identifier type, and
       count attributes by iteration. */
    hid_t member = H5Oopen(group, name, H5P_DEFAULT);
    if (member < 0) {
      FATAL_ERROR("%s: could not open '/schema/%s'", path, name);
    }
    if (H5Iget_type(member) != H5I_GROUP) {
      FATAL_ERROR("%s: '/schema/%s' must be a group", path, name);
    }
    hsize_t attr_idx = 0;
    uint64_t num_attrs = 0;
    if (H5Aiterate2(member, H5_INDEX_NAME, H5_ITER_INC, &attr_idx, horizontal_h5_count_attr,
                    &num_attrs) < 0) {
      FATAL_ERROR("%s: could not enumerate the attributes of '/schema/%s'", path, name);
    }
    if (num_attrs != HORIZONTAL_H5_SCHEMA_ATTR_COUNT) {
      FATAL_ERROR("%s: '/schema/%s' carries %" PRIu64 " attributes; format_version %d requires "
                  "exactly 'type', 'units', 'h_convention' and 'description'",
                  path, name, num_attrs, HORIZONTAL_HDF5_FORMAT_VERSION_V3);
    }
    if (horizontal_h5_member_count(member, path, name) != 0) {
      FATAL_ERROR("%s: '/schema/%s' has children; a schema declaration has none", path, name);
    }

    struct horizontal_h5_schema_entry *entry = &schema->entries[schema->count];
    entry->name = horizontal_h5_strdup(name);
    for (int a = 0; a < HORIZONTAL_H5_SCHEMA_ATTR_COUNT; a++) {
      entry->attrs[a] =
          horizontal_h5_v3_read_schema_attr(member, path, name, HORIZONTAL_H5_V3_SCHEMA_ATTRS[a]);
    }
    schema->count++;
    if (H5Oclose(member) < 0) {
      FATAL_ERROR("%s: could not close group '/schema/%s'", path, name);
    }

    entry->storage = horizontal_h5_v3_schema_type(entry->attrs[HORIZONTAL_H5_SCHEMA_TYPE]);
    if (entry->storage == NULL) {
      FATAL_ERROR("%s: '/schema/%s' declares type '%s'; the type vocabulary is 'int', 'long "
                  "long', 'float', 'double', 'vec3_int' and 'vec3_float'",
                  path, name, entry->attrs[HORIZONTAL_H5_SCHEMA_TYPE]);
    }
    int known_h = 0;
    for (size_t h = 0; h < HORIZONTAL_H5_V3_H_CONVENTION_COUNT; h++) {
      if (strcmp(HORIZONTAL_H5_V3_H_CONVENTIONS[h],
                 entry->attrs[HORIZONTAL_H5_SCHEMA_H_CONVENTION]) == 0) {
        known_h = 1;
        break;
      }
    }
    if (!known_h) {
      FATAL_ERROR("%s: '/schema/%s' declares h_convention '%s'; the vocabulary is 'carried', "
                  "'free' and 'none'",
                  path, name, entry->attrs[HORIZONTAL_H5_SCHEMA_H_CONVENTION]);
    }
  }

  if (H5Gclose(group) < 0) {
    FATAL_ERROR("%s: could not close group '/schema'", path);
  }

  for (size_t p = 0; p < HORIZONTAL_H5_V3_PAYLOAD_COUNT; p++) {
    const struct horizontal_h5_payload_spec *payload = &HORIZONTAL_H5_V3_PAYLOAD[p];
    const struct horizontal_h5_schema_entry *entry =
        horizontal_h5_v3_schema_find(schema, payload->name);
    if (entry == NULL) {
      FATAL_ERROR("%s: '/schema' does not declare payload field '%s', which the format_version "
                  "%d payload table requires",
                  path, payload->name, HORIZONTAL_HDF5_FORMAT_VERSION_V3);
    }
    if (payload->fixed_type != NULL &&
        strcmp(entry->attrs[HORIZONTAL_H5_SCHEMA_TYPE], payload->fixed_type) != 0) {
      FATAL_ERROR("%s: '/schema/%s' declares type '%s'; format_version %d fixes it as '%s'", path,
                  payload->name, entry->attrs[HORIZONTAL_H5_SCHEMA_TYPE],
                  HORIZONTAL_HDF5_FORMAT_VERSION_V3, payload->fixed_type);
    }
    if (entry->storage->ncols != payload->ncols) {
      FATAL_ERROR("%s: '/schema/%s' declares type '%s'; format_version %d requires a %s field",
                  path, payload->name, entry->attrs[HORIZONTAL_H5_SCHEMA_TYPE],
                  HORIZONTAL_HDF5_FORMAT_VERSION_V3,
                  payload->ncols == 0 ? "scalar [n_halos]" : "vector [n_halos, 3]");
    }
  }
}

/**
 * @brief   Abort unless a file's `/schema` equals snapshot 0's exactly: the
 *          same fields, each with the same four attribute values.
 */
static void horizontal_h5_v3_require_same_schema(const struct horizontal_h5_schema *reference,
                                                 const struct horizontal_h5_schema *schema,
                                                 const char *path) {
  for (size_t i = 0; i < schema->count; i++) {
    const struct horizontal_h5_schema_entry *entry = &schema->entries[i];
    const struct horizontal_h5_schema_entry *ref =
        horizontal_h5_v3_schema_find(reference, entry->name);
    if (ref == NULL) {
      FATAL_ERROR("%s: '/schema' declares '%s', which snapshot 0's '/schema' does not; '/schema' "
                  "must be identical in every file",
                  path, entry->name);
    }
    for (int a = 0; a < HORIZONTAL_H5_SCHEMA_ATTR_COUNT; a++) {
      if (strcmp(ref->attrs[a], entry->attrs[a]) != 0) {
        FATAL_ERROR("%s: '/schema/%s' attribute '%s' is '%s' but snapshot 0 declares '%s'; "
                    "'/schema' must be identical in every file",
                    path, entry->name, HORIZONTAL_H5_V3_SCHEMA_ATTRS[a], entry->attrs[a],
                    ref->attrs[a]);
      }
    }
  }
  for (size_t i = 0; i < reference->count; i++) {
    if (horizontal_h5_v3_schema_find(schema, reference->entries[i].name) == NULL) {
      FATAL_ERROR("%s: '/schema' does not declare '%s', which snapshot 0's '/schema' does; "
                  "'/schema' must be identical in every file",
                  path, reference->entries[i].name);
    }
  }
}

/**
 * @brief   Validate one /halos dataset's dtype, rank and shape.
 *
 * The version 3 counterpart of the per-dataset body of
 * horizontal_h5_validate_halo_datasets(), which version 2 keeps unchanged.
 */
static void horizontal_h5_v3_validate_dataset(hid_t group, const char *path, const char *name,
                                              enum horizontal_h5_scalar_type type, int ncols,
                                              int64_t n_halos, const char *declared_by) {
  hid_t dset = H5Dopen2(group, name, H5P_DEFAULT);
  if (dset < 0) {
    FATAL_ERROR("%s: '/halos/%s' is not a dataset", path, name);
  }

  hid_t dtype = H5Dget_type(dset);
  if (dtype < 0) {
    FATAL_ERROR("%s: could not read the datatype of '/halos/%s'", path, name);
  }
  if (!horizontal_h5_type_matches(dtype, type)) {
    FATAL_ERROR("%s: dataset '/halos/%s' must be %s on disk (%s); found HDF5 type class %d of %zu "
                "bytes",
                path, name, horizontal_h5_type_name(type), declared_by, (int)H5Tget_class(dtype),
                H5Tget_size(dtype));
  }
  if (H5Tclose(dtype) < 0) {
    FATAL_ERROR("%s: could not close the datatype of '/halos/%s'", path, name);
  }

  hid_t space = H5Dget_space(dset);
  if (space < 0) {
    FATAL_ERROR("%s: could not read the dataspace of '/halos/%s'", path, name);
  }
  const int expected_rank = ncols == 0 ? 1 : 2;
  const int rank = H5Sget_simple_extent_ndims(space);
  if (rank != expected_rank) {
    FATAL_ERROR("%s: dataset '/halos/%s' must have rank %d (%s); found rank %d", path, name,
                expected_rank, declared_by, rank);
  }
  hsize_t dims[2] = {0, 0};
  if (H5Sget_simple_extent_dims(space, dims, NULL) != expected_rank) {
    FATAL_ERROR("%s: could not read the extent of '/halos/%s'", path, name);
  }
  /* Compared in the wide type, never narrowed: see the version 2 check. */
  if (dims[0] != (hsize_t)n_halos) {
    FATAL_ERROR("%s: dataset '/halos/%s' has length %" PRIu64 " but header n_halos is %" PRId64,
                path, name, (uint64_t)dims[0], n_halos);
  }
  if (expected_rank == 2 && dims[1] != (hsize_t)ncols) {
    FATAL_ERROR("%s: dataset '/halos/%s' must have shape [%" PRId64
                ", %d] (%s); found second dimension %" PRIu64,
                path, name, n_halos, ncols, declared_by, (uint64_t)dims[1]);
  }
  if (H5Sclose(space) < 0 || H5Dclose(dset) < 0) {
    FATAL_ERROR("%s: could not close dataset '/halos/%s'", path, name);
  }
}

/**
 * @brief   Validate the version 3 /halos set: exactly the fixed topology and
 *          identity table plus one dataset per `/schema` declaration, every
 *          member hard-linked, each with the dtype and shape its table entry
 *          or its own declared `type` fixes.
 *
 * Runs over every declaration, whether or not the package declares the field:
 * an undeclared extra is still validated against its own declaration, never
 * silently trusted (Gate R0-8(a)).
 */
static void horizontal_h5_v3_validate_halo_datasets(hid_t file, const char *path, int64_t n_halos,
                                                    const struct horizontal_h5_schema *schema) {
  hid_t group = H5Gopen2(file, "/halos", H5P_DEFAULT);
  if (group < 0) {
    FATAL_ERROR("%s: could not open group '/halos'", path);
  }

  const hsize_t nlinks = horizontal_h5_member_count(group, path, "/halos");
  for (hsize_t i = 0; i < nlinks; i++) {
    char name[MAX_STRING_LEN];
    horizontal_h5_member_name(group, path, "/halos", i, name, sizeof(name));
    if (horizontal_h5_v3_fixed_spec(name) == NULL &&
        horizontal_h5_v3_schema_find(schema, name) == NULL) {
      FATAL_ERROR("%s: unexpected dataset '/halos/%s'; format_version %d permits only the fixed "
                  "topology and identity datasets and the fields '/schema' declares",
                  path, name, HORIZONTAL_HDF5_FORMAT_VERSION_V3);
    }
    horizontal_h5_v3_require_hard_link(group, path, "/halos", name);
  }

  for (size_t s = 0; s < HORIZONTAL_H5_V3_FIXED_DATASET_COUNT; s++) {
    const struct horizontal_h5_dataset_spec *spec = &HORIZONTAL_H5_V3_FIXED_DATASETS[s];
    if (H5Lexists(group, spec->name, H5P_DEFAULT) <= 0) {
      FATAL_ERROR("%s: required dataset '/halos/%s' is missing", path, spec->name);
    }
    horizontal_h5_v3_validate_dataset(group, path, spec->name, spec->type, spec->ncols, n_halos,
                                      "fixed by the format table");
  }

  for (size_t i = 0; i < schema->count; i++) {
    const struct horizontal_h5_schema_entry *entry = &schema->entries[i];
    if (H5Lexists(group, entry->name, H5P_DEFAULT) <= 0) {
      FATAL_ERROR("%s: '/schema' declares '%s' but dataset '/halos/%s' is missing", path,
                  entry->name, entry->name);
    }
    char declared_by[MAX_STRING_LEN];
    snprintf(declared_by, sizeof(declared_by), "'/schema/%s' declares type '%s'", entry->name,
             entry->attrs[HORIZONTAL_H5_SCHEMA_TYPE]);
    horizontal_h5_v3_validate_dataset(group, path, entry->name, entry->storage->type,
                                      entry->storage->ncols, n_halos, declared_by);
  }

  if (H5Gclose(group) < 0) {
    FATAL_ERROR("%s: could not close group '/halos'", path);
  }
}

/**
 * @brief   Compare the compiled package's declarations with a file's `/schema`.
 *
 * Every catalog field the package declares -- each core payload field it
 * consumes and each extra -- must be declared in `/schema` with the same
 * `type`, `units` and `h_convention`, so a wrong-by-10^10 mass is a startup
 * failure rather than a silent result. Topology and identity fields are never
 * in `/schema`; a package field bound to one of them is checked against the
 * format's fixed table instead, which is where Gate R0-2(a)'s `long long` link
 * storage is enforced: an `int` link would narrow version 3's int64 indices.
 *
 * `/schema` fields the package does not declare are not consulted here; they
 * are validated for internal consistency by the structural checks and never
 * materialised (Gate R0-8(a)).
 */
static void horizontal_h5_v3_validate_package(const struct horizontal_h5_schema *schema,
                                              const char *path) {
  for (size_t f = 0; f < HORIZONTAL_H5_CATALOG_FIELD_COUNT; f++) {
    const struct horizontal_h5_catalog_field *field = &HORIZONTAL_H5_CATALOG_FIELDS[f];
    const struct horizontal_h5_dataset_spec *fixed = horizontal_h5_v3_fixed_spec(field->dataset);

    if (strcmp(field->role_kind, "tree_link") == 0 &&
        (fixed == NULL || strcmp(field->dataset, field->core_role) != 0)) {
      FATAL_ERROR("%s: simulation package '%s' provides link role '%s' from dataset '/halos/%s'; "
                  "format_version %d stores that link only in '/halos/%s'",
                  path, MIMIC_COMPILED_SIMULATION, field->core_role, field->dataset,
                  HORIZONTAL_HDF5_FORMAT_VERSION_V3, field->core_role);
    }

    if (fixed != NULL) {
      const char *fixed_type = fixed->type == HORIZONTAL_H5_I64 ? "long long" : "int";
      if (strcmp(field->type, fixed_type) != 0) {
        FATAL_ERROR(
            "%s: simulation package '%s' declares '%s' (dataset '/halos/%s') as '%s', but "
            "format_version %d's fixed table stores it as %s ('%s'); reading it into "
            "that storage would %s",
            path, MIMIC_COMPILED_SIMULATION, field->member, field->dataset, field->type,
            HORIZONTAL_HDF5_FORMAT_VERSION_V3, horizontal_h5_type_name(fixed->type), fixed_type,
            field->member_size < (fixed->type == HORIZONTAL_H5_I64 ? 8u : 4u) ? "narrow it"
                                                                              : "misread it");
      }
      continue;
    }

    const struct horizontal_h5_schema_entry *entry =
        horizontal_h5_v3_schema_find(schema, field->dataset);
    if (entry == NULL) {
      FATAL_ERROR("%s: simulation package '%s' declares '%s' as dataset '/halos/%s', which "
                  "'/schema' does not declare; the package's halo_properties.yaml does not match "
                  "this dataset",
                  path, MIMIC_COMPILED_SIMULATION, field->member, field->dataset);
    }
    const char *const compiled[HORIZONTAL_H5_SCHEMA_ATTR_COUNT - 1] = {field->type, field->units,
                                                                       field->h_convention};
    for (int a = 0; a < HORIZONTAL_H5_SCHEMA_DESCRIPTION; a++) {
      if (strcmp(compiled[a], entry->attrs[a]) != 0) {
        FATAL_ERROR("%s: '/schema/%s' declares %s '%s' but simulation package '%s' declares '%s' "
                    "as '%s'; the package must declare exactly what the file declares",
                    path, field->dataset, HORIZONTAL_H5_V3_SCHEMA_ATTRS[a], entry->attrs[a],
                    MIMIC_COMPILED_SIMULATION, field->member, compiled[a]);
      }
    }
  }
}

/* ---------------------------------------------------------------------------
 * Slab loading
 * ------------------------------------------------------------------------- */

/**
 * @brief   Read one whole validated /halos dataset into buf.
 *
 * Extents are carried in hsize_t widened from int64_t; nothing on this path is
 * narrowed to int. The file dataspace is used whole (H5S_ALL) because
 * horizontal_h5_validate_halo_datasets() has already proven it is exactly
 * [n_halos] or [n_halos, ncols].
 *
 * The memory type is the native type for the destination field, never the
 * on-disk type: HDF5 performs the byte-order and width conversion, so a
 * conforming file written on the other endianness reads correctly.
 */
static void horizontal_h5_read_column(hid_t file, const char *path, const char *dataset_name,
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
#define HORIZONTAL_H5_MEMTYPE_READ_AS_INT H5T_NATIVE_INT
#define HORIZONTAL_H5_MEMTYPE_READ_AS_FLOAT H5T_NATIVE_FLOAT
#define HORIZONTAL_H5_MEMTYPE_READ_AS_LLONG H5T_NATIVE_LLONG
#define HORIZONTAL_H5_MEMTYPE(read_as) HORIZONTAL_H5_MEMTYPE_##read_as

/* Widest native element the property list can request, and so the per-element
   stride of the staging buffer below. */
#define HORIZONTAL_H5_MAX_ELEMENT_SIZE 8

/* Fixed-size staging buffer for slab filling: one hyperslab block of the widest
   element over NDIM columns. Never resized, so filling a slab allocates nothing
   beyond the slab itself, whatever the snapshot's size. */
static unsigned char
    horizontal_h5_fill_stage[HORIZONTAL_HDF5_SCAN_BLOCK * NDIM * HORIZONTAL_H5_MAX_ELEMENT_SIZE];

/**
 * @brief   Fill one struct RawHalo member of every slab halo from a validated
 *          /halos dataset, block by block.
 * @param   element_size   Bytes per element of the member (and of mem_type).
 * @param   ncols          1 for a scalar member, NDIM for a vector member.
 * @param   member_offset  offsetof(struct RawHalo, member).
 *
 * Each block of HORIZONTAL_HDF5_SCAN_BLOCK rows is read through the native
 * memory type into the fixed staging buffer, then copied into the member by
 * value with memcpy -- never through a cast of the buffer pointer to the
 * member's type, which the vertical reader does and which is an aliasing
 * violation cppcheck flags.
 */
static void horizontal_h5_fill_member(hid_t file, const char *path, const char *dataset_name,
                                      hid_t mem_type, size_t element_size, int ncols,
                                      int64_t n_halos, struct RawHalo *halos,
                                      size_t member_offset) {
  if (H5Tget_size(mem_type) != element_size || element_size > HORIZONTAL_H5_MAX_ELEMENT_SIZE ||
      ncols < 1 || ncols > NDIM) {
    FATAL_ERROR("%s: internal error filling '/halos/%s': memory type of %zu bytes for a %zu-byte "
                "member of %d column(s)",
                path, dataset_name, H5Tget_size(mem_type), element_size, ncols);
  }

  hid_t dset, space;
  horizontal_h5_open_scan(file, path, dataset_name, &dset, &space);
  const int rank = ncols > 1 ? 2 : 1;

  for (int64_t offset = 0; offset < n_halos; offset += HORIZONTAL_HDF5_SCAN_BLOCK) {
    const int64_t remaining = n_halos - offset;
    const int64_t count =
        remaining < HORIZONTAL_HDF5_SCAN_BLOCK ? remaining : (int64_t)HORIZONTAL_HDF5_SCAN_BLOCK;
    const hsize_t start[2] = {(hsize_t)offset, 0};
    const hsize_t block[2] = {(hsize_t)count, (hsize_t)ncols};

    if (H5Sselect_hyperslab(space, H5S_SELECT_SET, start, NULL, block, NULL) < 0) {
      FATAL_ERROR("%s: could not select halos [%" PRId64 ", %" PRId64 ") of '/halos/%s'", path,
                  offset, offset + count, dataset_name);
    }
    hid_t memspace = H5Screate_simple(rank, block, NULL);
    if (memspace < 0) {
      FATAL_ERROR("%s: could not create a read buffer dataspace for '/halos/%s'", path,
                  dataset_name);
    }
    if (H5Dread(dset, mem_type, memspace, space, H5P_DEFAULT, horizontal_h5_fill_stage) < 0) {
      FATAL_ERROR("%s: could not read halos [%" PRId64 ", %" PRId64 ") of '/halos/%s'", path,
                  offset, offset + count, dataset_name);
    }
    if (H5Sclose(memspace) < 0) {
      FATAL_ERROR("%s: could not close the read buffer dataspace for '/halos/%s'", path,
                  dataset_name);
    }

    for (int64_t i = 0; i < count; i++) {
      char *member = (char *)&halos[offset + i] + member_offset;
      for (int d = 0; d < ncols; d++) {
        memcpy(member + (size_t)d * element_size,
               horizontal_h5_fill_stage + ((size_t)i * (size_t)ncols + (size_t)d) * element_size,
               element_size);
      }
    }
  }

  horizontal_h5_close_scan(path, dataset_name, dset, space);
}

/* Horizontal-flavoured counterparts of the vertical reader's macros
   (src/io/vertical/hdf5.c), used to include the same generated property
   list. The differences are the dataset path ("/halos/<name>" rather than
   "tree_NNN/<name>"), the destination array (the slab rather than
   InputTreeHalos), int64_t counts, and bounded block reads copied out by value
   (horizontal_h5_fill_member()). */
#define READ_TREE_PROPERTY(field_name, hdf5_name, type_int, data_type)                             \
  horizontal_h5_fill_member(file, path, hdf5_name, HORIZONTAL_H5_MEMTYPE(type_int),                \
                            sizeof(data_type), 1, n_halos, halos,                                  \
                            offsetof(struct RawHalo, field_name))

#define READ_TREE_PROPERTY_MULTIPLEDIM(field_name, hdf5_name, type_int, data_type)                 \
  horizontal_h5_fill_member(file, path, hdf5_name, HORIZONTAL_H5_MEMTYPE(type_int),                \
                            sizeof(data_type), NDIM, n_halos, halos,                               \
                            offsetof(struct RawHalo, field_name))

/**
 * @brief   Fill a slab array from one snapshot file's /halos datasets.
 *
 * Every field of struct RawHalo is populated from the generated property list,
 * so a package that gains or renames a catalog field is followed automatically
 * with no edit here. Both format versions share this path; each has already
 * proven at open that every dataset the list names exists with the right shape.
 */
static void horizontal_h5_fill_halos(hid_t file, const char *path, int64_t n_halos,
                                     struct RawHalo *halos) {
#include "../../include/generated/read_tree_hdf5_properties.inc"
}

#undef READ_TREE_PROPERTY
#undef READ_TREE_PROPERTY_MULTIPLEDIM

/**
 * @brief   Schema-table entry by dataset name, or NULL if not declared.
 *
 * The same lookup pattern horizontal_h5_validate_halo_datasets() already uses to
 * check a dataset's membership, reused here so a dataset's name and on-disk
 * scalar type come from HORIZONTAL_H5_HALO_DATASETS rather than being retyped as
 * literals at the call site. The caller (horizontal_h5_fill_identity() below)
 * still hard-types its destination buffers as int64_t and passes a literal
 * column count of 1; that is not derived from this lookup, and is safe only
 * because both format versions fix ForestIndex and HaloRankInForest as scalar
 * (ncols 0) int64 columns -- format_version 2 in HORIZONTAL_H5_HALO_DATASETS
 * and format_version 3 in HORIZONTAL_H5_V3_FIXED_DATASETS, each checked at
 * open by its own dataset validation. This function does not itself enforce
 * that.
 */
static const struct horizontal_h5_dataset_spec *
horizontal_h5_dataset_spec_by_name(const char *name) {
  for (size_t s = 0; s < HORIZONTAL_H5_HALO_DATASET_COUNT; s++) {
    if (strcmp(HORIZONTAL_H5_HALO_DATASETS[s].name, name) == 0) {
      return &HORIZONTAL_H5_HALO_DATASETS[s];
    }
  }
  return NULL;
}

/* Horizontal-flavoured counterparts of READ_TREE_PROPERTY/READ_TREE_PROPERTY_MULTIPLEDIM
   above, redefined to check membership instead of reading data. Reused here so the
   check walks exactly the read list horizontal_h5_fill_halos() will later use, rather
   than a hand-maintained copy of it drifting out of step. */
#define READ_TREE_PROPERTY(field_name, hdf5_name, type_int, data_type)                             \
  if (horizontal_h5_dataset_spec_by_name(hdf5_name) == NULL) {                                     \
    FATAL_ERROR("Simulation package '%s' declares halo property '%s' as dataset '/halos/%s', "     \
                "which format_version %d of the horizontal_hdf5 contract does not define; the "    \
                "selected package's halo_properties.yaml does not match the horizontal format "    \
                "contract",                                                                        \
                MIMIC_COMPILED_SIMULATION, #field_name, hdf5_name,                                 \
                HORIZONTAL_HDF5_FORMAT_VERSION);                                                   \
  }
#define READ_TREE_PROPERTY_MULTIPLEDIM(field_name, hdf5_name, type_int, data_type)                 \
  READ_TREE_PROPERTY(field_name, hdf5_name, type_int, data_type)

/**
 * @brief   Fail fast if the compiled-in read list names a dataset the format
 *          table does not define.
 *
 * horizontal_h5_fill_halos() fills struct RawHalo by the SELECTED SIMULATION
 * PACKAGE's hdf5_name values, via the same generated read list included below
 * -- not by HORIZONTAL_H5_HALO_DATASETS. Nothing else in open_run checks that
 * those two name sets agree: a package built for a different tree format
 * (for example mini-millennium's lhalo_binary-style halo_properties.yaml,
 * whose hdf5_name values include M_mean200, M_TopHat, Filenr and
 * SubHaloIndex) would otherwise pass every structural, header and identity
 * check above -- for every configured snapshot -- and die only inside the
 * raw H5Dopen2 at the first load_slab. This check depends on no file, so it
 * runs once, before any of that per-snapshot work, and names the offending
 * dataset and package.
 */
static void horizontal_h5_validate_read_list_against_format(void) {
#include "../../include/generated/read_tree_hdf5_properties.inc"
}

#undef READ_TREE_PROPERTY
#undef READ_TREE_PROPERTY_MULTIPLEDIM

/**
 * @brief   Fill the reader-owned identity arrays from one snapshot file.
 *
 * ForestIndex and HaloRankInForest are horizontal-format identity metadata
 * (docs/dev/HORIZONTAL-HDF5-FORMAT.md), not struct RawHalo members: they are
 * read directly by dataset name into slab-owned arrays, independent of
 * halo_properties.yaml and the generated property list that fills struct
 * RawHalo above.
 */
static void horizontal_h5_fill_identity(hid_t file, const char *path, int64_t n_halos,
                                        int64_t *forest_index, int64_t *halo_rank_in_forest) {
  const struct horizontal_h5_dataset_spec *forest_spec =
      horizontal_h5_dataset_spec_by_name("ForestIndex");
  const struct horizontal_h5_dataset_spec *rank_spec =
      horizontal_h5_dataset_spec_by_name("HaloRankInForest");

  horizontal_h5_read_column(file, path, forest_spec->name,
                            horizontal_h5_native_type(forest_spec->type), n_halos, 1, forest_index);
  horizontal_h5_read_column(file, path, rank_spec->name, horizontal_h5_native_type(rank_spec->type),
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
enum horizontal_h5_link_domain {
  HORIZONTAL_H5_LINK_PREV = 0, /* snapshot N-1: progenitors */
  HORIZONTAL_H5_LINK_THIS,     /* snapshot N: siblings and FoF membership */
  HORIZONTAL_H5_LINK_NEXT,     /* snapshot N+1: descendants */
};

struct horizontal_h5_link_spec {
  const char *name;
  size_t offset; /* link field within struct RawHalo */
  size_t width;  /* its storage: int (4 bytes) or long long (8 bytes) */
  int allow_null_link;
  enum horizontal_h5_link_domain domain;
};

#define HORIZONTAL_H5_LINK(field, allow_null, domain)                                              \
  {#field, offsetof(struct RawHalo, field), sizeof(((struct RawHalo *)0)->field), allow_null,      \
   domain}

static const struct horizontal_h5_link_spec HORIZONTAL_H5_LINKS[] = {
    HORIZONTAL_H5_LINK(FirstProgenitor, 1, HORIZONTAL_H5_LINK_PREV),
    HORIZONTAL_H5_LINK(NextProgenitor, 1, HORIZONTAL_H5_LINK_THIS),
    /* Never -1: every halo belongs to a FoF group, at minimum its own. */
    HORIZONTAL_H5_LINK(FirstHaloInFOFgroup, 0, HORIZONTAL_H5_LINK_THIS),
    HORIZONTAL_H5_LINK(NextHaloInFOFgroup, 1, HORIZONTAL_H5_LINK_THIS),
    HORIZONTAL_H5_LINK(Descendant, 1, HORIZONTAL_H5_LINK_NEXT),
};
#define HORIZONTAL_H5_LINK_COUNT (sizeof(HORIZONTAL_H5_LINKS) / sizeof(HORIZONTAL_H5_LINKS[0]))

/* The link validators below read each of these members through offsetof() and
   horizontal_h5_link_value(), which knows exactly two storage widths: the
   generator admits `int` and `long long` for tree_link roles (Gate R0-2(a)).
   Any other width fails to compile rather than validating a truncated or
   over-read value. */
#define HORIZONTAL_H5_LINK_WIDTH_OK(field)                                                         \
  (sizeof(((struct RawHalo *)0)->field) == sizeof(int32_t) ||                                      \
   sizeof(((struct RawHalo *)0)->field) == sizeof(int64_t))
_Static_assert(sizeof(int) == sizeof(int32_t), "an int link must be 32 bits wide");
_Static_assert(HORIZONTAL_H5_LINK_WIDTH_OK(FirstProgenitor),
               "FirstProgenitor must be stored as int or long long");
_Static_assert(HORIZONTAL_H5_LINK_WIDTH_OK(NextProgenitor),
               "NextProgenitor must be stored as int or long long");
_Static_assert(HORIZONTAL_H5_LINK_WIDTH_OK(FirstHaloInFOFgroup),
               "FirstHaloInFOFgroup must be stored as int or long long");
_Static_assert(HORIZONTAL_H5_LINK_WIDTH_OK(NextHaloInFOFgroup),
               "NextHaloInFOFgroup must be stored as int or long long");
_Static_assert(HORIZONTAL_H5_LINK_WIDTH_OK(Descendant),
               "Descendant must be stored as int or long long");
#undef HORIZONTAL_H5_LINK_WIDTH_OK

/** @brief One link member of a slab halo, widened to int64_t by value. */
static int64_t horizontal_h5_link_value(const struct RawHalo *halo, size_t offset, size_t width) {
  const char *field = (const char *)halo + offset;
  if (width == sizeof(int64_t)) {
    int64_t value;
    memcpy(&value, field, sizeof(value));
    return value;
  }
  int32_t value;
  memcpy(&value, field, sizeof(value));
  return value;
}

/**
 * @brief   Halo count bounding one link field, and the snapshot it comes from.
 *
 * Snapshot 0 has no snapshot -1 and the final snapshot has no successor; both
 * are treated as holding zero halos, which is what makes a non-null
 * FirstProgenitor in snapshot 0, and a non-null Descendant in the final
 * snapshot, out of range rather than a special case.
 */
static int64_t horizontal_h5_link_limit(enum horizontal_h5_link_domain domain, int64_t snapnum,
                                        int64_t *bounding_snap) {
  switch (domain) {
  case HORIZONTAL_H5_LINK_PREV:
    *bounding_snap = snapnum - 1;
    return snapnum > 0 ? SNAP.halo_counts[snapnum - 1] : 0;
  case HORIZONTAL_H5_LINK_THIS:
    *bounding_snap = snapnum;
    return SNAP.halo_counts[snapnum];
  case HORIZONTAL_H5_LINK_NEXT:
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
static void horizontal_h5_validate_links(const char *path, int64_t snapnum, int64_t n_halos,
                                         const struct RawHalo *halos) {
  int violated_fields = 0;
  const char *first_field = NULL;
  char first_range[64] = "";
  int64_t first_bad_index = 0;
  int64_t first_bad_value = 0;

  for (size_t s = 0; s < HORIZONTAL_H5_LINK_COUNT; s++) {
    const struct horizontal_h5_link_spec *spec = &HORIZONTAL_H5_LINKS[s];
    int64_t bounding_snap = 0;
    const int64_t limit = horizontal_h5_link_limit(spec->domain, snapnum, &bounding_snap);

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
      const int64_t value = horizontal_h5_link_value(&halos[i], spec->offset, spec->width);
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

/* ---------------------------------------------------------------------------
 * Version 3 link validation
 *
 * Version 3 names each non-FoF link's target file explicitly, so every link is
 * bounded by the n_halos of the snapshot its companion column names, not by an
 * assumed N+1 or N-1 ("Link Scope" in the v3 specification).
 * ------------------------------------------------------------------------- */

enum horizontal_h5_v3_link {
  HORIZONTAL_H5_V3_DESCENDANT = 0,
  HORIZONTAL_H5_V3_FIRST_PROGENITOR,
  HORIZONTAL_H5_V3_NEXT_PROGENITOR,
  HORIZONTAL_H5_V3_TARGETED_LINKS, /* count of the links above, each with a target column */
  HORIZONTAL_H5_V3_FIRST_FOF = HORIZONTAL_H5_V3_TARGETED_LINKS,
  HORIZONTAL_H5_V3_NEXT_FOF,
  HORIZONTAL_H5_V3_LINK_COUNT,
};

struct horizontal_h5_v3_link_spec {
  const char *name;
  size_t offset; /* link field within struct RawHalo */
  size_t width;
  const char *target_name; /* companion target-snapshot column, NULL for FoF links */
  int allow_null_link;
};

#define HORIZONTAL_H5_V3_LINK(field, target, allow_null)                                           \
  {#field, offsetof(struct RawHalo, field), sizeof(((struct RawHalo *)0)->field), target,          \
   allow_null}

static const struct horizontal_h5_v3_link_spec HORIZONTAL_H5_V3_LINKS[HORIZONTAL_H5_V3_LINK_COUNT] =
    {
        HORIZONTAL_H5_V3_LINK(Descendant, "DescendantSnapshot", 1),
        HORIZONTAL_H5_V3_LINK(FirstProgenitor, "FirstProgenitorSnapshot", 1),
        HORIZONTAL_H5_V3_LINK(NextProgenitor, "NextProgenitorSnapshot", 1),
        /* Never -1: every halo belongs to a FoF group, at minimum its own. */
        HORIZONTAL_H5_V3_LINK(FirstHaloInFOFgroup, NULL, 0),
        HORIZONTAL_H5_V3_LINK(NextHaloInFOFgroup, NULL, 1),
};

/**
 * @brief   Check one version 3 link of one halo.
 * @param   target_snap  Its companion target-snapshot value (ignored for FoF).
 * @param   detail       When non-NULL, receives a description of the offence.
 * @return  Non-zero when the link is invalid.
 *
 * Enforces, in order: only -1 is null; the -1-iff--1 biconditional with the
 * companion column; a target snapshot inside the dataset; format invariant 1
 * (descendants strictly later, progenitors strictly earlier) and invariant 2
 * (a NextProgenitor names a halo strictly before its owner's own descendant,
 * which must exist); links_adjacent = 1's claim of an N+1 descendant; and the
 * index range of the target file.
 */
static int horizontal_h5_v3_link_invalid(enum horizontal_h5_v3_link link, int64_t value,
                                         int64_t target_snap, int64_t owner_desc_snap,
                                         int64_t snapnum, char *detail, size_t detail_size) {
  const struct horizontal_h5_v3_link_spec *spec = &HORIZONTAL_H5_V3_LINKS[link];
  char scratch[8];
  if (detail == NULL) {
    detail = scratch;
    detail_size = sizeof(scratch);
  }

  if (value < -1 || (value == -1 && !spec->allow_null_link)) {
    snprintf(detail, detail_size, "'%s' is %" PRId64 "; %s", spec->name, value,
             spec->allow_null_link ? "only -1 is null" : "it is never null");
    return 1;
  }

  if (spec->target_name == NULL) {
    /* FoF links stay within this snapshot. */
    if (value >= SNAP.halo_counts[snapnum]) {
      snprintf(detail, detail_size,
               "'%s' is %" PRId64 ", outside [0, %" PRId64 ") of snapshot %" PRId64, spec->name,
               value, SNAP.halo_counts[snapnum], snapnum);
      return 1;
    }
    return 0;
  }

  if ((value == -1) != (target_snap == -1)) {
    snprintf(detail, detail_size,
             "'%s' is %" PRId64 " but '%s' is %" PRId64 "; each is -1 if and only if the other is",
             spec->name, value, spec->target_name, target_snap);
    return 1;
  }
  if (value == -1) {
    return 0;
  }
  if (target_snap < 0 || target_snap >= SNAP.snapshot_count) {
    snprintf(detail, detail_size,
             "'%s' is %" PRId64 ", which names no snapshot of the dataset [0, %" PRId64 ")",
             spec->target_name, target_snap, SNAP.snapshot_count);
    return 1;
  }

  switch (link) {
  case HORIZONTAL_H5_V3_DESCENDANT:
    if (target_snap <= snapnum) {
      snprintf(detail, detail_size,
               "'%s' is %" PRId64 "; a descendant lies in a strictly later snapshot than %" PRId64,
               spec->target_name, target_snap, snapnum);
      return 1;
    }
    if (SNAP.info.links_adjacent == 1 && target_snap != snapnum + 1) {
      snprintf(detail, detail_size,
               "'%s' is %" PRId64 " but links_adjacent = 1 promises every descendant in "
               "snapshot %" PRId64,
               spec->target_name, target_snap, snapnum + 1);
      return 1;
    }
    break;
  case HORIZONTAL_H5_V3_FIRST_PROGENITOR:
    if (target_snap >= snapnum) {
      snprintf(detail, detail_size,
               "'%s' is %" PRId64
               "; a progenitor lies in a strictly earlier snapshot than %" PRId64,
               spec->target_name, target_snap, snapnum);
      return 1;
    }
    break;
  case HORIZONTAL_H5_V3_NEXT_PROGENITOR:
    if (owner_desc_snap == -1) {
      snprintf(detail, detail_size,
               "'%s' is %" PRId64 " but the halo has no descendant; a NextProgenitor names another "
               "progenitor of the owner's own descendant",
               spec->name, value);
      return 1;
    }
    if (target_snap >= owner_desc_snap) {
      snprintf(detail, detail_size,
               "'%s' is %" PRId64 " but the owner's 'DescendantSnapshot' is %" PRId64 "; every "
               "NextProgenitor chain member lies strictly before the shared descendant",
               spec->target_name, target_snap, owner_desc_snap);
      return 1;
    }
    break;
  default:
    break;
  }

  if (value >= SNAP.halo_counts[target_snap]) {
    snprintf(detail, detail_size,
             "'%s' is %" PRId64 ", outside [0, %" PRId64 ") of snapshot %" PRId64 " named by '%s'",
             spec->name, value, SNAP.halo_counts[target_snap], target_snap, spec->target_name);
    return 1;
  }
  return 0;
}

/**
 * @brief   Validate every link of a loaded version 3 slab, aborting on any
 *          offence.
 *
 * Reads only the slab already in memory -- the links in struct RawHalo and the
 * reader-owned target-snapshot arrays -- so it allocates nothing and re-reads
 * nothing. Diagnostics are bounded exactly as in version 2: each link field
 * contributes at most one counted summary line carrying the offence count and
 * the first offending halo, whatever the number of bad halos.
 */
static void horizontal_h5_v3_validate_links(
    const char *path, int64_t snapnum, int64_t n_halos, const struct RawHalo *halos,
    const int32_t *const target_snaps[HORIZONTAL_H5_V3_TARGETED_LINKS]) {
  int violated_fields = 0;
  const char *first_field = NULL;
  char first_detail[256] = "";
  int64_t first_bad_index = 0;

  for (int link = 0; link < HORIZONTAL_H5_V3_LINK_COUNT; link++) {
    const struct horizontal_h5_v3_link_spec *spec = &HORIZONTAL_H5_V3_LINKS[link];
    /* Only the first HORIZONTAL_H5_V3_TARGETED_LINKS links carry a target column. */
    const int32_t *targets = link < HORIZONTAL_H5_V3_TARGETED_LINKS ? target_snaps[link] : NULL;
    const int32_t *desc_snaps = target_snaps[HORIZONTAL_H5_V3_DESCENDANT];

    int64_t count = 0;
    int64_t bad_index = 0;
    char detail[256] = "";
    for (int64_t i = 0; i < n_halos; i++) {
      const int64_t value = horizontal_h5_link_value(&halos[i], spec->offset, spec->width);
      const int64_t target = targets != NULL ? targets[i] : snapnum;
      if (!horizontal_h5_v3_link_invalid((enum horizontal_h5_v3_link)link, value, target,
                                         desc_snaps[i], snapnum, NULL, 0)) {
        continue;
      }
      if (count == 0) {
        bad_index = i;
        horizontal_h5_v3_link_invalid((enum horizontal_h5_v3_link)link, value, target,
                                      desc_snaps[i], snapnum, detail, sizeof(detail));
      }
      count++;
    }

    if (count == 0) {
      continue;
    }
    /* One counted summary per field, never one line per halo. */
    ERROR_LOG("%s: snapshot %" PRId64 " has %" PRId64 " halo(s) with an invalid '%s'; first at "
              "halo %" PRId64 ": %s",
              path, snapnum, count, spec->name, bad_index, detail);
    if (violated_fields == 0) {
      first_field = spec->name;
      snprintf(first_detail, sizeof(first_detail), "%s", detail);
      first_bad_index = bad_index;
    }
    violated_fields++;
  }

  if (violated_fields > 0) {
    FATAL_ERROR("%s: snapshot %" PRId64 " has %d invalid link field(s); '%s' at halo %" PRId64
                ": %s",
                path, snapnum, violated_fields, first_field, first_bad_index, first_detail);
  }
}

/**
 * @brief   Rounding-tolerance equality between a header value and its
 *          configured counterpart.
 *
 * Asserts the two are the same number, not that they agree scientifically: a
 * non-finite value on either side is rejected outright, an exact-zero pair is
 * accepted outright, and otherwise the absolute difference must be at most
 * 16 * DBL_EPSILON relative to the larger magnitude (not 16 ULPs -- for a
 * normal value that bound is itself several times DBL_EPSILON wide, so this
 * is looser than 16 ULPs).
 */
static int horizontal_h5_physical_value_agrees(double header_value, double configured_value) {
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
 * diagnosable without a debugger. See horizontal_h5_physical_value_agrees() for
 * the tolerance this enforces.
 */
static void horizontal_h5_check_physical_value(const char *path, const char *attr_name,
                                               double header_value, double configured_value) {
  if (!horizontal_h5_physical_value_agrees(header_value, configured_value)) {
    FATAL_ERROR("%s: header attribute '%s' is %.17g but the configured simulation value is "
                "%.17g; they must agree to a rounding tolerance",
                path, attr_name, header_value, configured_value);
  }
}

/* ---------------------------------------------------------------------------
 * Reader hooks
 * ------------------------------------------------------------------------- */

/**
 * @brief   Bytes load_slab allocates per halo across every reader-owned slab array.
 *
 * Lists exactly the arrays load_slab allocates one row each: the raw record, the
 * two identity columns and, for version 3, the three int32 target-snapshot
 * columns and SourceHaloID. open_run publishes it as HorizontalRunInfo.slab_row_bytes
 * for the driver's retention accounting; tests/unit/test_horizontal_retention_budget.c
 * and the micro-uchuu-horizontal package test check it against the allocator.
 * Keep it in step with load_slab.
 */
static int64_t horizontal_h5_slab_row_bytes(int is_v3) {
  size_t row = sizeof(struct RawHalo) + 2 * sizeof(int64_t);
  if (is_v3) {
    row += HORIZONTAL_H5_V3_TARGETED_LINKS * sizeof(int32_t) + sizeof(int64_t);
  }
  return (int64_t)row;
}

/**
 * @brief   Open and fully validate the configured snapshot dataset.
 *
 * Publishes run-scoped metadata and builds the per-snapshot halo-count table
 * served by snapshot_halo_count().
 */
static void open_run_horizontal_hdf5(struct HorizontalRunInfo *info) {
  if (SNAP.is_open) {
    FATAL_ERROR("horizontal_hdf5: open_run called while a run is already open");
  }
  if (MimicConfig.Snaplistlen <= 0) {
    FATAL_ERROR(
        "horizontal_hdf5: the snapshot list is empty (Snaplistlen = %d); there is nothing to "
        "open",
        MimicConfig.Snaplistlen);
  }

  const int64_t snapshot_count = (int64_t)MimicConfig.Snaplistlen;
  int64_t *halo_counts = mymalloc_cat(sizeof(int64_t) * (size_t)snapshot_count, MEM_TREES);

  int32_t format_version = 0;
  int32_t links_adjacent = 0;
  int64_t n_forests_total = 0;
  int64_t max_halo_rank_in_forest = 0;
  int64_t total_halos = 0;
  int64_t measured_max_forest_index = INT64_MIN;
  int64_t measured_max_halo_rank = INT64_MIN;
  int is_empty_dataset = 0;

  /* The dataset's version, fixed by snapshot 0; and, for version 3, snapshot
     0's header strings and /schema, which every later file must repeat. */
  int32_t dataset_version = 0;
  struct horizontal_h5_v3_strings v3_reference;
  struct horizontal_h5_schema v3_reference_schema = {0, NULL};
  memset(&v3_reference, 0, sizeof(v3_reference));

  for (int64_t snap = 0; snap < snapshot_count; snap++) {
    char path[HORIZONTAL_HDF5_PATH_LEN];
    horizontal_h5_format_path(path, sizeof(path), snap);

    if (access(path, R_OK) != 0) {
      FATAL_ERROR("%s: no readable file for configured snapshot %" PRId64
                  "; every snapshot in the snapshot list must have a snapshot_NNN.h5 file",
                  path, snap);
    }

    hid_t file = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0) {
      FATAL_ERROR("%s: could not open the file as HDF5", path);
    }

    /* Version dispatch before any per-version check (Gate R0-1(a)). A file
       whose version cannot be read goes down its dataset's path, whose own
       structure and header checks then name exactly what is wrong. */
    int32_t file_version = 0;
    enum horizontal_h5_peek_fault peek_fault;
    const int has_version = horizontal_h5_peek_format_version(file, &file_version, &peek_fault);
    if (snap == 0) {
      /* Only a version 3 file has /schema (a version 2 file never enters this
         branch), so an unreadable carrier beside /schema is a malformed version
         3 header, not a version 2 file with a bad object set. */
      if (!has_version && H5Lexists(file, "schema", H5P_DEFAULT) > 0) {
        FATAL_ERROR("%s: snapshot 0 carries '/schema', which only a version 3 file has, but its "
                    "'format_version' cannot be read: %s; version 3 requires a hard-linked "
                    "'/header' with a scalar int32 attribute 'format_version'",
                    path, horizontal_h5_peek_fault_text(peek_fault));
      }
      dataset_version = has_version ? file_version : HORIZONTAL_HDF5_FORMAT_VERSION;
      if (dataset_version != HORIZONTAL_HDF5_FORMAT_VERSION &&
          dataset_version != HORIZONTAL_HDF5_FORMAT_VERSION_V3) {
        FATAL_ERROR("%s: header attribute 'format_version' is %" PRId32
                    " but this reader supports only versions %d and %d%s",
                    path, dataset_version, HORIZONTAL_HDF5_FORMAT_VERSION,
                    HORIZONTAL_HDF5_FORMAT_VERSION_V3,
                    dataset_version == 1 ? "; version 1 data was produced with fix_flybys() live "
                                           "and must be reconverted from source"
                                         : "");
      }
    } else if (has_version && file_version != dataset_version) {
      FATAL_ERROR("%s: header attribute 'format_version' is %" PRId32
                  " but snapshot 0 declares version %" PRId32
                  "; a dataset never mixes format versions",
                  path, file_version, dataset_version);
    }
    const int is_v3 = dataset_version == HORIZONTAL_HDF5_FORMAT_VERSION_V3;

    /* Structure before values, values before bulk reads. */
    struct horizontal_h5_header header;
    struct horizontal_h5_v3_strings v3_strings;
    if (is_v3) {
      horizontal_h5_v3_validate_object_set(file, path);
      horizontal_h5_v3_read_header(file, path, &header, &v3_strings);
    } else {
      horizontal_h5_validate_object_set(file, path);
      horizontal_h5_read_header(file, path, &header);
    }

    if (is_v3) {
      if (header.format_version != HORIZONTAL_HDF5_FORMAT_VERSION_V3) {
        FATAL_ERROR("%s: header attribute 'format_version' is %" PRId32
                    " but snapshot 0 declares version %d; a dataset never mixes format versions",
                    path, header.format_version, HORIZONTAL_HDF5_FORMAT_VERSION_V3);
      }
      /* links_adjacent is measured over the whole dataset, so it is 0 or 1 and
         identical in every file. */
      if (header.links_adjacent != 0 && header.links_adjacent != 1) {
        FATAL_ERROR("%s: header attribute 'links_adjacent' is %" PRId32
                    " but format_version %d requires 0 or 1",
                    path, header.links_adjacent, HORIZONTAL_HDF5_FORMAT_VERSION_V3);
      }
    } else {
      if (header.format_version != HORIZONTAL_HDF5_FORMAT_VERSION) {
        FATAL_ERROR("%s: header attribute 'format_version' is %" PRId32
                    " but this reader supports only version %d",
                    path, header.format_version, HORIZONTAL_HDF5_FORMAT_VERSION);
      }
      if (header.links_adjacent != 1) {
        FATAL_ERROR("%s: header attribute 'links_adjacent' is %" PRId32
                    " but format_version %d requires 1",
                    path, header.links_adjacent, HORIZONTAL_HDF5_FORMAT_VERSION);
      }
    }
    if ((int64_t)header.snapshot_number != snap) {
      FATAL_ERROR("%s: header attribute 'snapshot_number' is %" PRId32
                  " but the filename names snapshot %" PRId64,
                  path, header.snapshot_number, snap);
    }
    if (is_v3) {
      /* Version 3 lifts version 2's int32 ceiling: a snapshot may hold more
         than INT32_MAX halos (format invariant 4). */
      if (header.n_halos < 0) {
        FATAL_ERROR("%s: header attribute 'n_halos' is %" PRId64 "; it must be non-negative", path,
                    header.n_halos);
      }
    } else if (header.n_halos < 0 || header.n_halos > (int64_t)INT32_MAX) {
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
       horizontal_h5_physical_value_agrees(). particle_mass_msun_h is compared by
       multiplying the configured value up to native units, matching the
       producer's own operation (scripts/convert/hdf5_writer.py), never by
       dividing the header down. */
    horizontal_h5_check_physical_value(path, "box_size_mpc_h", header.box_size_mpc_h,
                                       MimicConfig.BoxSize);
    horizontal_h5_check_physical_value(path, "omega_matter", header.omega_matter,
                                       MimicConfig.Omega);
    horizontal_h5_check_physical_value(path, "omega_lambda", header.omega_lambda,
                                       MimicConfig.OmegaLambda);
    horizontal_h5_check_physical_value(path, "hubble_h", header.hubble_h, MimicConfig.Hubble_h);
    horizontal_h5_check_physical_value(path, "particle_mass_msun_h", header.particle_mass_msun_h,
                                       MimicConfig.PartMass * 1e10);

    if (snap == 0) {
      format_version = header.format_version;
      links_adjacent = header.links_adjacent;
      n_forests_total = header.n_forests_total;
      max_halo_rank_in_forest = header.max_halo_rank_in_forest;
      is_empty_dataset = (n_forests_total == HORIZONTAL_HDF5_EMPTY_N_FORESTS &&
                          max_halo_rank_in_forest == HORIZONTAL_HDF5_EMPTY_MAX_RANK);
      if (!is_empty_dataset && (n_forests_total < 0 || max_halo_rank_in_forest < 0)) {
        FATAL_ERROR("%s: header attributes 'n_forests_total' (%" PRId64
                    ") and 'max_halo_rank_in_forest' (%" PRId64
                    ") must both be non-negative, or exactly the empty-dataset sentinel (%" PRId64
                    ", %" PRId64 ")",
                    path, n_forests_total, max_halo_rank_in_forest, HORIZONTAL_HDF5_EMPTY_N_FORESTS,
                    HORIZONTAL_HDF5_EMPTY_MAX_RANK);
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
      if (is_v3 && header.links_adjacent != links_adjacent) {
        FATAL_ERROR("%s: header attribute 'links_adjacent' is %" PRId32 " but snapshot 0 declares "
                    "%" PRId32 "; it is measured over the whole dataset and must be identical in "
                    "every file",
                    path, header.links_adjacent, links_adjacent);
      }
    }

    if (is_v3) {
      if (snap == 0) {
        v3_reference = v3_strings;
      } else {
        if (strcmp(v3_strings.source_format, v3_reference.source_format) != 0) {
          FATAL_ERROR("%s: header attribute 'source_format' is '%s' but snapshot 0 declares '%s'; "
                      "it must be identical in every file",
                      path, v3_strings.source_format, v3_reference.source_format);
        }
        if (strcmp(v3_strings.column_mapping_sha256, v3_reference.column_mapping_sha256) != 0) {
          FATAL_ERROR("%s: header attribute 'column_mapping_sha256' is '%s' but snapshot 0 "
                      "declares '%s'; it must be identical in every file",
                      path, v3_strings.column_mapping_sha256, v3_reference.column_mapping_sha256);
        }
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

    if (is_v3) {
      /* /schema structure first, since it defines the /halos set; then its
         identity across files, the /halos set against it, and -- once, since
         every file repeats snapshot 0's /schema -- the package against it. */
      struct horizontal_h5_schema schema = {0, NULL};
      horizontal_h5_v3_read_schema(file, path, &schema);
      if (snap > 0) {
        horizontal_h5_v3_require_same_schema(&v3_reference_schema, &schema, path);
      }
      horizontal_h5_v3_validate_halo_datasets(file, path, header.n_halos, &schema);
      if (snap == 0) {
        horizontal_h5_v3_validate_package(&schema, path);
        v3_reference_schema = schema;
      } else {
        horizontal_h5_v3_free_schema(&schema);
      }
    } else {
      horizontal_h5_validate_halo_datasets(file, path, header.n_halos);

      if (snap == 0) {
        /* Depends on no file: fail fast on a mismatched package before the
           per-snapshot data scans below run for every configured snapshot. */
        horizontal_h5_validate_read_list_against_format();
      }
    }

    /* Invariant 5's measured-data component. Bounded block scans only. */
    horizontal_h5_scan_snapnum(file, path, header.n_halos, header.snapshot_number);
    if (header.n_halos > 0) {
      /* Upper bound stays open here; it is checked below against the run-scoped measured max. */
      const int64_t file_max_rank =
          horizontal_h5_scan_i64_max(file, path, "HaloRankInForest", header.n_halos, 0, INT64_MAX);
      if (file_max_rank > measured_max_halo_rank) {
        measured_max_halo_rank = file_max_rank;
      }
      const int64_t file_max_forest = horizontal_h5_scan_i64_max(
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
  horizontal_h5_v3_free_schema(&v3_reference_schema);

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
     (docs/dev/HORIZONTAL-HDF5-FORMAT.md), verified before anything is published so
     an unencodable dataset never reaches a caller. */
  struct HorizontalRunInfo candidate;
  candidate.snapshot_count = snapshot_count;
  candidate.format_version = format_version;
  candidate.links_adjacent = links_adjacent;
  candidate.slab_row_bytes =
      horizontal_h5_slab_row_bytes(format_version == HORIZONTAL_HDF5_FORMAT_VERSION_V3);
  candidate.n_forests_total = n_forests_total;
  candidate.max_halo_rank_in_forest = max_halo_rank_in_forest;

  if (!horizontal_identity_bounds_valid(&candidate, MimicConfig.UniqueGalaxyIDMultiplier)) {
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
static void close_run_horizontal_hdf5(void) {
  if (!SNAP.is_open) {
    FATAL_ERROR("horizontal_hdf5: close_run called with no open run");
  }
  /* Slabs are reader-owned but caller-scoped: closing the run underneath a
     loaded slab would leave the caller holding a dangling array. */
  if (SNAP.loaded_slabs > 0) {
    FATAL_ERROR("horizontal_hdf5: close_run called with %" PRId64
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
static int64_t snapshot_halo_count_horizontal_hdf5(int64_t snapnum) {
  if (!SNAP.is_open) {
    FATAL_ERROR("horizontal_hdf5: snapshot_halo_count called with no open run");
  }
  if (snapnum < 0 || snapnum >= SNAP.snapshot_count) {
    FATAL_ERROR("horizontal_hdf5: snapshot %" PRId64 " is outside the open run's range [0, %" PRId64
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
 * snapshot number with every array NULL, which snapshot_slab_is_empty()
 * correctly reports as loaded.
 */
static void load_slab_horizontal_hdf5(int64_t snapnum, struct SnapshotSlab *slab) {
  if (!SNAP.is_open) {
    FATAL_ERROR("horizontal_hdf5: load_slab called with no open run");
  }
  if (snapnum < 0 || snapnum >= SNAP.snapshot_count) {
    FATAL_ERROR("horizontal_hdf5: snapshot %" PRId64 " is outside the open run's range [0, %" PRId64
                ")",
                snapnum, SNAP.snapshot_count);
  }
  if (!snapshot_slab_is_empty(slab)) {
    FATAL_ERROR("horizontal_hdf5: load_slab into a slab already holding snapshot %" PRId64
                "; release it before loading snapshot %" PRId64,
                slab->snapnum, snapnum);
  }

  const int64_t n_halos = SNAP.halo_counts[snapnum];
  char path[HORIZONTAL_HDF5_PATH_LEN];
  horizontal_h5_format_path(path, sizeof(path), snapnum);

  const int is_v3 = SNAP.info.format_version == HORIZONTAL_HDF5_FORMAT_VERSION_V3;
  struct RawHalo *halos = NULL;
  int64_t *forest_index = NULL;
  int64_t *halo_rank_in_forest = NULL;
  int32_t *target_snaps[HORIZONTAL_H5_V3_TARGETED_LINKS] = {NULL, NULL, NULL};
  int64_t *source_halo_id = NULL;
  if (n_halos > 0) {
    /* Version 3 lifts the int32 ceiling on n_halos, so the byte counts below
       are checked before they are formed rather than trusted to fit. */
    if ((uint64_t)n_halos > SIZE_MAX / sizeof(struct RawHalo)) {
      FATAL_ERROR("%s: snapshot %" PRId64 " holds %" PRId64 " halos, whose slab of %zu-byte "
                  "records does not fit in this process's address space",
                  path, snapnum, n_halos, sizeof(struct RawHalo));
    }

    hid_t file = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
    if (file < 0) {
      FATAL_ERROR("%s: could not open the file as HDF5 while loading snapshot %" PRId64, path,
                  snapnum);
    }

    halos = mymalloc_cat(sizeof(struct RawHalo) * (size_t)n_halos, MEM_TREES);
    horizontal_h5_fill_halos(file, path, n_halos, halos);

    forest_index = mymalloc_cat(sizeof(int64_t) * (size_t)n_halos, MEM_TREES);
    halo_rank_in_forest = mymalloc_cat(sizeof(int64_t) * (size_t)n_halos, MEM_TREES);
    horizontal_h5_fill_identity(file, path, n_halos, forest_index, halo_rank_in_forest);

    if (is_v3) {
      /* Gate R0-3(a): the three target-snapshot columns and SourceHaloID are
         format metadata held in reader-owned slab arrays, read whole straight
         into those arrays -- they are the slab, so nothing is staged. */
      for (int link = 0; link < HORIZONTAL_H5_V3_TARGETED_LINKS; link++) {
        target_snaps[link] = mymalloc_cat(sizeof(int32_t) * (size_t)n_halos, MEM_TREES);
        horizontal_h5_read_column(file, path, HORIZONTAL_H5_V3_LINKS[link].target_name,
                                  H5T_NATIVE_INT32, n_halos, 1, target_snaps[link]);
      }
      source_halo_id = mymalloc_cat(sizeof(int64_t) * (size_t)n_halos, MEM_TREES);
      horizontal_h5_read_column(file, path, "SourceHaloID", H5T_NATIVE_INT64, n_halos, 1,
                                source_halo_id);
    }

    if (H5Fclose(file) < 0) {
      FATAL_ERROR("%s: could not close the file after loading snapshot %" PRId64, path, snapnum);
    }

    if (is_v3) {
      const int32_t *const targets[HORIZONTAL_H5_V3_TARGETED_LINKS] = {
          target_snaps[0], target_snaps[1], target_snaps[2]};
      horizontal_h5_v3_validate_links(path, snapnum, n_halos, halos, targets);
    } else {
      horizontal_h5_validate_links(path, snapnum, n_halos, halos);
    }
  }

  slab->snapnum = snapnum;
  slab->nhalos = n_halos;
  slab->halos = halos;
  slab->forest_index = forest_index;
  slab->halo_rank_in_forest = halo_rank_in_forest;
  slab->descendant_snapshot = target_snaps[HORIZONTAL_H5_V3_DESCENDANT];
  slab->first_progenitor_snapshot = target_snaps[HORIZONTAL_H5_V3_FIRST_PROGENITOR];
  slab->next_progenitor_snapshot = target_snaps[HORIZONTAL_H5_V3_NEXT_PROGENITOR];
  slab->source_halo_id = source_halo_id;
  SNAP.loaded_slabs++;
}

/** @brief Release a loaded slab. A slab already empty is left untouched. */
static void release_slab_horizontal_hdf5(struct SnapshotSlab *slab) {
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
  if (slab->descendant_snapshot != NULL) {
    myfree(slab->descendant_snapshot);
  }
  if (slab->first_progenitor_snapshot != NULL) {
    myfree(slab->first_progenitor_snapshot);
  }
  if (slab->next_progenitor_snapshot != NULL) {
    myfree(slab->next_progenitor_snapshot);
  }
  if (slab->source_halo_id != NULL) {
    myfree(slab->source_halo_id);
  }
  *slab = snapshot_slab_empty();
  if (SNAP.loaded_slabs > 0) {
    SNAP.loaded_slabs--;
  }
}

/* Horizontal HDF5: one file per snapshot, validated in full at open, read
   one snapshot at a time into a reader-owned slab. */
const struct HorizontalReader HorizontalHDF5Reader = {
    .name = "horizontal_hdf5",
    .processing_order = INPUT_PROCESSING_ORDER_HORIZONTAL,
    .open_run = open_run_horizontal_hdf5,
    .close_run = close_run_horizontal_hdf5,
    .snapshot_halo_count = snapshot_halo_count_horizontal_hdf5,
    .load_slab = load_slab_horizontal_hdf5,
    .release_slab = release_slab_horizontal_hdf5,
};

#endif /* HDF5 */
