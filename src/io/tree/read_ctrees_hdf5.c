/**
 * @file    tree/read_ctrees_hdf5.c
 * @brief   Consistent-Trees forests-HDF5 merger-tree reader (HDF5 builds only).
 *
 * Reads the "forests-HDF5" packaging of Consistent-Trees output (e.g. the Uchuu
 * trees produced by uchuutools) and presents it to the core as the
 * partition/unit model: one output partition per planned forest chunk, one unit
 * per forest. Unlike the ASCII reader, the merger-tree pointers are already
 * stored in the file, so there is no topology reconstruction -- this reader maps
 * a chunk forest range onto the input files, reads each forest's contiguous halo
 * slab, applies the shared Consistent-Trees -> L-Halo value conventions, and
 * bridges the loaded `struct halo_data` into the generated per-simulation
 * `struct RawHalo`.
 *
 * Consistent-Trees is a FORMAT, not a simulation: this reader is
 * simulation-agnostic and reads SimulationDir / particle mass / cosmology from
 * whatever simulation package is compiled in. A package that uses it must
 * declare a RawHalo with the field set the bridge writes and ctrees-native units
 * (Msun/h masses, Mpc/h positions) so the generated reference-unit accessors do
 * the catalog -> reference conversion. See docs/dev/CTREES-UCHUU-VALIDATION.md.
 *
 * Split of responsibilities mirrors the ASCII reader:
 *   - value conventions on the NATIVE Mvir (spin normalisation, particle-count
 *     estimate): apply_ctrees_value_conventions (shared, read_ctrees_common.h);
 *   - unit conversions (mass * 1e-10, positions to Mpc/h): downstream generated
 *     reference-unit accessors, NOT here — this fixes sage-model's missing
 *     kpc/h -> Mpc/h position step by making it metadata.
 *
 * MPI forest distribution is weighted by per-forest halo count (selectable via
 * forest_distribution_scheme / exponent_forest_dist_scheme): the forests-HDF5
 * metadata exposes per-forest halo counts up front, so this reader can balance
 * load by cost rather than by forest count alone (the ASCII reader cannot).
 *
 * Ported from sage-model io/read_tree_consistentrees_hdf5.c (the format source
 * of truth) with sage's run_params replaced by MimicConfig, sage's mymalloc by
 * the categorised Mimic allocator, and sage's error returns turned into
 * FATAL_ERROR at the reader seam.
 */

#ifdef HDF5

#include <hdf5.h>

#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "constants.h"
#include "error.h"
#include "globals.h"
#include "memory.h"
#include "types.h"

#include "tree/ctrees/ctrees_compat.h"
#include "tree/ctrees/forest_utils.h"
#include "tree/chunk_plan.h"
#include "tree/forest_distribution.h"
#include "tree/read_ctrees_common.h"
#include "tree/read_ctrees_hdf5.h"
#include "tree/reader.h"

/* Single error sentinel for the read helpers; every failure is surfaced as a
   FATAL_ERROR at the void open/load/close seam, so the specific value carries no
   meaning beyond "negative == failed". */
#define CT_H5_ERR (-1)

#define CTREES_READ_WINDOW_BYTES (128ULL * 1024ULL * 1024ULL)
#define CTREES_H5_FIELD_BYTES (sizeof(int64_t))

#ifdef MIMIC_TEST_BUILD
#define CTREES_TEST_READ_WINDOW_BYTES (2ULL * CTREES_H5_FIELD_COUNT * CTREES_H5_FIELD_BYTES)
#endif

/* One row of the per-file "ForestInfo" compound dataset. Field order/types match
   the forests-HDF5 layout written by uchuutools (see sage-model). */
struct ctrees_forestinfo {
  int64_t forestid;
  int64_t foresthalosoffset;
  int64_t forestnhalos;
  int64_t forestntrees;
};

enum ctrees_hdf5_field_id {
  CTREES_H5_FIELD_DESCENDANT = 0,
  CTREES_H5_FIELD_FIRST_PROGENITOR,
  CTREES_H5_FIELD_NEXT_PROGENITOR,
  CTREES_H5_FIELD_FIRST_FOF,
  CTREES_H5_FIELD_NEXT_FOF,
  CTREES_H5_FIELD_MVIR,
  CTREES_H5_FIELD_X,
  CTREES_H5_FIELD_Y,
  CTREES_H5_FIELD_Z,
  CTREES_H5_FIELD_VRMS,
  CTREES_H5_FIELD_VMAX,
  CTREES_H5_FIELD_ID,
  CTREES_H5_FIELD_SNAP,
  CTREES_H5_FIELD_VX,
  CTREES_H5_FIELD_VY,
  CTREES_H5_FIELD_VZ,
  CTREES_H5_FIELD_JX,
  CTREES_H5_FIELD_JY,
  CTREES_H5_FIELD_JZ,
  CTREES_H5_FIELD_COUNT
};

static const char *const CTREES_H5_FIXED_FIELD_NAMES[CTREES_H5_FIELD_COUNT] = {
    [CTREES_H5_FIELD_DESCENDANT] = "Descendant",
    [CTREES_H5_FIELD_FIRST_PROGENITOR] = "FirstProgenitor",
    [CTREES_H5_FIELD_NEXT_PROGENITOR] = "NextProgenitor",
    [CTREES_H5_FIELD_FIRST_FOF] = "FirstHaloInFOFgroup",
    [CTREES_H5_FIELD_NEXT_FOF] = "NextHaloInFOFgroup",
    [CTREES_H5_FIELD_MVIR] = "Mvir",
    [CTREES_H5_FIELD_X] = "x",
    [CTREES_H5_FIELD_Y] = "y",
    [CTREES_H5_FIELD_Z] = "z",
    [CTREES_H5_FIELD_VRMS] = "vrms",
    [CTREES_H5_FIELD_VMAX] = "vmax",
    [CTREES_H5_FIELD_ID] = "id",
    [CTREES_H5_FIELD_SNAP] = NULL,
    [CTREES_H5_FIELD_VX] = "vx",
    [CTREES_H5_FIELD_VY] = "vy",
    [CTREES_H5_FIELD_VZ] = "vz",
    [CTREES_H5_FIELD_JX] = "Jx",
    [CTREES_H5_FIELD_JY] = "Jy",
    [CTREES_H5_FIELD_JZ] = "Jz",
};

struct ctrees_hdf5_field_handle {
  hid_t dataset;
  hid_t filespace;
  hid_t datatype;
  hsize_t extent;
  size_t element_size;
  char name[MAX_STRING_LEN];
};

struct ctrees_hdf5_field_cache {
  struct ctrees_hdf5_field_handle fields[CTREES_H5_FIELD_COUNT];
  hsize_t halo_extent;
  int is_open;
};

/* Run-scoped metadata plus the one staged forest range. One reader instance per
   process, so a file-static record is sufficient (mirrors the other readers). */
struct ctrees_hdf5_partition {
  hid_t meta_fd; /* run-scoped forests-HDF5 metadata/data file */
  int totnfiles; /* lastfile + 1 (indexable by file number) */
  int firstfile; /* requested input file range */
  int lastfile;
  int64_t totnforests;           /* global requested forest count */
  int64_t *nforests_per_file;    /* [totnfiles] run-scoped per-file forest counts */
  int64_t *first_forest_in_file; /* [totnfiles] global first forest in each file */
  struct ChunkPlan chunk_plan;   /* run-scoped output chunk ranges */
  double *chunk_costs;           /* [chunk_plan.nchunks] run-scoped LPT costs */

  hid_t *h5_file_groups;                       /* [totnfiles] staged "File%d" groups */
  hid_t *h5_forests_group;                     /* [totnfiles] staged "File%d/Forests" groups */
  struct ctrees_hdf5_field_cache *field_cache; /* [totnfiles] staged SOA field handles */
  int8_t *contig_halo_props;                   /* [totnfiles] 1 = halos stored contiguously (SOA) */
  int start_filenum;                           /* first/last file in the staged range */
  int end_filenum;
  int64_t start_forestnum;                     /* global first forest in the staged range */
  int64_t nforests;                            /* units (forests) in the staged range */
  int32_t *forest_filenum;                     /* [nforests] file holding each forest */
  int64_t *forest_treenr_in_file;              /* [nforests] tree row of each forest in its file */
  struct ctrees_forestinfo **forestinfo_cache; /* [totnfiles][file forest row] */
  int64_t *forestinfo_cache_nrows;             /* [totnfiles] cached rows per file */
  void *read_window_buffer;                    /* [field][halo] bounded SOA slab cache */
  hsize_t read_window_capacity;                /* halos per field in read_window_buffer */
  hsize_t read_window_start_halo;              /* first halo covered in current window */
  hsize_t read_window_len;                     /* halos covered in current window */
  int read_window_file;                        /* file covered by current window, or -1 */
  char snap_field_name[MAX_STRING_LEN];        /* "Snap_num" (older) or "Snap_idx" (newer) */
  int8_t snap_field_is_double;                 /* 1 if the snap field is stored as float */
};
static struct ctrees_hdf5_partition CTH;

/* numpy.allclose defaults: pass if |a-b| <= absdiff or <= reldiff*max(|a|,|b|). */
static int ct_almost_equal(double a, double b, double absdiff, double reldiff) {
  const double diff = fabs(a - b);
  if (diff <= absdiff) {
    return 1;
  }
  const double largest = fabs(a) > fabs(b) ? fabs(a) : fabs(b);
  return diff <= largest * reldiff;
}

/* Read a fixed-size scalar attribute, checking the on-disk size matches the
   destination (ported from sage io/hdf5_read_utils.c read_attribute). */
static int ct_read_attribute(hid_t fd, const char *group_name, const char *attr_name, void *dst,
                             size_t dst_size) {
  hid_t attr_id = H5Aopen_by_name(fd, group_name, attr_name, H5P_DEFAULT, H5P_DEFAULT);
  XRETURN(attr_id >= 0, CT_H5_ERR, "Error: Could not open attribute '%s' in group '%s'\n",
          attr_name, group_name);
  hid_t attr_dtype = H5Aget_type(attr_id);
  XRETURN(attr_dtype >= 0, CT_H5_ERR, "Error: Could not get datatype for attribute '%s'\n",
          attr_name);
  XRETURN(dst_size == H5Tget_size(attr_dtype), CT_H5_ERR,
          "Error: attribute '%s' is %zu bytes on disk but the destination is %zu bytes\n",
          attr_name, H5Tget_size(attr_dtype), dst_size);
  herr_t status = H5Aread(attr_id, attr_dtype, dst);
  XRETURN(status >= 0, CT_H5_ERR, "Error: Could not read attribute '%s' in group '%s'\n", attr_name,
          group_name);
  XRETURN(H5Tclose(attr_dtype) >= 0, CT_H5_ERR, "Error: Could not close datatype for '%s'\n",
          attr_name);
  XRETURN(H5Aclose(attr_id) >= 0, CT_H5_ERR, "Error: Could not close attribute '%s'\n", attr_name);
  return EXIT_SUCCESS;
}

static int ct_h5_get_1d_extent(hid_t h5_fspace, const char *dataset_name, hsize_t *length) {
  const int ndims = H5Sget_simple_extent_ndims(h5_fspace);
  XRETURN(ndims == 1, CT_H5_ERR, "Error: dataset '%s' must be one-dimensional; got rank %d\n",
          dataset_name, ndims);

  hsize_t dims[1] = {0};
  XRETURN(H5Sget_simple_extent_dims(h5_fspace, dims, NULL) == 1, CT_H5_ERR,
          "Error: could not read extent for dataset '%s'\n", dataset_name);
  *length = dims[0];
  return EXIT_SUCCESS;
}

static void init_field_cache_ctrees_hdf5(struct ctrees_hdf5_field_cache *cache) {
  cache->halo_extent = 0;
  cache->is_open = 0;
  for (int ifield = 0; ifield < CTREES_H5_FIELD_COUNT; ifield++) {
    cache->fields[ifield].dataset = -1;
    cache->fields[ifield].filespace = -1;
    cache->fields[ifield].datatype = -1;
    cache->fields[ifield].extent = 0;
    cache->fields[ifield].element_size = 0;
    cache->fields[ifield].name[0] = '\0';
  }
}

static void close_one_field_cache_ctrees_hdf5(struct ctrees_hdf5_field_cache *cache) {
  for (int ifield = 0; ifield < CTREES_H5_FIELD_COUNT; ifield++) {
    struct ctrees_hdf5_field_handle *field = &cache->fields[ifield];
    if (field->datatype >= 0) {
      H5Tclose(field->datatype);
      field->datatype = -1;
    }
    if (field->filespace >= 0) {
      H5Sclose(field->filespace);
      field->filespace = -1;
    }
    if (field->dataset >= 0) {
      H5Dclose(field->dataset);
      field->dataset = -1;
    }
  }
  cache->is_open = 0;
}

static void free_field_cache_ctrees_hdf5(void) {
  if (CTH.field_cache == NULL) {
    return;
  }
  for (int ifile = 0; ifile < CTH.totnfiles; ifile++) {
    close_one_field_cache_ctrees_hdf5(&CTH.field_cache[ifile]);
  }
  myfree(CTH.field_cache);
  CTH.field_cache = NULL;
}

static const char *ctrees_hdf5_field_name(const enum ctrees_hdf5_field_id field_id) {
  if (field_id == CTREES_H5_FIELD_SNAP) {
    return CTH.snap_field_name;
  }
  return CTREES_H5_FIXED_FIELD_NAMES[field_id];
}

static int allocate_read_window_ctrees_hdf5(const size_t window_bytes) {
  const size_t halo_row_bytes = CTREES_H5_FIELD_COUNT * CTREES_H5_FIELD_BYTES;
  CTH.read_window_capacity = (hsize_t)(window_bytes / halo_row_bytes);
  XRETURN(CTH.read_window_capacity > 0, CT_H5_ERR,
          "Error: CTrees HDF5 read window budget %zu bytes cannot hold one %zu-byte halo row\n",
          window_bytes, halo_row_bytes);

  const size_t allocation_bytes =
      (size_t)CTH.read_window_capacity * CTREES_H5_FIELD_COUNT * CTREES_H5_FIELD_BYTES;
  CTH.read_window_buffer = mymalloc_cat(allocation_bytes, MEM_IO);
  CTH.read_window_file = -1;
  CTH.read_window_start_halo = 0;
  CTH.read_window_len = 0;
  return EXIT_SUCCESS;
}

static void free_read_window_ctrees_hdf5(void) {
  if (CTH.read_window_buffer != NULL) {
    myfree(CTH.read_window_buffer);
  }
  CTH.read_window_buffer = NULL;
  CTH.read_window_capacity = 0;
  CTH.read_window_start_halo = 0;
  CTH.read_window_len = 0;
  CTH.read_window_file = -1;
}

static void *ctrees_hdf5_window_field_buffer(const enum ctrees_hdf5_field_id field_id,
                                             const hsize_t halo_offset) {
  unsigned char *base = (unsigned char *)CTH.read_window_buffer;
  const size_t field_offset =
      (size_t)field_id * (size_t)CTH.read_window_capacity * CTREES_H5_FIELD_BYTES;
  return base + field_offset + (size_t)halo_offset * CTREES_H5_FIELD_BYTES;
}

static int open_one_field_cache_ctrees_hdf5(struct ctrees_hdf5_field_cache *cache,
                                            hid_t h5_forests_group, const int ifile) {
  hsize_t expected_extent = 0;
  for (int ifield = 0; ifield < CTREES_H5_FIELD_COUNT; ifield++) {
    struct ctrees_hdf5_field_handle *field = &cache->fields[ifield];
    const char *field_name = ctrees_hdf5_field_name((enum ctrees_hdf5_field_id)ifield);
    XRETURN(field_name != NULL && field_name[0] != '\0', CT_H5_ERR,
            "Error: field %d has no dataset name for file %d\n", ifield, ifile);
    snprintf(field->name, sizeof(field->name), "%s", field_name);

    field->dataset = H5Dopen2(h5_forests_group, field->name, H5P_DEFAULT);
    if (field->dataset < 0) {
      fprintf(stderr, "Error: Could not open dataset '%s' in file %d\n", field->name, ifile);
      return CT_H5_ERR;
    }
    field->filespace = H5Dget_space(field->dataset);
    if (field->filespace < 0) {
      fprintf(stderr, "Error: Could not get filespace for '%s' in file %d\n", field->name, ifile);
      return CT_H5_ERR;
    }
    if (ct_h5_get_1d_extent(field->filespace, field->name, &field->extent) != EXIT_SUCCESS) {
      return CT_H5_ERR;
    }
    if (ifield == 0) {
      expected_extent = field->extent;
    } else if (field->extent != expected_extent) {
      fprintf(stderr, "Error: file %d dataset '%s' has length %llu but '%s' has length %llu\n",
              ifile, field->name, (unsigned long long)field->extent, cache->fields[0].name,
              (unsigned long long)expected_extent);
      return CT_H5_ERR;
    }

    field->datatype = H5Dget_type(field->dataset);
    if (field->datatype < 0) {
      fprintf(stderr, "Error: Could not get datatype for '%s' in file %d\n", field->name, ifile);
      return CT_H5_ERR;
    }
    field->element_size = H5Tget_size(field->datatype);
    if (field->element_size != sizeof(int64_t)) {
      fprintf(stderr,
              "Error: file %d dataset '%s' is %zu bytes on disk but the reader expects 8 bytes\n",
              ifile, field->name, field->element_size);
      return CT_H5_ERR;
    }
  }

  cache->halo_extent = expected_extent;
  cache->is_open = 1;
  return EXIT_SUCCESS;
}

static int open_field_cache_ctrees_hdf5(const int start_filenum, const int end_filenum) {
  CTH.field_cache = mymalloc_cat(CTH.totnfiles * sizeof(*CTH.field_cache), MEM_IO);
  for (int ifile = 0; ifile < CTH.totnfiles; ifile++) {
    init_field_cache_ctrees_hdf5(&CTH.field_cache[ifile]);
  }

  for (int ifile = start_filenum; ifile <= end_filenum; ifile++) {
    if (open_one_field_cache_ctrees_hdf5(&CTH.field_cache[ifile], CTH.h5_forests_group[ifile],
                                         ifile) != EXIT_SUCCESS) {
      free_field_cache_ctrees_hdf5();
      return CT_H5_ERR;
    }
  }
  return EXIT_SUCCESS;
}

static int validate_ctrees_hdf5_forest_slab(const hsize_t halo_extent, const int64_t halosoffset,
                                            const int64_t nhalos, const int unit,
                                            const int filenum) {
  XRETURN(nhalos >= 0, CT_H5_ERR,
          "Error: forest %d in file %d has negative halo count %" PRId64 "\n", unit, filenum,
          nhalos);
  XRETURN(nhalos < INT_MAX, CT_H5_ERR,
          "Error: forest %d in file %d has %" PRId64 " halos, above the int index limit\n", unit,
          filenum, nhalos);
  XRETURN(nhalos < TREE_MUL_FAC, CT_H5_ERR,
          "Error: forest %d in file %d has %" PRId64
          " halos, at or above the unique-galaxy-id limit of %lld\n",
          unit, filenum, nhalos, (long long)TREE_MUL_FAC);
  XRETURN(halosoffset >= 0, CT_H5_ERR,
          "Error: forest %d in file %d has negative halo offset %" PRId64 "\n", unit, filenum,
          halosoffset);

  const hsize_t offset = (hsize_t)halosoffset;
  const hsize_t count = (hsize_t)nhalos;
  XRETURN(offset <= halo_extent && count <= halo_extent - offset, CT_H5_ERR,
          "Error: forest %d in file %d requests halo slab [offset=%" PRId64 ", count=%" PRId64
          ") but Mvir has length %llu\n",
          unit, filenum, halosoffset, nhalos, (unsigned long long)halo_extent);
  return EXIT_SUCCESS;
}

/* Read a contiguous hyperslab from one cached forests-group dataset into a flat buffer. */
static int ct_read_forest_array(const struct ctrees_hdf5_field_handle *field, const hsize_t offset,
                                const hsize_t count, void *buffer, const size_t dst_size) {
  int status = CT_H5_ERR;
  hid_t h5_memspace = -1;

  if (field == NULL || field->dataset < 0 || field->filespace < 0 || field->datatype < 0) {
    fprintf(stderr, "Error: cached HDF5 field handle is not open\n");
    goto cleanup;
  }
  if (offset > field->extent || count > field->extent - offset) {
    fprintf(stderr,
            "Error: dataset '%s' length is %llu but requested slab [offset=%llu, "
            "count=%llu)\n",
            field->name, (unsigned long long)field->extent, (unsigned long long)offset,
            (unsigned long long)count);
    goto cleanup;
  }
  if (H5Sselect_hyperslab(field->filespace, H5S_SELECT_SET, &offset, NULL, &count, NULL) < 0) {
    fprintf(stderr, "Error: Could not select hyperslab for '%s'\n", field->name);
    goto cleanup;
  }
  h5_memspace = H5Screate_simple(1, &count, NULL);
  if (h5_memspace < 0) {
    fprintf(stderr, "Error: Could not create memspace for '%s'\n", field->name);
    goto cleanup;
  }
  if (dst_size != field->element_size) {
    fprintf(stderr, "Error: dataset '%s' is %zu bytes on disk but the destination is %zu bytes\n",
            field->name, field->element_size, dst_size);
    goto cleanup;
  }
  if (H5Dread(field->dataset, field->datatype, h5_memspace, field->filespace, H5P_DEFAULT, buffer) <
      0) {
    fprintf(stderr, "Error: Could not read dataset '%s'\n", field->name);
    goto cleanup;
  }
  status = EXIT_SUCCESS;

cleanup:
  if (h5_memspace >= 0)
    H5Sclose(h5_memspace);
  return status;
}

#define CT_FIELD(cache, field_id) (&(cache)->fields[(field_id)])

#define CT_READ_FOREST_ARRAY(cache, field_id, p_offset, p_count, buffer, dst_type)                 \
  do {                                                                                             \
    status = ct_read_forest_array(CT_FIELD(cache, field_id), *(p_offset), *(p_count), buffer,      \
                                  sizeof(dst_type));                                               \
    if (status != EXIT_SUCCESS)                                                                    \
      goto cleanup;                                                                                \
  } while (0)

#define CT_ASSIGN_SINGLE(buffer, buffer_dtype, dest, field)                                        \
  {                                                                                                \
    buffer_dtype *macro_x = (buffer_dtype *)buffer;                                                \
    for (hsize_t mi = 0; mi < nhalos; mi++) {                                                      \
      dest[mi].field = *macro_x++;                                                                 \
    }                                                                                              \
  }

#define CT_ASSIGN_MULTI(buffer, buffer_dtype, dest, field, dim)                                    \
  {                                                                                                \
    buffer_dtype *macro_x = (buffer_dtype *)buffer;                                                \
    for (hsize_t mi = 0; mi < nhalos; mi++) {                                                      \
      dest[mi].field[dim] = *macro_x++;                                                            \
    }                                                                                              \
  }

#define CT_READ_ASSIGN_SINGLE(cache, field_id, off, cnt, buf, bdt, dst, field)                     \
  {                                                                                                \
    CT_READ_FOREST_ARRAY(cache, field_id, off, cnt, buf, bdt);                                     \
    CT_ASSIGN_SINGLE(buf, bdt, dst, field);                                                        \
  }

#define CT_READ_ASSIGN_MULTI(cache, field_id, off, cnt, buf, bdt, dst, field, dim)                 \
  {                                                                                                \
    CT_READ_FOREST_ARRAY(cache, field_id, off, cnt, buf, bdt);                                     \
    CT_ASSIGN_MULTI(buf, bdt, dst, field, dim);                                                    \
  }

#define CT_ASSIGN_SNAP_INT(cache, buf, dst)                                                        \
  {                                                                                                \
    const struct ctrees_hdf5_field_handle *macro_field = CT_FIELD(cache, CTREES_H5_FIELD_SNAP);    \
    int64_t *macro_x = (int64_t *)buf;                                                             \
    for (hsize_t mi = 0; mi < nhalos; mi++) {                                                      \
      const int64_t macro_v = macro_x[mi];                                                         \
      if (!(macro_v >= 0 && macro_v <= INT_MAX && macro_v <= MimicConfig.LastSnapshotNr)) {        \
        fprintf(stderr, "Error: snapshot field '%s'[%llu] = %lld is outside [0, %d]\n",            \
                macro_field->name, (unsigned long long)mi, (long long)macro_v,                     \
                MimicConfig.LastSnapshotNr);                                                       \
        status = CT_H5_ERR;                                                                        \
        goto cleanup;                                                                              \
      }                                                                                            \
      dst[mi].SnapNum = (int)macro_v;                                                              \
    }                                                                                              \
  }

#define CT_ASSIGN_SNAP_DOUBLE(cache, buf, dst)                                                     \
  {                                                                                                \
    const struct ctrees_hdf5_field_handle *macro_field = CT_FIELD(cache, CTREES_H5_FIELD_SNAP);    \
    double *macro_x = (double *)buf;                                                               \
    for (hsize_t mi = 0; mi < nhalos; mi++) {                                                      \
      const double macro_v = macro_x[mi];                                                          \
      if (!(isfinite(macro_v) && floor(macro_v) == macro_v && macro_v >= 0.0 &&                    \
            macro_v <= (double)INT_MAX && macro_v <= (double)MimicConfig.LastSnapshotNr)) {        \
        fprintf(stderr, "Error: snapshot field '%s'[%llu] = %.17g is not an integer in [0, %d]\n", \
                macro_field->name, (unsigned long long)mi, macro_v, MimicConfig.LastSnapshotNr);   \
        status = CT_H5_ERR;                                                                        \
        goto cleanup;                                                                              \
      }                                                                                            \
      dst[mi].SnapNum = (int)macro_v;                                                              \
    }                                                                                              \
  }

#define CT_READ_ASSIGN_SNAP_INT(cache, off, cnt, buf, dst)                                         \
  {                                                                                                \
    CT_READ_FOREST_ARRAY(cache, CTREES_H5_FIELD_SNAP, off, cnt, buf, int64_t);                     \
    CT_ASSIGN_SNAP_INT(cache, buf, dst);                                                           \
  }

#define CT_READ_ASSIGN_SNAP_DOUBLE(cache, off, cnt, buf, dst)                                      \
  {                                                                                                \
    CT_READ_FOREST_ARRAY(cache, CTREES_H5_FIELD_SNAP, off, cnt, buf, double);                      \
    CT_ASSIGN_SNAP_DOUBLE(cache, buf, dst);                                                        \
  }

/* Read one int64 merger-link field and narrow it into halo_data's int field,
   validating each value is the no-link sentinel (-1) or a forest-local index in
   [0, nhalos). The core uses these links as array indices during traversal, so a
   wrapped or out-of-range value from a malformed/schema-mismatched file would be
   an out-of-bounds access; fail fast instead of narrowing blindly. */
#define CT_ASSIGN_LINK(cache, field_id, buf, dst, field)                                           \
  {                                                                                                \
    const struct ctrees_hdf5_field_handle *macro_field = CT_FIELD(cache, field_id);                \
    int64_t *macro_x = (int64_t *)buf;                                                             \
    for (hsize_t mi = 0; mi < nhalos; mi++) {                                                      \
      const int64_t macro_v = macro_x[mi];                                                         \
      if (!(macro_v >= -1 && macro_v < (int64_t)nhalos)) {                                         \
        fprintf(stderr, "Error: merger link '%s'[%llu] = %lld is outside [-1, %llu)\n",            \
                macro_field->name, (unsigned long long)mi, (long long)macro_v,                     \
                (unsigned long long)nhalos);                                                       \
        status = CT_H5_ERR;                                                                        \
        goto cleanup;                                                                              \
      }                                                                                            \
      dst[mi].field = (int)macro_v;                                                                \
    }                                                                                              \
  }

#define CT_READ_ASSIGN_LINK(cache, field_id, off, cnt, buf, dst, field)                            \
  {                                                                                                \
    CT_READ_FOREST_ARRAY(cache, field_id, off, cnt, buf, int64_t);                                 \
    CT_ASSIGN_LINK(cache, field_id, buf, dst, field);                                              \
  }

static int assign_forest_from_window_ctrees_hdf5(struct ctrees_hdf5_field_cache *field_cache,
                                                 const hsize_t nhalos,
                                                 const hsize_t window_halo_offset,
                                                 const int8_t snap_field_is_double,
                                                 struct halo_data *halos) {
  int status = EXIT_SUCCESS;

  void *desc = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_DESCENDANT, window_halo_offset);
  CT_ASSIGN_LINK(field_cache, CTREES_H5_FIELD_DESCENDANT, desc, halos, Descendant);
  void *first_prog =
      ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_FIRST_PROGENITOR, window_halo_offset);
  CT_ASSIGN_LINK(field_cache, CTREES_H5_FIELD_FIRST_PROGENITOR, first_prog, halos, FirstProgenitor);
  void *next_prog =
      ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_NEXT_PROGENITOR, window_halo_offset);
  CT_ASSIGN_LINK(field_cache, CTREES_H5_FIELD_NEXT_PROGENITOR, next_prog, halos, NextProgenitor);
  void *first_fof = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_FIRST_FOF, window_halo_offset);
  CT_ASSIGN_LINK(field_cache, CTREES_H5_FIELD_FIRST_FOF, first_fof, halos, FirstHaloInFOFgroup);
  void *next_fof = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_NEXT_FOF, window_halo_offset);
  CT_ASSIGN_LINK(field_cache, CTREES_H5_FIELD_NEXT_FOF, next_fof, halos, NextHaloInFOFgroup);

  void *mvir = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_MVIR, window_halo_offset);
  CT_ASSIGN_SINGLE(mvir, double, halos, Mvir);

  void *x = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_X, window_halo_offset);
  CT_ASSIGN_MULTI(x, double, halos, Pos, 0);
  void *y = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_Y, window_halo_offset);
  CT_ASSIGN_MULTI(y, double, halos, Pos, 1);
  void *z = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_Z, window_halo_offset);
  CT_ASSIGN_MULTI(z, double, halos, Pos, 2);

  void *vrms = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_VRMS, window_halo_offset);
  CT_ASSIGN_SINGLE(vrms, double, halos, VelDisp);
  void *vmax = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_VMAX, window_halo_offset);
  CT_ASSIGN_SINGLE(vmax, double, halos, Vmax);
  void *id = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_ID, window_halo_offset);
  CT_ASSIGN_SINGLE(id, int64_t, halos, MostBoundID);

  void *snap = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_SNAP, window_halo_offset);
  if (snap_field_is_double) {
    CT_ASSIGN_SNAP_DOUBLE(field_cache, snap, halos);
  } else {
    CT_ASSIGN_SNAP_INT(field_cache, snap, halos);
  }

  void *vx = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_VX, window_halo_offset);
  CT_ASSIGN_MULTI(vx, double, halos, Vel, 0);
  void *vy = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_VY, window_halo_offset);
  CT_ASSIGN_MULTI(vy, double, halos, Vel, 1);
  void *vz = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_VZ, window_halo_offset);
  CT_ASSIGN_MULTI(vz, double, halos, Vel, 2);

  void *jx = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_JX, window_halo_offset);
  CT_ASSIGN_MULTI(jx, double, halos, Spin, 0);
  void *jy = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_JY, window_halo_offset);
  CT_ASSIGN_MULTI(jy, double, halos, Spin, 1);
  void *jz = ctrees_hdf5_window_field_buffer(CTREES_H5_FIELD_JZ, window_halo_offset);
  CT_ASSIGN_MULTI(jz, double, halos, Spin, 2);

cleanup:
  return status;
}

/* Read one forest's contiguous (SOA) halo slab into `halos`. Only the fields the
   bridge consumes are read (the array-of-structs packaging is not supported, as
   in sage). The five merger links are read int64 and validated forest-local
   before narrowing to halo_data's int fields (see CT_READ_ASSIGN_LINK). */
static int read_contiguous_forest_ctrees_h5(struct ctrees_hdf5_field_cache *field_cache,
                                            const hsize_t nhalos, const hsize_t halosoffset,
                                            const int8_t snap_field_is_double,
                                            struct halo_data *halos) {
  if (nhalos == 0) {
    return EXIT_SUCCESS;
  }

  int status = EXIT_SUCCESS;
  void *buffer = mymalloc_cat(nhalos * sizeof(double), MEM_IO); /* double is the widest field */

  CT_READ_ASSIGN_LINK(field_cache, CTREES_H5_FIELD_DESCENDANT, &halosoffset, &nhalos, buffer, halos,
                      Descendant);
  CT_READ_ASSIGN_LINK(field_cache, CTREES_H5_FIELD_FIRST_PROGENITOR, &halosoffset, &nhalos, buffer,
                      halos, FirstProgenitor);
  CT_READ_ASSIGN_LINK(field_cache, CTREES_H5_FIELD_NEXT_PROGENITOR, &halosoffset, &nhalos, buffer,
                      halos, NextProgenitor);
  CT_READ_ASSIGN_LINK(field_cache, CTREES_H5_FIELD_FIRST_FOF, &halosoffset, &nhalos, buffer, halos,
                      FirstHaloInFOFgroup);
  CT_READ_ASSIGN_LINK(field_cache, CTREES_H5_FIELD_NEXT_FOF, &halosoffset, &nhalos, buffer, halos,
                      NextHaloInFOFgroup);

  CT_READ_ASSIGN_SINGLE(field_cache, CTREES_H5_FIELD_MVIR, &halosoffset, &nhalos, buffer, double,
                        halos, Mvir); /* native Msun/h; accessor scales */

  CT_READ_ASSIGN_MULTI(field_cache, CTREES_H5_FIELD_X, &halosoffset, &nhalos, buffer, double, halos,
                       Pos, 0);
  CT_READ_ASSIGN_MULTI(field_cache, CTREES_H5_FIELD_Y, &halosoffset, &nhalos, buffer, double, halos,
                       Pos, 1);
  CT_READ_ASSIGN_MULTI(field_cache, CTREES_H5_FIELD_Z, &halosoffset, &nhalos, buffer, double, halos,
                       Pos, 2);

  CT_READ_ASSIGN_SINGLE(field_cache, CTREES_H5_FIELD_VRMS, &halosoffset, &nhalos, buffer, double,
                        halos, VelDisp);
  CT_READ_ASSIGN_SINGLE(field_cache, CTREES_H5_FIELD_VMAX, &halosoffset, &nhalos, buffer, double,
                        halos, Vmax);
  CT_READ_ASSIGN_SINGLE(field_cache, CTREES_H5_FIELD_ID, &halosoffset, &nhalos, buffer, int64_t,
                        halos, MostBoundID); /* the carried-through ctrees halo id */

  if (snap_field_is_double) {
    CT_READ_ASSIGN_SNAP_DOUBLE(field_cache, &halosoffset, &nhalos, buffer, halos);
  } else {
    CT_READ_ASSIGN_SNAP_INT(field_cache, &halosoffset, &nhalos, buffer, halos);
  }

  CT_READ_ASSIGN_MULTI(field_cache, CTREES_H5_FIELD_VX, &halosoffset, &nhalos, buffer, double,
                       halos, Vel, 0);
  CT_READ_ASSIGN_MULTI(field_cache, CTREES_H5_FIELD_VY, &halosoffset, &nhalos, buffer, double,
                       halos, Vel, 1);
  CT_READ_ASSIGN_MULTI(field_cache, CTREES_H5_FIELD_VZ, &halosoffset, &nhalos, buffer, double,
                       halos, Vel, 2);

  /* Spin holds the angular momentum J here; apply_ctrees_value_conventions
     normalises it by the native Mvir afterwards. */
  CT_READ_ASSIGN_MULTI(field_cache, CTREES_H5_FIELD_JX, &halosoffset, &nhalos, buffer, double,
                       halos, Spin, 0);
  CT_READ_ASSIGN_MULTI(field_cache, CTREES_H5_FIELD_JY, &halosoffset, &nhalos, buffer, double,
                       halos, Spin, 1);
  CT_READ_ASSIGN_MULTI(field_cache, CTREES_H5_FIELD_JZ, &halosoffset, &nhalos, buffer, double,
                       halos, Spin, 2);

cleanup:
  myfree(buffer);
  return status;
}

static int read_window_covers_ctrees_hdf5(const int filenum, const hsize_t halosoffset,
                                          const hsize_t nhalos) {
  if (CTH.read_window_file != filenum || halosoffset < CTH.read_window_start_halo) {
    return 0;
  }
  const hsize_t window_offset = halosoffset - CTH.read_window_start_halo;
  return window_offset <= CTH.read_window_len && nhalos <= CTH.read_window_len - window_offset;
}

static int refill_read_window_ctrees_hdf5(struct ctrees_hdf5_field_cache *field_cache,
                                          const int filenum, const hsize_t halosoffset,
                                          const hsize_t nhalos) {
  XRETURN(CTH.read_window_buffer != NULL && CTH.read_window_capacity > 0, CT_H5_ERR,
          "Error: CTrees HDF5 read window is not allocated\n");
  XRETURN(nhalos <= CTH.read_window_capacity, CT_H5_ERR,
          "Error: requested forest with %llu halos exceeds the CTrees HDF5 read window capacity "
          "of %llu halos\n",
          (unsigned long long)nhalos, (unsigned long long)CTH.read_window_capacity);
  XRETURN(halosoffset <= field_cache->halo_extent, CT_H5_ERR,
          "Error: read-window offset %llu exceeds field extent %llu in file %d\n",
          (unsigned long long)halosoffset, (unsigned long long)field_cache->halo_extent, filenum);

  hsize_t window_len = CTH.read_window_capacity;
  const hsize_t remaining = field_cache->halo_extent - halosoffset;
  if (window_len > remaining) {
    window_len = remaining;
  }
  XRETURN(window_len >= nhalos, CT_H5_ERR,
          "Error: read-window refill in file %d covers %llu halos but forest needs %llu\n", filenum,
          (unsigned long long)window_len, (unsigned long long)nhalos);

  for (int ifield = 0; ifield < CTREES_H5_FIELD_COUNT; ifield++) {
    void *dst = ctrees_hdf5_window_field_buffer((enum ctrees_hdf5_field_id)ifield, 0);
    if (ct_read_forest_array(&field_cache->fields[ifield], halosoffset, window_len, dst,
                             CTREES_H5_FIELD_BYTES) != EXIT_SUCCESS) {
      return CT_H5_ERR;
    }
  }

  CTH.read_window_file = filenum;
  CTH.read_window_start_halo = halosoffset;
  CTH.read_window_len = window_len;
  return EXIT_SUCCESS;
}

static int read_windowed_forest_ctrees_h5(struct ctrees_hdf5_field_cache *field_cache,
                                          const int filenum, const hsize_t nhalos,
                                          const hsize_t halosoffset,
                                          const int8_t snap_field_is_double,
                                          struct halo_data *halos) {
  if (nhalos == 0) {
    return EXIT_SUCCESS;
  }
  XRETURN(CTH.read_window_buffer != NULL && CTH.read_window_capacity > 0, CT_H5_ERR,
          "Error: CTrees HDF5 read window is not allocated\n");

  if (nhalos > CTH.read_window_capacity) {
    return read_contiguous_forest_ctrees_h5(field_cache, nhalos, halosoffset, snap_field_is_double,
                                            halos);
  }

  if (!read_window_covers_ctrees_hdf5(filenum, halosoffset, nhalos) &&
      refill_read_window_ctrees_hdf5(field_cache, filenum, halosoffset, nhalos) != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }

  const hsize_t window_halo_offset = halosoffset - CTH.read_window_start_halo;
  return assign_forest_from_window_ctrees_hdf5(field_cache, nhalos, window_halo_offset,
                                               snap_field_is_double, halos);
}

static int validate_forestinfo_cache_row_ctrees_hdf5(const struct ctrees_forestinfo *finfo,
                                                     const int64_t row, const int ifile) {
  XRETURN(finfo->forestnhalos >= 0, CT_H5_ERR,
          "Error: file %d ForestInfo row %" PRId64 " has negative ForestNhalos=%" PRId64 "\n",
          ifile, row, finfo->forestnhalos);
  XRETURN(finfo->forestnhalos < INT_MAX, CT_H5_ERR,
          "Error: file %d ForestInfo row %" PRId64 " has %" PRId64
          " halos, above the int index limit\n",
          ifile, row, finfo->forestnhalos);
  XRETURN(finfo->forestnhalos < TREE_MUL_FAC, CT_H5_ERR,
          "Error: file %d ForestInfo row %" PRId64 " has %" PRId64
          " halos, at or above the unique-galaxy-id limit of %lld\n",
          ifile, row, finfo->forestnhalos, (long long)TREE_MUL_FAC);
  XRETURN(finfo->foresthalosoffset >= 0, CT_H5_ERR,
          "Error: file %d ForestInfo row %" PRId64 " has negative ForestHalosOffset=%" PRId64 "\n",
          ifile, row, finfo->foresthalosoffset);
  return EXIT_SUCCESS;
}

static hid_t create_forestinfo_memory_type_ctrees_hdf5(void) {
  hid_t dtype = H5Tcreate(H5T_COMPOUND, sizeof(struct ctrees_forestinfo));
  if (dtype < 0) {
    return -1;
  }
  if (H5Tinsert(dtype, "ForestID", HOFFSET(struct ctrees_forestinfo, forestid), H5T_NATIVE_INT64) <
          0 ||
      H5Tinsert(dtype, "ForestHalosOffset", HOFFSET(struct ctrees_forestinfo, foresthalosoffset),
                H5T_NATIVE_INT64) < 0 ||
      H5Tinsert(dtype, "ForestNhalos", HOFFSET(struct ctrees_forestinfo, forestnhalos),
                H5T_NATIVE_INT64) < 0 ||
      H5Tinsert(dtype, "ForestNTrees", HOFFSET(struct ctrees_forestinfo, forestntrees),
                H5T_NATIVE_INT64) < 0) {
    H5Tclose(dtype);
    return -1;
  }
  return dtype;
}

static void free_forestinfo_cache_ctrees_hdf5(void) {
  if (CTH.forestinfo_cache != NULL) {
    for (int i = 0; i < CTH.totnfiles; i++) {
      if (CTH.forestinfo_cache[i] != NULL) {
        myfree(CTH.forestinfo_cache[i]);
      }
    }
    myfree(CTH.forestinfo_cache);
    CTH.forestinfo_cache = NULL;
  }
  if (CTH.forestinfo_cache_nrows != NULL) {
    myfree(CTH.forestinfo_cache_nrows);
    CTH.forestinfo_cache_nrows = NULL;
  }
}

static int load_forestinfo_cache_ctrees_hdf5(const int start_filenum, const int end_filenum,
                                             const int64_t *totnforests_per_file) {
  CTH.forestinfo_cache = mymalloc_cat(CTH.totnfiles * sizeof(*CTH.forestinfo_cache), MEM_IO);
  CTH.forestinfo_cache_nrows =
      mymalloc_cat(CTH.totnfiles * sizeof(*CTH.forestinfo_cache_nrows), MEM_IO);
  for (int i = 0; i < CTH.totnfiles; i++) {
    CTH.forestinfo_cache[i] = NULL;
    CTH.forestinfo_cache_nrows[i] = 0;
  }

  for (int ifile = start_filenum; ifile <= end_filenum; ifile++) {
    int file_status = CT_H5_ERR;
    hid_t finfo_dset = -1;
    hid_t finfo_fspace = -1;
    hid_t finfo_memspace = -1;
    hid_t finfo_file_dtype = -1;
    hid_t finfo_mem_dtype = -1;
    const int64_t nforests_this_file = totnforests_per_file[ifile];
    XRETURN(nforests_this_file >= 1, CT_H5_ERR,
            "Error: file %d reports %" PRId64 " forests (expected >= 1)\n", ifile,
            nforests_this_file);

    finfo_dset = H5Dopen2(CTH.h5_file_groups[ifile], "ForestInfo", H5P_DEFAULT);
    if (finfo_dset < 0) {
      fprintf(stderr, "Error: Could not open 'ForestInfo' in file %d\n", ifile);
      goto forestinfo_cache_cleanup;
    }
    finfo_fspace = H5Dget_space(finfo_dset);
    if (finfo_fspace < 0) {
      fprintf(stderr, "Error: Could not get 'ForestInfo' space in file %d\n", ifile);
      goto forestinfo_cache_cleanup;
    }
    hsize_t finfo_length = 0;
    if (ct_h5_get_1d_extent(finfo_fspace, "ForestInfo", &finfo_length) != EXIT_SUCCESS) {
      goto forestinfo_cache_cleanup;
    }
    if ((hsize_t)nforests_this_file != finfo_length) {
      fprintf(stderr,
              "Error: file %d reports %" PRId64 " forests but 'ForestInfo' contains %llu rows\n",
              ifile, nforests_this_file, (unsigned long long)finfo_length);
      goto forestinfo_cache_cleanup;
    }
    finfo_file_dtype = H5Dget_type(finfo_dset);
    if (finfo_file_dtype < 0) {
      fprintf(stderr, "Error: Could not get 'ForestInfo' datatype in file %d\n", ifile);
      goto forestinfo_cache_cleanup;
    }
    const size_t dtype_size = H5Tget_size(finfo_file_dtype);
    if (dtype_size != sizeof(struct ctrees_forestinfo)) {
      fprintf(stderr,
              "Error: file %d 'ForestInfo' record is %zu bytes on disk but the reader expects %zu "
              "(4 x int64); dataset layout mismatch\n",
              ifile, dtype_size, sizeof(struct ctrees_forestinfo));
      goto forestinfo_cache_cleanup;
    }
    finfo_mem_dtype = create_forestinfo_memory_type_ctrees_hdf5();
    if (finfo_mem_dtype < 0) {
      fprintf(stderr, "Error: Could not create cached 'ForestInfo' memory datatype for file %d\n",
              ifile);
      goto forestinfo_cache_cleanup;
    }

    const hsize_t count = (hsize_t)nforests_this_file;
    finfo_memspace = H5Screate_simple(1, &count, NULL);
    if (finfo_memspace < 0) {
      fprintf(stderr, "Error: Could not create cached 'ForestInfo' memspace for file %d\n", ifile);
      goto forestinfo_cache_cleanup;
    }
    CTH.forestinfo_cache[ifile] =
        mymalloc_cat(count * sizeof(*CTH.forestinfo_cache[ifile]), MEM_IO);
    if (H5Dread(finfo_dset, finfo_mem_dtype, finfo_memspace, finfo_fspace, H5P_DEFAULT,
                CTH.forestinfo_cache[ifile]) < 0) {
      fprintf(stderr, "Error: Could not read cached 'ForestInfo' rows in file %d\n", ifile);
      goto forestinfo_cache_cleanup;
    }
    for (int64_t row = 0; row < nforests_this_file; row++) {
      if (validate_forestinfo_cache_row_ctrees_hdf5(&CTH.forestinfo_cache[ifile][row], row,
                                                    ifile) != EXIT_SUCCESS) {
        goto forestinfo_cache_cleanup;
      }
    }
    CTH.forestinfo_cache_nrows[ifile] = nforests_this_file;
    file_status = EXIT_SUCCESS;

  forestinfo_cache_cleanup:
    if (finfo_mem_dtype >= 0)
      H5Tclose(finfo_mem_dtype);
    if (finfo_file_dtype >= 0)
      H5Tclose(finfo_file_dtype);
    if (finfo_memspace >= 0)
      H5Sclose(finfo_memspace);
    if (finfo_fspace >= 0)
      H5Sclose(finfo_fspace);
    if (finfo_dset >= 0)
      H5Dclose(finfo_dset);
    if (file_status != EXIT_SUCCESS) {
      return CT_H5_ERR;
    }
  }
  return EXIT_SUCCESS;
}

/* Read the per-forest halo counts (the "ForestNhalos" member of one file's
   "ForestInfo" compound dataset) into a caller-owned per-file buffer. */
static int read_nhalos_for_file(const int ifile, const int64_t nforests_this_file,
                                int64_t *nhalos_per_forest) {
  int file_status = CT_H5_ERR;
  hid_t finfo_dset = -1;
  hid_t finfo_fspace = -1;
  hid_t finfo_memspace = -1;
  hid_t nhalos_dtype = -1;
  char dataset_name[MAX_STRING_LEN];

  snprintf(dataset_name, sizeof(dataset_name), "File%d/ForestInfo", ifile);
  finfo_dset = H5Dopen2(CTH.meta_fd, dataset_name, H5P_DEFAULT);
  if (finfo_dset < 0) {
    fprintf(stderr, "Error: Could not open 'ForestInfo' in file %d\n", ifile);
    goto forestinfo_cleanup;
  }
  finfo_fspace = H5Dget_space(finfo_dset);
  if (finfo_fspace < 0) {
    fprintf(stderr, "Error: Could not get 'ForestInfo' space in file %d\n", ifile);
    goto forestinfo_cleanup;
  }
  hsize_t finfo_length = 0;
  if (ct_h5_get_1d_extent(finfo_fspace, "ForestInfo", &finfo_length) != EXIT_SUCCESS) {
    goto forestinfo_cleanup;
  }
  if ((hsize_t)nforests_this_file != finfo_length) {
    fprintf(stderr,
            "Error: file %d reports %" PRId64 " forests but 'ForestInfo' contains %llu rows\n",
            ifile, nforests_this_file, (unsigned long long)finfo_length);
    goto forestinfo_cleanup;
  }
  const hsize_t count = (hsize_t)nforests_this_file;
  finfo_memspace = H5Screate_simple(1, &count, NULL);
  if (finfo_memspace < 0) {
    fprintf(stderr, "Error: Could not create 'ForestInfo' memspace\n");
    goto forestinfo_cleanup;
  }
  nhalos_dtype = H5Tcreate(H5T_COMPOUND, sizeof(int64_t));
  if (nhalos_dtype < 0) {
    fprintf(stderr, "Error: Could not create compound type (file %d)\n", ifile);
    goto forestinfo_cleanup;
  }
  if (H5Tinsert(nhalos_dtype, "ForestNhalos", 0, H5T_NATIVE_INT64) < 0) {
    fprintf(stderr, "Error: Could not insert 'ForestNhalos' field (file %d)\n", ifile);
    goto forestinfo_cleanup;
  }
  if (H5Dread(finfo_dset, nhalos_dtype, finfo_memspace, finfo_fspace, H5P_DEFAULT,
              nhalos_per_forest) < 0) {
    fprintf(stderr, "Error: Could not read 'ForestNhalos' (file %d)\n", ifile);
    goto forestinfo_cleanup;
  }
  for (int64_t i = 0; i < nforests_this_file; i++) {
    if (nhalos_per_forest[i] < 0) {
      fprintf(stderr,
              "Error: file %d ForestInfo row %" PRId64 " has negative ForestNhalos=%" PRId64 "\n",
              ifile, i, nhalos_per_forest[i]);
      goto forestinfo_cleanup;
    }
  }
  file_status = EXIT_SUCCESS;

forestinfo_cleanup:
  if (nhalos_dtype >= 0)
    H5Tclose(nhalos_dtype);
  if (finfo_memspace >= 0)
    H5Sclose(finfo_memspace);
  if (finfo_fspace >= 0)
    H5Sclose(finfo_fspace);
  if (finfo_dset >= 0)
    H5Dclose(finfo_dset);
  return file_status;
}

#ifdef MIMIC_TEST_BUILD
static int read_nhalos_per_forest(const int firstfile, const int lastfile,
                                  const int64_t *totnforests_per_file, int64_t *nhalos_per_forest) {
  int64_t written = 0;
  for (int ifile = firstfile; ifile <= lastfile; ifile++) {
    const int64_t nforests_this_file = totnforests_per_file[ifile];
    if (read_nhalos_for_file(ifile, nforests_this_file, &nhalos_per_forest[written]) !=
        EXIT_SUCCESS) {
      return CT_H5_ERR;
    }
    written += nforests_this_file;
  }
  return EXIT_SUCCESS;
}
#endif

static int build_chunk_plan_ctrees_hdf5(void) {
  XRETURN(
      CTH.totnforests >= 1 && CTH.nforests_per_file != NULL, CT_H5_ERR,
      "Error: Consistent-Trees HDF5 reader cannot build chunks before forest counts are read\n");

  struct ChunkPlanBuilder builder;
  XRETURN(chunk_plan_builder_init(&builder, (double)MimicConfig.TargetFileSize,
                                  MimicConfig.ForestsPerFile) == 0,
          CT_H5_ERR,
          "Error: failed to initialise Consistent-Trees HDF5 chunk planner "
          "(target_file_size=%" PRId64 ", forests_per_file=%" PRId64 ")\n",
          MimicConfig.TargetFileSize, MimicConfig.ForestsPerFile);

  const enum ForestDistributionScheme scheme =
      (enum ForestDistributionScheme)MimicConfig.ForestDistributionScheme;
  for (int ifile = CTH.firstfile; ifile <= CTH.lastfile; ifile++) {
    const int64_t nforests_this_file = CTH.nforests_per_file[ifile];
    if ((uint64_t)nforests_this_file > (uint64_t)(SIZE_MAX / sizeof(double))) {
      fprintf(stderr, "Error: file %d has too many forests to stage planning buffers\n", ifile);
      chunk_plan_free(&builder.plan);
      return CT_H5_ERR;
    }

    int64_t *nhalos_per_forest =
        mymalloc_cat((size_t)nforests_this_file * sizeof(*nhalos_per_forest), MEM_IO);
    double *size_per_forest =
        mymalloc_cat((size_t)nforests_this_file * sizeof(*size_per_forest), MEM_IO);
    double *cost_per_forest =
        mymalloc_cat((size_t)nforests_this_file * sizeof(*cost_per_forest), MEM_IO);

    int file_status = read_nhalos_for_file(ifile, nforests_this_file, nhalos_per_forest);
    for (int64_t i = 0; file_status == EXIT_SUCCESS && i < nforests_this_file; i++) {
      size_per_forest[i] = (double)nhalos_per_forest[i] * (double)sizeof(struct HaloOutput);
      cost_per_forest[i] = compute_forest_cost_from_nhalos(scheme, nhalos_per_forest[i],
                                                           MimicConfig.Exponent_Forest_Dist_Scheme);
      if (cost_per_forest[i] < 0.0 || !isfinite(cost_per_forest[i])) {
        fprintf(stderr,
                "Error: invalid Consistent-Trees HDF5 forest cost %g for forest %" PRId64 "\n",
                cost_per_forest[i], CTH.first_forest_in_file[ifile] + i);
        file_status = CT_H5_ERR;
      }
    }
    if (file_status == EXIT_SUCCESS &&
        chunk_plan_builder_add_file_with_cost(&builder, nforests_this_file, size_per_forest,
                                              cost_per_forest) != 0) {
      fprintf(stderr, "Error: failed to feed file %d into Consistent-Trees HDF5 chunk planner\n",
              ifile);
      file_status = CT_H5_ERR;
    }

    myfree(cost_per_forest);
    myfree(size_per_forest);
    myfree(nhalos_per_forest);
    if (file_status != EXIT_SUCCESS) {
      chunk_plan_free(&builder.plan);
      return CT_H5_ERR;
    }
  }

  if (chunk_plan_builder_finish(&builder, &CTH.chunk_plan) != 0) {
    chunk_plan_free(&builder.plan);
    XRETURN(0, CT_H5_ERR,
            "Error: failed to build Consistent-Trees HDF5 chunk plan (target_file_size=%" PRId64
            ", forests_per_file=%" PRId64 ")\n",
            MimicConfig.TargetFileSize, MimicConfig.ForestsPerFile);
  }
  XRETURN(CTH.chunk_plan.nchunks > 0, CT_H5_ERR,
          "Error: Consistent-Trees HDF5 chunk planner emitted no chunks for %" PRId64 " forests\n",
          CTH.totnforests);
  XRETURN(CTH.chunk_plan.nchunks < INT_MAX, CT_H5_ERR,
          "Error: Consistent-Trees HDF5 chunk count %" PRId64
          " cannot be represented by the reader interface\n",
          CTH.chunk_plan.nchunks);

  CTH.chunk_costs = mymalloc_cat((size_t)CTH.chunk_plan.nchunks * sizeof(*CTH.chunk_costs), MEM_IO);
  for (int64_t chunk = 0; chunk < CTH.chunk_plan.nchunks; chunk++) {
    const struct ChunkPlanRange *range = &CTH.chunk_plan.chunks[chunk];
    CTH.chunk_costs[chunk] = range->cost;
  }

  DEBUG_LOG("Consistent-Trees HDF5 chunk plan: nchunks=%" PRId64 ", target_file_size=%" PRId64
            ", forests_per_file=%" PRId64,
            CTH.chunk_plan.nchunks, MimicConfig.TargetFileSize, MimicConfig.ForestsPerFile);
  for (int64_t chunk = 0; chunk < CTH.chunk_plan.nchunks; chunk++) {
    const struct ChunkPlanRange *range = &CTH.chunk_plan.chunks[chunk];
    DEBUG_LOG("  chunk %" PRId64 ": forests [%" PRId64 ", %" PRId64 "), size_proxy=%g, cost=%g",
              chunk, range->start_forest, range->start_forest + range->nforests, range->size,
              CTH.chunk_costs[chunk]);
  }
  return EXIT_SUCCESS;
}

static int prepare_run_ctrees_hdf5_state(void);
static int stage_range_ctrees_hdf5(int64_t start_forestnum, int64_t nforests, int thistask,
                                   int ntasks);

static void close_staged_file_groups_ctrees_hdf5(void) {
  if (CTH.h5_forests_group != NULL) {
    for (int i = 0; i < CTH.totnfiles; i++) {
      if (CTH.h5_forests_group[i] >= 0) {
        H5Gclose(CTH.h5_forests_group[i]);
      }
    }
    myfree(CTH.h5_forests_group);
    CTH.h5_forests_group = NULL;
  }
  if (CTH.h5_file_groups != NULL) {
    for (int i = 0; i < CTH.totnfiles; i++) {
      if (CTH.h5_file_groups[i] >= 0) {
        H5Gclose(CTH.h5_file_groups[i]);
      }
    }
    myfree(CTH.h5_file_groups);
    CTH.h5_file_groups = NULL;
  }
}

static void clear_staged_range_ctrees_hdf5(void) {
  free_read_window_ctrees_hdf5();
  free_field_cache_ctrees_hdf5();
  free_forestinfo_cache_ctrees_hdf5();
  close_staged_file_groups_ctrees_hdf5();

  if (CTH.contig_halo_props != NULL) {
    myfree(CTH.contig_halo_props);
    CTH.contig_halo_props = NULL;
  }
  if (CTH.forest_treenr_in_file != NULL) {
    myfree(CTH.forest_treenr_in_file);
    CTH.forest_treenr_in_file = NULL;
  }
  if (CTH.forest_filenum != NULL) {
    myfree(CTH.forest_filenum);
    CTH.forest_filenum = NULL;
  }

  CTH.start_filenum = -1;
  CTH.end_filenum = -1;
  CTH.start_forestnum = 0;
  CTH.nforests = 0;
  CTH.snap_field_name[0] = '\0';
  CTH.snap_field_is_double = 0;
}

static void teardown_run_ctrees_hdf5_state(void) {
  clear_staged_range_ctrees_hdf5();

  if (CTH.chunk_costs != NULL) {
    myfree(CTH.chunk_costs);
    CTH.chunk_costs = NULL;
  }
  chunk_plan_free(&CTH.chunk_plan);
  if (CTH.first_forest_in_file != NULL) {
    myfree(CTH.first_forest_in_file);
    CTH.first_forest_in_file = NULL;
  }
  if (CTH.nforests_per_file != NULL) {
    myfree(CTH.nforests_per_file);
    CTH.nforests_per_file = NULL;
  }
  if (CTH.meta_fd >= 0) {
    H5Fclose(CTH.meta_fd);
    CTH.meta_fd = -1;
  }
  CTH.meta_fd = -1;
  CTH.totnfiles = 0;
  CTH.firstfile = 0;
  CTH.lastfile = 0;
  CTH.totnforests = 0;
  CTH.start_filenum = -1;
  CTH.end_filenum = -1;
}

#ifdef MIMIC_TEST_BUILD
static int ctrees_hdf5_test_open_file0_forests(const char *filename, hid_t *file,
                                               hid_t *forests_group) {
  *file = H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (*file < 0) {
    return CT_H5_ERR;
  }
  *forests_group = H5Gopen(*file, "File0/Forests", H5P_DEFAULT);
  if (*forests_group < 0) {
    H5Fclose(*file);
    *file = -1;
    return CT_H5_ERR;
  }
  return EXIT_SUCCESS;
}

static int ctrees_hdf5_test_prepare_field_cache(const char *filename, const char *snap_field_name,
                                                const int8_t snap_field_is_double, hid_t *file,
                                                hid_t *forests_group) {
  memset(&CTH, 0, sizeof(CTH));
  CTH.totnfiles = 1;
  if (ctrees_hdf5_test_open_file0_forests(filename, file, forests_group) != EXIT_SUCCESS) {
    memset(&CTH, 0, sizeof(CTH));
    return CT_H5_ERR;
  }
  CTH.h5_forests_group = mymalloc_cat(sizeof(*CTH.h5_forests_group), MEM_IO);
  CTH.h5_forests_group[0] = *forests_group;
  snprintf(CTH.snap_field_name, sizeof(CTH.snap_field_name), "%s", snap_field_name);
  CTH.snap_field_is_double = snap_field_is_double;
  const int status = open_field_cache_ctrees_hdf5(0, 0);
  if (status == EXIT_SUCCESS &&
      allocate_read_window_ctrees_hdf5(CTREES_TEST_READ_WINDOW_BYTES) != EXIT_SUCCESS) {
    free_field_cache_ctrees_hdf5();
    myfree(CTH.h5_forests_group);
    H5Gclose(*forests_group);
    H5Fclose(*file);
    *forests_group = -1;
    *file = -1;
    memset(&CTH, 0, sizeof(CTH));
    return CT_H5_ERR;
  }
  if (status != EXIT_SUCCESS) {
    free_field_cache_ctrees_hdf5();
    myfree(CTH.h5_forests_group);
    H5Gclose(*forests_group);
    H5Fclose(*file);
    *forests_group = -1;
    *file = -1;
    memset(&CTH, 0, sizeof(CTH));
  }
  return status;
}

static void ctrees_hdf5_test_close_field_cache(hid_t file, hid_t forests_group) {
  free_read_window_ctrees_hdf5();
  free_field_cache_ctrees_hdf5();
  if (CTH.h5_forests_group != NULL) {
    myfree(CTH.h5_forests_group);
  }
  if (forests_group >= 0) {
    H5Gclose(forests_group);
  }
  if (file >= 0) {
    H5Fclose(file);
  }
  memset(&CTH, 0, sizeof(CTH));
}

int ctrees_hdf5_test_read_nhalos_per_forest(const char *filename, const int64_t expected_nforests,
                                            int64_t *nhalos_per_forest) {
  hid_t file = H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) {
    return CT_H5_ERR;
  }

  memset(&CTH, 0, sizeof(CTH));
  CTH.meta_fd = file;
  const int64_t totnforests_per_file[1] = {expected_nforests};
  const int status = read_nhalos_per_forest(0, 0, totnforests_per_file, nhalos_per_forest);

  H5Fclose(file);
  memset(&CTH, 0, sizeof(CTH));
  return status;
}

int ctrees_hdf5_test_read_forestinfo_cache(const char *filename, const int64_t expected_nforests,
                                           const int64_t row, int64_t *halosoffset,
                                           int64_t *nhalos) {
  hid_t file = H5Fopen(filename, H5F_ACC_RDONLY, H5P_DEFAULT);
  if (file < 0) {
    return CT_H5_ERR;
  }
  hid_t file_group = H5Gopen(file, "File0", H5P_DEFAULT);
  if (file_group < 0) {
    H5Fclose(file);
    return CT_H5_ERR;
  }

  memset(&CTH, 0, sizeof(CTH));
  CTH.totnfiles = 1;
  hid_t file_groups[1] = {file_group};
  CTH.h5_file_groups = file_groups;
  const int64_t totnforests_per_file[1] = {expected_nforests};
  int status = load_forestinfo_cache_ctrees_hdf5(0, 0, totnforests_per_file);
  if (status == EXIT_SUCCESS) {
    if (row < 0 || row >= CTH.forestinfo_cache_nrows[0]) {
      status = CT_H5_ERR;
    } else {
      *halosoffset = CTH.forestinfo_cache[0][row].foresthalosoffset;
      *nhalos = CTH.forestinfo_cache[0][row].forestnhalos;
    }
  }

  free_forestinfo_cache_ctrees_hdf5();
  H5Gclose(file_group);
  H5Fclose(file);
  memset(&CTH, 0, sizeof(CTH));
  return status;
}

int ctrees_hdf5_test_validate_forest_slab(const char *filename, const int64_t halosoffset,
                                          const int64_t nhalos) {
  hid_t file = -1;
  hid_t forests_group = -1;
  if (ctrees_hdf5_test_prepare_field_cache(filename, "Snap_idx", 0, &file, &forests_group) !=
      EXIT_SUCCESS) {
    return CT_H5_ERR;
  }
  const int status =
      validate_ctrees_hdf5_forest_slab(CTH.field_cache[0].halo_extent, halosoffset, nhalos, 0, 0);
  ctrees_hdf5_test_close_field_cache(file, forests_group);
  return status;
}

int ctrees_hdf5_test_open_field_cache(const char *filename, const char *snap_field_name,
                                      const int8_t snap_field_is_double) {
  hid_t file = -1;
  hid_t forests_group = -1;
  const int status = ctrees_hdf5_test_prepare_field_cache(
      filename, snap_field_name, snap_field_is_double, &file, &forests_group);
  if (status == EXIT_SUCCESS) {
    ctrees_hdf5_test_close_field_cache(file, forests_group);
  }
  return status;
}

int ctrees_hdf5_test_read_forest(const char *filename, const char *snap_field_name,
                                 const int8_t snap_field_is_double, const int64_t halosoffset,
                                 const int64_t nhalos, struct halo_data *halos) {
  hid_t file = -1;
  hid_t forests_group = -1;
  if (ctrees_hdf5_test_prepare_field_cache(filename, snap_field_name, snap_field_is_double, &file,
                                           &forests_group) != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }
  int status =
      validate_ctrees_hdf5_forest_slab(CTH.field_cache[0].halo_extent, halosoffset, nhalos, 0, 0);
  if (status == EXIT_SUCCESS) {
    status = read_windowed_forest_ctrees_h5(&CTH.field_cache[0], 0, (hsize_t)nhalos,
                                            (hsize_t)halosoffset, snap_field_is_double, halos);
  }
  ctrees_hdf5_test_close_field_cache(file, forests_group);
  return status;
}

int ctrees_hdf5_test_read_two_forests_windowed(
    const char *filename, const char *snap_field_name, const int8_t snap_field_is_double,
    const int64_t first_halosoffset, const int64_t first_nhalos, struct halo_data *first_halos,
    const int64_t second_halosoffset, const int64_t second_nhalos, struct halo_data *second_halos) {
  hid_t file = -1;
  hid_t forests_group = -1;
  if (ctrees_hdf5_test_prepare_field_cache(filename, snap_field_name, snap_field_is_double, &file,
                                           &forests_group) != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }
  int status = validate_ctrees_hdf5_forest_slab(CTH.field_cache[0].halo_extent, first_halosoffset,
                                                first_nhalos, 0, 0);
  if (status == EXIT_SUCCESS) {
    status = read_windowed_forest_ctrees_h5(&CTH.field_cache[0], 0, (hsize_t)first_nhalos,
                                            (hsize_t)first_halosoffset, snap_field_is_double,
                                            first_halos);
  }
  if (status == EXIT_SUCCESS) {
    status = validate_ctrees_hdf5_forest_slab(CTH.field_cache[0].halo_extent, second_halosoffset,
                                              second_nhalos, 1, 0);
  }
  if (status == EXIT_SUCCESS) {
    status = read_windowed_forest_ctrees_h5(&CTH.field_cache[0], 0, (hsize_t)second_nhalos,
                                            (hsize_t)second_halosoffset, snap_field_is_double,
                                            second_halos);
  }
  ctrees_hdf5_test_close_field_cache(file, forests_group);
  return status;
}

static void ctrees_hdf5_test_release_partition_globals(void) {
  if (InputTreeFirstHalo != NULL) {
    myfree(InputTreeFirstHalo);
    InputTreeFirstHalo = NULL;
  }
  if (InputTreeNHalos != NULL) {
    myfree(InputTreeNHalos);
    InputTreeNHalos = NULL;
  }
}

static void ctrees_hdf5_test_fill_stage_probe(struct ctrees_hdf5_test_stage_probe *probe) {
  memset(probe, 0, sizeof(*probe));
  probe->ntrees = Ntrees;
  probe->start_filenum = CTH.start_filenum;
  probe->end_filenum = CTH.end_filenum;
  probe->global_forest_offset = GlobalForestOffset;
  probe->run_counts_available =
      CTH.nforests_per_file != NULL && CTH.nforests_per_file[CTH.firstfile] > 0;
  if (CTH.nforests > 0) {
    const int first_unit_filenum = CTH.forest_filenum[0];
    probe->first_unit_filenum = first_unit_filenum;
    probe->first_unit_treenr_in_file = CTH.forest_treenr_in_file[0];
    probe->has_active_field_cache =
        CTH.field_cache != NULL && CTH.field_cache[first_unit_filenum].is_open;
  } else {
    probe->first_unit_filenum = -1;
    probe->first_unit_treenr_in_file = -1;
  }
  probe->stale_file0_cache_present =
      CTH.field_cache != NULL && CTH.field_cache[0].is_open && CTH.start_filenum != 0;
}

int ctrees_hdf5_test_prepare_and_stage_ranges(const char *simulation_dir, const char *tree_name,
                                              const int64_t starts[2], const int64_t counts[2],
                                              struct ctrees_hdf5_test_stage_probe probes[2]) {
  memset(&CTH, 0, sizeof(CTH));
  CTH.meta_fd = -1;
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s", simulation_dir);
  snprintf(MimicConfig.TreeName, sizeof(MimicConfig.TreeName), "%s", tree_name);
  MimicConfig.FirstFile = 0;
  MimicConfig.LastFile = 2;
  MimicConfig.TargetFileSize = 1024;
  MimicConfig.ForestsPerFile = 0;
  MimicConfig.ForestDistributionScheme = uniform_in_forests;
  MimicConfig.Exponent_Forest_Dist_Scheme = 0.0;

  if (prepare_run_ctrees_hdf5_state() != EXIT_SUCCESS) {
    teardown_run_ctrees_hdf5_state();
    return CT_H5_ERR;
  }

  int status = EXIT_SUCCESS;
  for (int i = 0; i < 2; i++) {
    if (stage_range_ctrees_hdf5(starts[i], counts[i], 0, 1) != EXIT_SUCCESS) {
      status = CT_H5_ERR;
      break;
    }
    ctrees_hdf5_test_fill_stage_probe(&probes[i]);
    clear_staged_range_ctrees_hdf5();
    ctrees_hdf5_test_release_partition_globals();
  }

  teardown_run_ctrees_hdf5_state();
  ctrees_hdf5_test_release_partition_globals();
  return status;
}

int ctrees_hdf5_test_prepare_chunk_plan(const char *simulation_dir, const char *tree_name,
                                        const int64_t forests_per_file,
                                        const int64_t target_file_size, const int max_chunks,
                                        int64_t *starts, int64_t *counts, double *costs,
                                        int *nchunks) {
  memset(&CTH, 0, sizeof(CTH));
  CTH.meta_fd = -1;
  snprintf(MimicConfig.SimulationDir, sizeof(MimicConfig.SimulationDir), "%s", simulation_dir);
  snprintf(MimicConfig.TreeName, sizeof(MimicConfig.TreeName), "%s", tree_name);
  MimicConfig.FirstFile = 0;
  MimicConfig.LastFile = 2;
  MimicConfig.TargetFileSize = target_file_size;
  MimicConfig.ForestsPerFile = forests_per_file;
  MimicConfig.ForestDistributionScheme = linear_in_nhalos;
  MimicConfig.Exponent_Forest_Dist_Scheme = 0.0;

  if (prepare_run_ctrees_hdf5_state() != EXIT_SUCCESS) {
    teardown_run_ctrees_hdf5_state();
    return CT_H5_ERR;
  }

  if (nchunks != NULL) {
    *nchunks = (int)CTH.chunk_plan.nchunks;
  }
  const int limit =
      max_chunks < (int)CTH.chunk_plan.nchunks ? max_chunks : (int)CTH.chunk_plan.nchunks;
  for (int i = 0; i < limit; i++) {
    starts[i] = CTH.chunk_plan.chunks[i].start_forest;
    counts[i] = CTH.chunk_plan.chunks[i].nforests;
    costs[i] = CTH.chunk_costs[i];
  }

  teardown_run_ctrees_hdf5_state();
  return EXIT_SUCCESS;
}

int ctrees_hdf5_test_rejects_oversized_stage_range(void) {
  int64_t nforests_per_file[1] = {(int64_t)INT_MAX + 1};

  memset(&CTH, 0, sizeof(CTH));
  CTH.meta_fd = 0;
  CTH.nforests_per_file = nforests_per_file;
  CTH.totnforests = (int64_t)INT_MAX + 1;

  const int status = stage_range_ctrees_hdf5(0, (int64_t)INT_MAX + 1, 0, 1);

  CTH.nforests_per_file = NULL;
  CTH.meta_fd = -1;
  clear_staged_range_ctrees_hdf5();
  memset(&CTH, 0, sizeof(CTH));
  return status != EXIT_SUCCESS ? EXIT_SUCCESS : CT_H5_ERR;
}
#endif /* MIMIC_TEST_BUILD */

/* Detect the snapshot-number field and its on-disk type. Older Consistent-Trees
   writes "Snap_num" (integer); newer (Uchuu) writes "Snap_idx" (integer, or
   double if the converter mis-typed it). Mirrors sage's detection. */
static int detect_snap_field(hid_t h5_forests_group) {
  snprintf(CTH.snap_field_name, sizeof(CTH.snap_field_name), "Snap_num");
  if (H5Lexists(h5_forests_group, CTH.snap_field_name, H5P_DEFAULT) <= 0) {
    snprintf(CTH.snap_field_name, sizeof(CTH.snap_field_name), "Snap_idx");
    XRETURN(H5Lexists(h5_forests_group, CTH.snap_field_name, H5P_DEFAULT) > 0, CT_H5_ERR,
            "Error: Could not find the snapshot field as 'Snap_num' or 'Snap_idx'\n");
  }
  hid_t snap_dset = H5Dopen2(h5_forests_group, CTH.snap_field_name, H5P_DEFAULT);
  XRETURN(snap_dset >= 0, CT_H5_ERR, "Error: Could not open snapshot dataset '%s'\n",
          CTH.snap_field_name);
  const hid_t snap_dtype = H5Dget_type(snap_dset);
  XRETURN(snap_dtype >= 0, CT_H5_ERR, "Error: Could not get datatype for '%s'\n",
          CTH.snap_field_name);
  const H5T_class_t cls = H5Tget_class(snap_dtype);
  if (cls == H5T_INTEGER) {
    CTH.snap_field_is_double = 0;
  } else if (cls == H5T_FLOAT) {
    CTH.snap_field_is_double = 1;
  } else {
    XRETURN(0, CT_H5_ERR, "Error: snapshot field '%s' is neither integer nor float\n",
            CTH.snap_field_name);
  }
  XRETURN(H5Dclose(snap_dset) >= 0, CT_H5_ERR, "Error: Could not close snapshot dataset\n");
  XRETURN(H5Tclose(snap_dtype) >= 0, CT_H5_ERR, "Error: Could not close snapshot datatype\n");
  return EXIT_SUCCESS;
}

/* Read run-scoped metadata and distribution weights once for the whole driver run. */
static int prepare_run_ctrees_hdf5_state(void) {
  const int firstfile = MimicConfig.FirstFile;
  const int lastfile = MimicConfig.LastFile;
  const int numfiles = lastfile - firstfile + 1;
  XRETURN(firstfile >= 0 && lastfile >= firstfile, CT_H5_ERR,
          "Error: invalid file range [first_file=%d, last_file=%d]; need 0 <= first_file <= "
          "last_file\n",
          firstfile, lastfile);

  char metadata_fname[3 * MAX_STRING_LEN + 2];
  int meta_path_len = snprintf(metadata_fname, sizeof(metadata_fname), "%s/%s",
                               MimicConfig.SimulationDir, MimicConfig.TreeName);
  if (meta_path_len < 0 || (size_t)meta_path_len >= sizeof(metadata_fname)) {
    FATAL_ERROR("Metadata file path too long (%d chars, max %zu)", meta_path_len,
                sizeof(metadata_fname) - 1);
  }
  CTH.meta_fd = H5Fopen(metadata_fname, H5F_ACC_RDONLY, H5P_DEFAULT);
  XRETURN(CTH.meta_fd >= 0, CT_H5_ERR, "Error: Could not open metadata file '%s'\n",
          metadata_fname);

  int64_t check_totnfiles;
  if (ct_read_attribute(CTH.meta_fd, "/", "Nfiles", &check_totnfiles, sizeof(check_totnfiles)) !=
      EXIT_SUCCESS) {
    return CT_H5_ERR;
  }
  XRETURN(numfiles <= check_totnfiles && (int64_t)lastfile < check_totnfiles, CT_H5_ERR,
          "Error: requested files [%d, %d] (%d files) but the dataset only has %" PRId64
          " files (valid indices 0..%" PRId64 ")\n",
          firstfile, lastfile, numfiles, check_totnfiles, check_totnfiles - 1);

  const int64_t totnfiles = lastfile + 1; /* wastes [0, firstfile) but lets file number index */
  CTH.totnfiles = (int)totnfiles;
  CTH.firstfile = firstfile;
  CTH.lastfile = lastfile;
  CTH.nforests_per_file = mymalloc_cat(totnfiles * sizeof(*CTH.nforests_per_file), MEM_IO);
  CTH.first_forest_in_file = mymalloc_cat(totnfiles * sizeof(*CTH.first_forest_in_file), MEM_IO);
  for (int64_t i = 0; i < totnfiles; i++) {
    CTH.nforests_per_file[i] = 0;
    CTH.first_forest_in_file[i] = 0;
  }
  const int64_t max_unique_id_forests = mimic_unique_galaxy_id_max_forests();
  int64_t totnforests = 0;
  for (int ifile = firstfile; ifile <= lastfile; ifile++) {
    char file_group_name[MAX_STRING_LEN];
    snprintf(file_group_name, sizeof(file_group_name), "File%d", ifile);
    int64_t nforests_this_file;
    if (ct_read_attribute(CTH.meta_fd, file_group_name, "Nforests", &nforests_this_file,
                          sizeof(nforests_this_file)) != EXIT_SUCCESS) {
      return CT_H5_ERR;
    }
    XRETURN(nforests_this_file >= 1, CT_H5_ERR,
            "Error: file %d reports %" PRId64 " forests (expected >= 1)\n", ifile,
            nforests_this_file);
    XRETURN(nforests_this_file <= LLONG_MAX - totnforests, CT_H5_ERR,
            "Error: Consistent-Trees total forest count would overflow int64 after file %d\n",
            ifile);
    CTH.first_forest_in_file[ifile] = totnforests;
    CTH.nforests_per_file[ifile] = nforests_this_file;
    totnforests += nforests_this_file;
  }
  CTH.totnforests = totnforests;
  XRETURN(totnforests >= 1, CT_H5_ERR, "Error: total forest count %" PRId64 " must be >= 1\n",
          totnforests);
  XRETURN(mimic_unique_galaxy_id_total_forests_valid(totnforests), CT_H5_ERR,
          "Error: Consistent-Trees total forest count %" PRId64
          " exceeds the UniqueGalaxyID encoding limit of %" PRId64 "\n",
          totnforests, max_unique_id_forests);

  if (build_chunk_plan_ctrees_hdf5() != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }
  return EXIT_SUCCESS;
}

static int open_staged_file_groups_ctrees_hdf5(const int start_filenum, const int end_filenum) {
  CTH.h5_file_groups = mymalloc_cat(CTH.totnfiles * sizeof(*CTH.h5_file_groups), MEM_IO);
  CTH.h5_forests_group = mymalloc_cat(CTH.totnfiles * sizeof(*CTH.h5_forests_group), MEM_IO);
  CTH.contig_halo_props = mymalloc_cat(CTH.totnfiles * sizeof(*CTH.contig_halo_props), MEM_IO);
  for (int i = 0; i < CTH.totnfiles; i++) {
    CTH.h5_file_groups[i] = -1;
    CTH.h5_forests_group[i] = -1;
    CTH.contig_halo_props[i] = 0;
  }

  for (int ifile = start_filenum; ifile <= end_filenum; ifile++) {
    char file_group_name[MAX_STRING_LEN];
    snprintf(file_group_name, sizeof(file_group_name), "File%d", ifile);
    hid_t h5_file_group = H5Gopen(CTH.meta_fd, file_group_name, H5P_DEFAULT);
    XRETURN(h5_file_group >= 0, CT_H5_ERR, "Error: Could not open file group '%s'\n",
            file_group_name);
    CTH.h5_file_groups[ifile] = h5_file_group;
    hid_t h5_forest_group = H5Gopen(h5_file_group, "Forests", H5P_DEFAULT);
    XRETURN(h5_forest_group >= 0, CT_H5_ERR, "Error: Could not open 'Forests' group in '%s'\n",
            file_group_name);
    CTH.h5_forests_group[ifile] = h5_forest_group;
  }
  return EXIT_SUCCESS;
}

/* Stage one forest range after prepare_run has loaded run-scoped metadata. */
static int stage_range_ctrees_hdf5(const int64_t start_forestnum, const int64_t nforests,
                                   const int thistask, const int ntasks) {
  clear_staged_range_ctrees_hdf5();

  XRETURN(CTH.meta_fd >= 0 && CTH.nforests_per_file != NULL, CT_H5_ERR,
          "Error: Consistent-Trees HDF5 reader was not prepared before staging\n");
  XRETURN(nforests >= 0 && start_forestnum >= 0 && start_forestnum <= CTH.totnforests - nforests,
          CT_H5_ERR,
          "Error: requested forest range [%" PRId64 ", %" PRId64 ") outside [0, %" PRId64 ")\n",
          start_forestnum, start_forestnum + nforests, CTH.totnforests);
  XRETURN(nforests <= INT_MAX, CT_H5_ERR,
          "Error: Consistent-Trees HDF5 chunk has %" PRId64
          " forests, exceeding the 32-bit per-partition limit\n",
          nforests);

  CTH.start_forestnum = start_forestnum;
  GlobalForestOffset = CTH.start_forestnum;
  CTH.nforests = nforests;
  Ntrees = (int)nforests;

  CTH.forest_filenum =
      mymalloc_cat((nforests > 0 ? nforests : 1) * sizeof(*CTH.forest_filenum), MEM_IO);
  CTH.forest_treenr_in_file =
      mymalloc_cat((nforests > 0 ? nforests : 1) * sizeof(*CTH.forest_treenr_in_file), MEM_IO);
  InputTreeNHalos = mymalloc_cat((nforests > 0 ? nforests : 1) * sizeof(int), MEM_TREES);
  InputTreeFirstHalo = mymalloc_cat((nforests > 0 ? nforests : 1) * sizeof(int), MEM_TREES);
  for (int64_t i = 0; i < nforests; i++) {
    InputTreeNHalos[i] = 0;    /* filled per forest in load_unit */
    InputTreeFirstHalo[i] = 0; /* each forest loads into a fresh InputTreeHalos */
  }

  /* No forests for this range: nothing more to set up. */
  if (nforests == 0) {
    CTH.start_filenum = CTH.firstfile;
    CTH.end_filenum = CTH.firstfile;
    return EXIT_SUCCESS;
  }

  const int64_t end_forestnum = start_forestnum + nforests; /* exclusive */
  int64_t *num_forests_to_process_per_file =
      mymalloc_cat(CTH.totnfiles * sizeof(*num_forests_to_process_per_file), MEM_IO);
  int64_t *start_forestnum_to_process_per_file =
      mymalloc_cat(CTH.totnfiles * sizeof(*start_forestnum_to_process_per_file), MEM_IO);
  for (int64_t i = 0; i < CTH.totnfiles; i++) {
    num_forests_to_process_per_file[i] = 0;
    start_forestnum_to_process_per_file[i] = 0;
  }

  int start_filenum = -1, end_filenum = -1;
  if (find_start_and_end_filenum(
          start_forestnum, end_forestnum, CTH.nforests_per_file, CTH.totnforests, CTH.firstfile,
          CTH.lastfile, thistask, ntasks, num_forests_to_process_per_file,
          start_forestnum_to_process_per_file, &start_filenum, &end_filenum) != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }
  CTH.start_filenum = start_filenum;
  CTH.end_filenum = end_filenum;

  if (open_staged_file_groups_ctrees_hdf5(start_filenum, end_filenum) != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }

  /* Map each chunk-local forest to its file and the tree row within that file. */
  int curr_filenum = start_filenum;
  int64_t end_forestnum_in_currfile =
      CTH.nforests_per_file[start_filenum] - start_forestnum_to_process_per_file[start_filenum];
  int64_t offset = 0;
  for (int64_t iforest = 0; iforest < nforests; iforest++) {
    if (iforest >= end_forestnum_in_currfile) {
      offset = end_forestnum_in_currfile;
      curr_filenum++;
      end_forestnum_in_currfile += CTH.nforests_per_file[curr_filenum];
    }
    CTH.forest_filenum[iforest] = curr_filenum;
    if (curr_filenum == start_filenum) {
      CTH.forest_treenr_in_file[iforest] =
          iforest + start_forestnum_to_process_per_file[curr_filenum];
    } else {
      CTH.forest_treenr_in_file[iforest] = iforest - offset;
    }
  }

  if (load_forestinfo_cache_ctrees_hdf5(start_filenum, end_filenum, CTH.nforests_per_file) !=
      EXIT_SUCCESS) {
    return CT_H5_ERR;
  }

  myfree(start_forestnum_to_process_per_file);
  myfree(num_forests_to_process_per_file);

  /* Verify the per-file cosmology against the compiled simulation package and
     confirm the (only supported) contiguous halo storage. */
  const double maxdiff = 1e-8, maxreldiff = 1e-5;
  for (int ifile = start_filenum; ifile <= end_filenum; ifile++) {
    char file_group_name[MAX_STRING_LEN];
    snprintf(file_group_name, sizeof(file_group_name), "File%d", ifile);
    int8_t contig_halo_props;
    if (ct_read_attribute(CTH.meta_fd, file_group_name, "contiguous-halo-props", &contig_halo_props,
                          sizeof(contig_halo_props)) != EXIT_SUCCESS) {
      return CT_H5_ERR;
    }
    CTH.contig_halo_props[ifile] = contig_halo_props;
    XRETURN(contig_halo_props, CT_H5_ERR,
            "Error: Consistent-Trees HDF5 array-of-structs (non-contiguous) layout in file %d is "
            "not supported\n",
            ifile);

    double om, ol, little_h, file_boxsize;
    if (ct_read_attribute(CTH.h5_file_groups[ifile], "simulation_params", "Omega_M", &om,
                          sizeof(om)) != EXIT_SUCCESS ||
        ct_read_attribute(CTH.h5_file_groups[ifile], "simulation_params", "Omega_L", &ol,
                          sizeof(ol)) != EXIT_SUCCESS ||
        ct_read_attribute(CTH.h5_file_groups[ifile], "simulation_params", "hubble", &little_h,
                          sizeof(little_h)) != EXIT_SUCCESS ||
        ct_read_attribute(CTH.h5_file_groups[ifile], "simulation_params", "Boxsize", &file_boxsize,
                          sizeof(file_boxsize)) != EXIT_SUCCESS) {
      return CT_H5_ERR;
    }
    XRETURN(ct_almost_equal(file_boxsize, MimicConfig.BoxSize, maxdiff, maxreldiff), CT_H5_ERR,
            "Error: file BoxSize %g differs from the simulation package value %g\n", file_boxsize,
            MimicConfig.BoxSize);
    XRETURN(ct_almost_equal(om, MimicConfig.Omega, maxdiff, maxreldiff), CT_H5_ERR,
            "Error: file Omega_M %g differs from the simulation package value %g\n", om,
            MimicConfig.Omega);
    XRETURN(ct_almost_equal(ol, MimicConfig.OmegaLambda, maxdiff, maxreldiff), CT_H5_ERR,
            "Error: file Omega_L %g differs from the simulation package value %g\n", ol,
            MimicConfig.OmegaLambda);
    XRETURN(ct_almost_equal(little_h, MimicConfig.Hubble_h, maxdiff, maxreldiff), CT_H5_ERR,
            "Error: file hubble %g differs from the simulation package value %g\n", little_h,
            MimicConfig.Hubble_h);
  }

  if (detect_snap_field(CTH.h5_forests_group[start_filenum]) != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }
  if (open_field_cache_ctrees_hdf5(start_filenum, end_filenum) != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }
  if (allocate_read_window_ctrees_hdf5(CTREES_READ_WINDOW_BYTES) != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }

  return EXIT_SUCCESS;
}

static void prepare_run_ctrees_hdf5(void) {
  memset(&CTH, 0, sizeof(CTH));
  CTH.meta_fd = -1;
  CTH.start_filenum = -1;
  CTH.end_filenum = -1;

  if (prepare_run_ctrees_hdf5_state() != EXIT_SUCCESS) {
    teardown_run_ctrees_hdf5_state();
    FATAL_ERROR("Failed to prepare the Consistent-Trees HDF5 reader");
  }
}

static int num_partitions_ctrees_hdf5(void) { return (int)CTH.chunk_plan.nchunks; }

static int partition_output_id_ctrees_hdf5(int partition) { return partition; }

static int partition_exists_ctrees_hdf5(int partition) {
  return partition >= 0 && (int64_t)partition < CTH.chunk_plan.nchunks;
}

static int64_t count_partition_units_ctrees_hdf5(int partition) {
  if (!partition_exists_ctrees_hdf5(partition)) {
    return -1;
  }
  return CTH.chunk_plan.chunks[partition].nforests;
}

static int64_t global_forest_offset_ctrees_hdf5(int partition) {
  if (!partition_exists_ctrees_hdf5(partition)) {
    FATAL_ERROR("Consistent-Trees HDF5: chunk id %d is outside [0, %" PRId64 ")", partition,
                CTH.chunk_plan.nchunks);
  }
  return CTH.chunk_plan.chunks[partition].start_forest;
}

static double partition_cost_ctrees_hdf5(int partition) {
  if (!partition_exists_ctrees_hdf5(partition)) {
    FATAL_ERROR("Consistent-Trees HDF5: chunk id %d is outside [0, %" PRId64 ")", partition,
                CTH.chunk_plan.nchunks);
  }
  return CTH.chunk_costs[partition];
}

/**
 * @brief   Open one chunk partition and stage its forest range.
 * @param   output_id   The global chunk id (also the output file id).
 */
static void open_partition_ctrees_hdf5(int output_id) {
  if (!partition_exists_ctrees_hdf5(output_id)) {
    FATAL_ERROR("Consistent-Trees HDF5: chunk id %d is outside [0, %" PRId64 ")", output_id,
                CTH.chunk_plan.nchunks);
  }

  const struct ChunkPlanRange *range = &CTH.chunk_plan.chunks[output_id];
  if (range->nforests > INT_MAX) {
    FATAL_ERROR("Consistent-Trees HDF5 chunk %d has %" PRId64
                " forests, exceeding the 32-bit per-partition limit",
                output_id, range->nforests);
  }

  const int thistask = ThisTask;
  const int ntasks = (NTask > 0) ? NTask : 1;
  if (stage_range_ctrees_hdf5(range->start_forest, range->nforests, thistask, ntasks) !=
      EXIT_SUCCESS) {
    clear_staged_range_ctrees_hdf5();
    FATAL_ERROR("Failed to set up Consistent-Trees HDF5 chunk %d on task %d", output_id, thistask);
  }
}

/**
 * @brief   Load one forest (unit) into InputTreeHalos as RawHalo records.
 */
static void load_unit_ctrees_hdf5(int unit) {
  if (unit < 0 || (int64_t)unit >= CTH.nforests) {
    FATAL_ERROR("Consistent-Trees HDF5: forest index %d out of range [0, %" PRId64 ")", unit,
                CTH.nforests);
  }

  const int32_t filenum = CTH.forest_filenum[unit];
  const int64_t treenr_in_file = CTH.forest_treenr_in_file[unit];
  if (filenum < CTH.start_filenum || filenum > CTH.end_filenum) {
    FATAL_ERROR("Consistent-Trees HDF5: forest %d maps to file %d outside [%d, %d]", unit, filenum,
                CTH.start_filenum, CTH.end_filenum);
  }

  if (treenr_in_file < 0) {
    FATAL_ERROR("Consistent-Trees HDF5: forest %d maps to negative row %" PRId64 " in file %d",
                unit, treenr_in_file, filenum);
  }
  if (CTH.forestinfo_cache == NULL || CTH.forestinfo_cache_nrows == NULL ||
      CTH.forestinfo_cache[filenum] == NULL) {
    FATAL_ERROR("Consistent-Trees HDF5: missing cached 'ForestInfo' rows for file %d", filenum);
  }
  if (treenr_in_file >= CTH.forestinfo_cache_nrows[filenum]) {
    FATAL_ERROR("Consistent-Trees HDF5: forest %d maps to row %" PRId64
                " outside cached 'ForestInfo' rows [0, %" PRId64 ") in file %d",
                unit, treenr_in_file, CTH.forestinfo_cache_nrows[filenum], filenum);
  }

  const struct ctrees_forestinfo *finfo = &CTH.forestinfo_cache[filenum][treenr_in_file];
  const int64_t halosoffset = finfo->foresthalosoffset;
  const int64_t nhalos = finfo->forestnhalos;
  if (CTH.field_cache == NULL || !CTH.field_cache[filenum].is_open) {
    FATAL_ERROR("Consistent-Trees HDF5: missing cached field handles for file %d", filenum);
  }
  struct ctrees_hdf5_field_cache *field_cache = &CTH.field_cache[filenum];
  if (validate_ctrees_hdf5_forest_slab(field_cache->halo_extent, halosoffset, nhalos, unit,
                                       filenum) != EXIT_SUCCESS) {
    FATAL_ERROR("Consistent-Trees HDF5: invalid forest %d metadata (nhalos=%" PRId64
                ", offset=%" PRId64 ") in file %d",
                unit, nhalos, halosoffset, filenum);
  }

  struct halo_data *halos =
      mymalloc_cat(sizeof(struct halo_data) * (nhalos > 0 ? nhalos : 1), MEM_TREES);
  if (nhalos > 0 &&
      read_windowed_forest_ctrees_h5(field_cache, filenum, (hsize_t)nhalos, (hsize_t)halosoffset,
                                     CTH.snap_field_is_double, halos) != EXIT_SUCCESS) {
    FATAL_ERROR("Consistent-Trees HDF5: could not read forest %d (nhalos=%" PRId64
                ", offset=%" PRId64 ") from file %d",
                unit, nhalos, halosoffset, filenum);
  }

  /* Pointers and id are already in-file; only the native-Mvir conventions remain. */
  apply_ctrees_value_conventions(halos, nhalos);

  InputTreeNHalos[unit] = (int)nhalos;
  InputTreeHalos = mymalloc_cat(sizeof(struct RawHalo) * (nhalos > 0 ? nhalos : 1), MEM_TREES);
  for (int64_t i = 0; i < nhalos; i++) {
    bridge_halo_data_to_rawhalo(&InputTreeHalos[i], &halos[i]);
  }
  myfree(halos);
}

/** @brief Close this chunk partition: close groups/file, free scaffolding. */
static void close_partition_ctrees_hdf5(void) { clear_staged_range_ctrees_hdf5(); }

static void teardown_run_ctrees_hdf5(void) { teardown_run_ctrees_hdf5_state(); }

/* Consistent-Trees forests-HDF5: forest-organised, merger pointers in-file. One
   output partition per planned chunk, one unit per forest; the driver assigns
   chunks to MPI tasks using the published LPT costs. */
const struct TreeReader CTreesHDF5Reader = {
    .name = "consistent_trees_hdf5",
    .file_extension = "",
    .partition_model = PARTITION_ENUMERATED,
    .processing_order = INPUT_PROCESSING_ORDER_TREE,
    .prepare_run = prepare_run_ctrees_hdf5,
    .teardown_run = teardown_run_ctrees_hdf5,
    .num_partitions = num_partitions_ctrees_hdf5,
    .partition_output_id = partition_output_id_ctrees_hdf5,
    .partition_exists = partition_exists_ctrees_hdf5,
    .format_partition_path = NULL,
    .count_partition_units = count_partition_units_ctrees_hdf5,
    .global_forest_offset = global_forest_offset_ctrees_hdf5,
    .partition_cost = partition_cost_ctrees_hdf5,
    .open_partition = open_partition_ctrees_hdf5,
    .load_unit = load_unit_ctrees_hdf5,
    .close_partition = close_partition_ctrees_hdf5,
};

#endif /* HDF5 */
