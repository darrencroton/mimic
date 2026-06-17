/**
 * @file    tree/read_ctrees_hdf5.c
 * @brief   Consistent-Trees forests-HDF5 merger-tree reader (HDF5 builds only).
 *
 * Reads the "forests-HDF5" packaging of Consistent-Trees output (e.g. the Uchuu
 * trees produced by uchuutools) and presents it to the core as the
 * partition/unit model: one partition per MPI task, one unit per forest. Unlike
 * the ASCII reader, the merger-tree pointers are already stored in the file, so
 * there is no topology reconstruction — this reader maps a per-task forest range
 * onto the input files, reads each forest's contiguous halo slab, applies the
 * shared Consistent-Trees -> L-Halo value conventions, and bridges the loaded
 * `struct halo_data` into the generated per-simulation `struct RawHalo`.
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
#include "tree/read_ctrees_common.h"
#include "tree/read_ctrees_hdf5.h"
#include "tree/reader.h"

/* Single error sentinel for the read helpers; every failure is surfaced as a
   FATAL_ERROR at the void open/load/close seam, so the specific value carries no
   meaning beyond "negative == failed". */
#define CT_H5_ERR (-1)

/* One row of the per-file "ForestInfo" compound dataset. Field order/types match
   the forests-HDF5 layout written by uchuutools (see sage-model). */
struct ctrees_forestinfo {
  int64_t forestid;
  int64_t foresthalosoffset;
  int64_t forestnhalos;
  int64_t forestntrees;
};

/* The one open partition (this task's forest chunk). One reader instance per
   process, so a file-static record is sufficient (mirrors the other readers). */
struct ctrees_hdf5_partition {
  hid_t meta_fd;             /* the forests-HDF5 metadata/data file */
  hid_t *h5_file_groups;     /* [totnfiles] "File%d" groups (per-file) */
  hid_t *h5_forests_group;   /* [totnfiles] "File%d/Forests" groups */
  int8_t *contig_halo_props; /* [totnfiles] 1 = halos stored contiguously (SOA) */
  int totnfiles;             /* lastfile + 1 (indexable by file number) */
  int start_filenum;         /* first/last file this task reads from */
  int end_filenum;
  int64_t nforests;                     /* units (forests) on this task */
  int32_t *forest_filenum;              /* [nforests] file holding each forest */
  int64_t *forest_treenr_in_file;       /* [nforests] tree row of each forest in its file */
  char snap_field_name[MAX_STRING_LEN]; /* "Snap_num" (older) or "Snap_idx" (newer) */
  int8_t snap_field_is_double;          /* 1 if the snap field is stored as float */
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

/* Read a contiguous hyperslab of one forests-group dataset into a flat buffer,
   verifying the on-disk element size matches the destination type. Adapted from
   sage's READ_PARTIAL_FOREST_ARRAY with a single error sentinel. */
#define CT_READ_FOREST_ARRAY(file_group, field_name, p_offset, p_count, buffer, dst_type)          \
  {                                                                                                \
    hid_t h5_dset = H5Dopen2(file_group, field_name, H5P_DEFAULT);                                 \
    XRETURN(h5_dset >= 0, CT_H5_ERR, "Error: Could not open dataset '%s'\n", field_name);          \
    hid_t h5_fspace = H5Dget_space(h5_dset);                                                       \
    XRETURN(h5_fspace >= 0, CT_H5_ERR, "Error: Could not get filespace for '%s'\n", field_name);   \
    herr_t macro_status =                                                                          \
        H5Sselect_hyperslab(h5_fspace, H5S_SELECT_SET, p_offset, NULL, p_count, NULL);             \
    XRETURN(macro_status >= 0, CT_H5_ERR, "Error: Could not select hyperslab for '%s'\n",          \
            field_name);                                                                           \
    hid_t h5_memspace = H5Screate_simple(1, p_count, NULL);                                        \
    XRETURN(h5_memspace >= 0, CT_H5_ERR, "Error: Could not create memspace for '%s'\n",            \
            field_name);                                                                           \
    const hid_t h5_dtype = H5Dget_type(h5_dset);                                                   \
    XRETURN(h5_dtype >= 0, CT_H5_ERR, "Error: Could not get datatype for '%s'\n", field_name);     \
    XRETURN(sizeof(dst_type) == H5Tget_size(h5_dtype), CT_H5_ERR,                                  \
            "Error: dataset '%s' is %zu bytes on disk but the destination is %zu bytes\n",         \
            field_name, H5Tget_size(h5_dtype), sizeof(dst_type));                                  \
    macro_status = H5Dread(h5_dset, h5_dtype, h5_memspace, h5_fspace, H5P_DEFAULT, buffer);        \
    XRETURN(macro_status >= 0, CT_H5_ERR, "Error: Could not read dataset '%s'\n", field_name);     \
    XRETURN(H5Dclose(h5_dset) >= 0, CT_H5_ERR, "Error: Could not close dataset '%s'\n",            \
            field_name);                                                                           \
    XRETURN(H5Tclose(h5_dtype) >= 0, CT_H5_ERR, "Error: Could not close datatype for '%s'\n",      \
            field_name);                                                                           \
    XRETURN(H5Sclose(h5_fspace) >= 0, CT_H5_ERR, "Error: Could not close filespace for '%s'\n",    \
            field_name);                                                                           \
    XRETURN(H5Sclose(h5_memspace) >= 0, CT_H5_ERR, "Error: Could not close memspace for '%s'\n",   \
            field_name);                                                                           \
  }

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

#define CT_READ_ASSIGN_SINGLE(fg, fn, off, cnt, buf, bdt, dst, field)                              \
  {                                                                                                \
    CT_READ_FOREST_ARRAY(fg, fn, off, cnt, buf, bdt);                                              \
    CT_ASSIGN_SINGLE(buf, bdt, dst, field);                                                        \
  }

#define CT_READ_ASSIGN_MULTI(fg, fn, off, cnt, buf, bdt, dst, field, dim)                          \
  {                                                                                                \
    CT_READ_FOREST_ARRAY(fg, fn, off, cnt, buf, bdt);                                              \
    CT_ASSIGN_MULTI(buf, bdt, dst, field, dim);                                                    \
  }

/* Read one int64 merger-link field and narrow it into halo_data's int field,
   validating each value is the no-link sentinel (-1) or a forest-local index in
   [0, nhalos). The core uses these links as array indices during traversal, so a
   wrapped or out-of-range value from a malformed/schema-mismatched file would be
   an out-of-bounds access; fail fast instead of narrowing blindly. */
#define CT_READ_ASSIGN_LINK(fg, fn, off, cnt, buf, dst, field)                                     \
  {                                                                                                \
    CT_READ_FOREST_ARRAY(fg, fn, off, cnt, buf, int64_t);                                          \
    int64_t *macro_x = (int64_t *)buf;                                                             \
    for (hsize_t mi = 0; mi < nhalos; mi++) {                                                      \
      const int64_t macro_v = macro_x[mi];                                                         \
      XRETURN(macro_v >= -1 && macro_v < (int64_t)nhalos, CT_H5_ERR,                               \
              "Error: merger link '%s'[%llu] = %lld is outside [-1, %llu)\n", fn,                  \
              (unsigned long long)mi, (long long)macro_v, (unsigned long long)nhalos);             \
      dst[mi].field = (int)macro_v;                                                                \
    }                                                                                              \
  }

/* Read one forest's contiguous (SOA) halo slab into `halos`. Only the fields the
   bridge consumes are read (the array-of-structs packaging is not supported, as
   in sage). The five merger links are read int64 and validated forest-local
   before narrowing to halo_data's int fields (see CT_READ_ASSIGN_LINK). */
static int read_contiguous_forest_ctrees_h5(hid_t h5_forests_group, const hsize_t nhalos,
                                            const hsize_t halosoffset, const char *snap_field_name,
                                            const int8_t snap_field_is_double,
                                            struct halo_data *halos) {
  void *buffer = mymalloc_cat(nhalos * sizeof(double), MEM_IO); /* double is the widest field */

  CT_READ_ASSIGN_LINK(h5_forests_group, "Descendant", &halosoffset, &nhalos, buffer, halos,
                      Descendant);
  CT_READ_ASSIGN_LINK(h5_forests_group, "FirstProgenitor", &halosoffset, &nhalos, buffer, halos,
                      FirstProgenitor);
  CT_READ_ASSIGN_LINK(h5_forests_group, "NextProgenitor", &halosoffset, &nhalos, buffer, halos,
                      NextProgenitor);
  CT_READ_ASSIGN_LINK(h5_forests_group, "FirstHaloInFOFgroup", &halosoffset, &nhalos, buffer, halos,
                      FirstHaloInFOFgroup);
  CT_READ_ASSIGN_LINK(h5_forests_group, "NextHaloInFOFgroup", &halosoffset, &nhalos, buffer, halos,
                      NextHaloInFOFgroup);

  CT_READ_ASSIGN_SINGLE(h5_forests_group, "Mvir", &halosoffset, &nhalos, buffer, double, halos,
                        Mvir); /* native Msun/h; accessor scales */

  CT_READ_ASSIGN_MULTI(h5_forests_group, "x", &halosoffset, &nhalos, buffer, double, halos, Pos, 0);
  CT_READ_ASSIGN_MULTI(h5_forests_group, "y", &halosoffset, &nhalos, buffer, double, halos, Pos, 1);
  CT_READ_ASSIGN_MULTI(h5_forests_group, "z", &halosoffset, &nhalos, buffer, double, halos, Pos, 2);

  CT_READ_ASSIGN_SINGLE(h5_forests_group, "vrms", &halosoffset, &nhalos, buffer, double, halos,
                        VelDisp);
  CT_READ_ASSIGN_SINGLE(h5_forests_group, "vmax", &halosoffset, &nhalos, buffer, double, halos,
                        Vmax);
  CT_READ_ASSIGN_SINGLE(h5_forests_group, "id", &halosoffset, &nhalos, buffer, int64_t, halos,
                        MostBoundID); /* the carried-through ctrees halo id */

  if (snap_field_is_double) {
    CT_READ_ASSIGN_SINGLE(h5_forests_group, snap_field_name, &halosoffset, &nhalos, buffer, double,
                          halos, SnapNum);
  } else {
    CT_READ_ASSIGN_SINGLE(h5_forests_group, snap_field_name, &halosoffset, &nhalos, buffer, int64_t,
                          halos, SnapNum);
  }

  CT_READ_ASSIGN_MULTI(h5_forests_group, "vx", &halosoffset, &nhalos, buffer, double, halos, Vel,
                       0);
  CT_READ_ASSIGN_MULTI(h5_forests_group, "vy", &halosoffset, &nhalos, buffer, double, halos, Vel,
                       1);
  CT_READ_ASSIGN_MULTI(h5_forests_group, "vz", &halosoffset, &nhalos, buffer, double, halos, Vel,
                       2);

  /* Spin holds the angular momentum J here; apply_ctrees_value_conventions
     normalises it by the native Mvir afterwards. */
  CT_READ_ASSIGN_MULTI(h5_forests_group, "Jx", &halosoffset, &nhalos, buffer, double, halos, Spin,
                       0);
  CT_READ_ASSIGN_MULTI(h5_forests_group, "Jy", &halosoffset, &nhalos, buffer, double, halos, Spin,
                       1);
  CT_READ_ASSIGN_MULTI(h5_forests_group, "Jz", &halosoffset, &nhalos, buffer, double, halos, Spin,
                       2);

  myfree(buffer);
  return EXIT_SUCCESS;
}

/* Read the per-forest halo counts (the "ForestNhalos" member of each file's
   "ForestInfo" compound dataset) into nhalos_per_forest, used only when the
   forest distribution is weighted. Returns EXIT_SUCCESS or CT_H5_ERR. */
static int read_nhalos_per_forest(const int firstfile, const int lastfile,
                                  const int64_t *totnforests_per_file, int64_t *nhalos_per_forest) {
  int64_t written = 0;
  for (int ifile = firstfile; ifile <= lastfile; ifile++) {
    const int64_t nforests_this_file = totnforests_per_file[ifile];
    hid_t finfo_dset = H5Dopen2(CTH.h5_file_groups[ifile], "ForestInfo", H5P_DEFAULT);
    XRETURN(finfo_dset >= 0, CT_H5_ERR, "Error: Could not open 'ForestInfo' in file %d\n", ifile);
    hid_t nhalos_dtype = H5Tcreate(H5T_COMPOUND, sizeof(int64_t));
    XRETURN(nhalos_dtype >= 0, CT_H5_ERR, "Error: Could not create compound type (file %d)\n",
            ifile);
    XRETURN(H5Tinsert(nhalos_dtype, "ForestNhalos", 0, H5T_NATIVE_INT64) >= 0, CT_H5_ERR,
            "Error: Could not insert 'ForestNhalos' field (file %d)\n", ifile);
    herr_t status = H5Dread(finfo_dset, nhalos_dtype, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                            &nhalos_per_forest[written]);
    XRETURN(status >= 0, CT_H5_ERR, "Error: Could not read 'ForestNhalos' (file %d)\n", ifile);
    XRETURN(H5Tclose(nhalos_dtype) >= 0, CT_H5_ERR, "Error: Could not close compound type\n");
    XRETURN(H5Dclose(finfo_dset) >= 0, CT_H5_ERR, "Error: Could not close 'ForestInfo' (file %d)\n",
            ifile);
    written += nforests_this_file;
  }
  return EXIT_SUCCESS;
}

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

/* The heavy lifting of open_partition: read the forests-HDF5 metadata, split the
   forests across tasks (weighted by halo count when configured), map this task's
   forest range onto its input files, verify the file cosmology against the
   simulation package, and stage the per-forest (file, treenr) lookup. Returns
   EXIT_SUCCESS or a negative sentinel; the caller turns failure into FATAL. */
static int setup_forests_io_ctrees_hdf5(const int thistask, const int ntasks) {
  const int firstfile = MimicConfig.FirstFile;
  const int lastfile = MimicConfig.LastFile;
  const int numfiles = lastfile - firstfile + 1;
  XRETURN(firstfile >= 0 && lastfile >= firstfile, CT_H5_ERR,
          "Error: invalid file range [first_file=%d, last_file=%d]; need 0 <= first_file <= "
          "last_file\n",
          firstfile, lastfile);

  char metadata_fname[2 * MAX_STRING_LEN + 1];
  snprintf(metadata_fname, sizeof(metadata_fname), "%s/%s%s", MimicConfig.SimulationDir,
           MimicConfig.TreeName, MimicConfig.TreeExtension);
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
  CTH.h5_file_groups = mymalloc_cat(totnfiles * sizeof(*CTH.h5_file_groups), MEM_IO);
  CTH.h5_forests_group = mymalloc_cat(totnfiles * sizeof(*CTH.h5_forests_group), MEM_IO);
  CTH.contig_halo_props = mymalloc_cat(totnfiles * sizeof(*CTH.contig_halo_props), MEM_IO);
  for (int64_t i = 0; i < totnfiles; i++) {
    CTH.h5_file_groups[i] = -1;
    CTH.h5_forests_group[i] = -1;
    CTH.contig_halo_props[i] = 0;
  }

  for (int ifile = firstfile; ifile <= lastfile; ifile++) {
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

  int64_t *totnforests_per_file = mymalloc_cat(totnfiles * sizeof(*totnforests_per_file), MEM_IO);
  for (int64_t i = 0; i < totnfiles; i++) {
    totnforests_per_file[i] = 0;
  }
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
    totnforests_per_file[ifile] = nforests_this_file;
    totnforests += nforests_this_file;
  }
  XRETURN(totnforests >= 1, CT_H5_ERR, "Error: total forest count %" PRId64 " must be >= 1\n",
          totnforests);
  if (totnforests >= INT_MAX) {
    XRETURN(0, CT_H5_ERR, "Error: forest count %" PRId64 " cannot be indexed by a 32-bit int\n",
            totnforests);
  }

  /* Weighted distribution needs per-forest halo counts; uniform does not. */
  const enum Valid_Forest_Distribution_Schemes scheme =
      (enum Valid_Forest_Distribution_Schemes)MimicConfig.ForestDistributionScheme;
  int64_t *nhalos_per_forest = NULL;
  if (scheme != uniform_in_forests) {
    nhalos_per_forest = mymalloc_cat(totnforests * sizeof(*nhalos_per_forest), MEM_IO);
    if (read_nhalos_per_forest(firstfile, lastfile, totnforests_per_file, nhalos_per_forest) !=
        EXIT_SUCCESS) {
      return CT_H5_ERR;
    }
  }

  int64_t nforests_this_task = 0, start_forestnum = 0;
  if (distribute_weighted_forests_over_ntasks(
          totnforests, nhalos_per_forest, scheme, MimicConfig.Exponent_Forest_Dist_Scheme, ntasks,
          thistask, &nforests_this_task, &start_forestnum) != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }
  if (nhalos_per_forest != NULL) {
    myfree(nhalos_per_forest);
  }
  XRETURN(nforests_this_task < CTREES_MAX_FORESTS_PER_TASK, CT_H5_ERR,
          "Error: task %d was assigned %" PRId64 " forests, at or above the unique-galaxy-id limit "
          "of %lld; run with more MPI tasks\n",
          thistask, nforests_this_task, (long long)CTREES_MAX_FORESTS_PER_TASK);

  CTH.nforests = nforests_this_task;
  Ntrees = (int)nforests_this_task;

  CTH.forest_filenum = mymalloc_cat(
      (nforests_this_task > 0 ? nforests_this_task : 1) * sizeof(*CTH.forest_filenum), MEM_IO);
  CTH.forest_treenr_in_file = mymalloc_cat((nforests_this_task > 0 ? nforests_this_task : 1) *
                                               sizeof(*CTH.forest_treenr_in_file),
                                           MEM_IO);
  InputTreeNHalos =
      mymalloc_cat((nforests_this_task > 0 ? nforests_this_task : 1) * sizeof(int), MEM_TREES);
  InputTreeFirstHalo =
      mymalloc_cat((nforests_this_task > 0 ? nforests_this_task : 1) * sizeof(int), MEM_TREES);
  for (int64_t i = 0; i < nforests_this_task; i++) {
    InputTreeNHalos[i] = 0;    /* filled per forest in load_unit */
    InputTreeFirstHalo[i] = 0; /* each forest loads into a fresh InputTreeHalos */
  }

  /* No forests for this task: nothing more to set up. */
  if (nforests_this_task == 0) {
    CTH.start_filenum = firstfile;
    CTH.end_filenum = firstfile;
    myfree(totnforests_per_file);
    return EXIT_SUCCESS;
  }

  const int64_t end_forestnum = start_forestnum + nforests_this_task; /* exclusive */
  int64_t *num_forests_to_process_per_file =
      mymalloc_cat(totnfiles * sizeof(*num_forests_to_process_per_file), MEM_IO);
  int64_t *start_forestnum_to_process_per_file =
      mymalloc_cat(totnfiles * sizeof(*start_forestnum_to_process_per_file), MEM_IO);
  for (int64_t i = 0; i < totnfiles; i++) {
    num_forests_to_process_per_file[i] = 0;
    start_forestnum_to_process_per_file[i] = 0;
  }

  int start_filenum = -1, end_filenum = -1;
  if (find_start_and_end_filenum(
          start_forestnum, end_forestnum, totnforests_per_file, totnforests, firstfile, lastfile,
          thistask, ntasks, num_forests_to_process_per_file, start_forestnum_to_process_per_file,
          &start_filenum, &end_filenum) != EXIT_SUCCESS) {
    return CT_H5_ERR;
  }
  CTH.start_filenum = start_filenum;
  CTH.end_filenum = end_filenum;

  /* Map each task-local forest to its file and the tree row within that file. */
  int curr_filenum = start_filenum;
  int64_t end_forestnum_in_currfile =
      totnforests_per_file[start_filenum] - start_forestnum_to_process_per_file[start_filenum];
  int64_t offset = 0;
  for (int64_t iforest = 0; iforest < nforests_this_task; iforest++) {
    if (iforest >= end_forestnum_in_currfile) {
      offset = end_forestnum_in_currfile;
      curr_filenum++;
      end_forestnum_in_currfile += totnforests_per_file[curr_filenum];
    }
    CTH.forest_filenum[iforest] = curr_filenum;
    if (curr_filenum == start_filenum) {
      CTH.forest_treenr_in_file[iforest] =
          iforest + start_forestnum_to_process_per_file[curr_filenum];
    } else {
      CTH.forest_treenr_in_file[iforest] = iforest - offset;
    }
  }

  myfree(start_forestnum_to_process_per_file);
  myfree(num_forests_to_process_per_file);
  myfree(totnforests_per_file);

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

  return EXIT_SUCCESS;
}

/**
 * @brief   Open this task's partition: distribute forests and stage file lookups.
 * @param   output_id   The MPI task id (the per-task output partition id).
 */
static void open_partition_ctrees_hdf5(int output_id) {
  (void)output_id; /* equals ThisTask; the split below reads ThisTask/NTask */
  const int thistask = ThisTask;
  const int ntasks = (NTask > 0) ? NTask : 1; /* serial builds leave NTask == 0 */

  memset(&CTH, 0, sizeof(CTH));
  CTH.meta_fd = -1;

  /* The unique-galaxy-id task term is FILENR_MUL_FAC*ThisTask; guard the int64
     overflow before any work. */
  if ((long long)thistask > CTREES_MAX_TASK_ID) {
    FATAL_ERROR("MPI task id %d exceeds the unique-galaxy-id task limit of %lld", thistask,
                (long long)CTREES_MAX_TASK_ID);
  }

  if (setup_forests_io_ctrees_hdf5(thistask, ntasks) != EXIT_SUCCESS) {
    FATAL_ERROR("Failed to set up the Consistent-Trees HDF5 reader on task %d", thistask);
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

  /* Read this forest's halo offset and count from the file's ForestInfo. */
  struct ctrees_forestinfo finfo;
  const hsize_t count = 1, treerow = (hsize_t)treenr_in_file;
  hid_t h5_file_group = CTH.h5_file_groups[filenum];
  {
    hid_t h5_dset = H5Dopen2(h5_file_group, "ForestInfo", H5P_DEFAULT);
    if (h5_dset < 0) {
      FATAL_ERROR("Consistent-Trees HDF5: could not open 'ForestInfo' in file %d", filenum);
    }
    hid_t h5_fspace = H5Dget_space(h5_dset);
    hid_t h5_memspace = H5Screate_simple(1, &count, NULL);
    if (h5_fspace < 0 || h5_memspace < 0 ||
        H5Sselect_hyperslab(h5_fspace, H5S_SELECT_SET, &treerow, NULL, &count, NULL) < 0) {
      FATAL_ERROR("Consistent-Trees HDF5: could not select 'ForestInfo' row %" PRId64 " in file %d",
                  treenr_in_file, filenum);
    }
    const hid_t h5_dtype = H5Dget_type(h5_dset);
    /* Read with the file's compound type into a fixed four-int64 struct; guard
       against a larger/relaid-out on-disk record overrunning the stack buffer
       (the read uses the file datatype, so its size must match exactly). */
    if (H5Tget_size(h5_dtype) != sizeof(struct ctrees_forestinfo)) {
      FATAL_ERROR("Consistent-Trees HDF5: 'ForestInfo' record in file %d is %zu bytes on disk but "
                  "the reader expects %zu (4 x int64); dataset layout mismatch",
                  filenum, H5Tget_size(h5_dtype), sizeof(struct ctrees_forestinfo));
    }
    if (H5Dread(h5_dset, h5_dtype, h5_memspace, h5_fspace, H5P_DEFAULT, &finfo) < 0) {
      FATAL_ERROR("Consistent-Trees HDF5: could not read 'ForestInfo' row %" PRId64 " in file %d",
                  treenr_in_file, filenum);
    }
    H5Tclose(h5_dtype);
    H5Sclose(h5_memspace);
    H5Sclose(h5_fspace);
    H5Dclose(h5_dset);
  }

  const int64_t halosoffset = finfo.foresthalosoffset;
  const int64_t nhalos = finfo.forestnhalos;
  if (nhalos < 0) {
    FATAL_ERROR("Consistent-Trees HDF5: forest %d has a negative halo count %" PRId64, unit,
                nhalos);
  }
  if (nhalos >= INT_MAX) {
    FATAL_ERROR("Consistent-Trees HDF5: forest %d has %" PRId64
                " halos, which cannot be indexed by "
                "an int",
                unit, nhalos);
  }
  if (nhalos >= TREE_MUL_FAC) {
    FATAL_ERROR("Consistent-Trees HDF5: forest %d has %" PRId64
                " halos, at or above the unique-galaxy-id limit of %lld",
                unit, nhalos, (long long)TREE_MUL_FAC);
  }

  struct halo_data *halos =
      mymalloc_cat(sizeof(struct halo_data) * (nhalos > 0 ? nhalos : 1), MEM_TREES);
  if (read_contiguous_forest_ctrees_h5(CTH.h5_forests_group[filenum], (hsize_t)nhalos,
                                       (hsize_t)halosoffset, CTH.snap_field_name,
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

/** @brief Close this task's partition: close groups/file, free scaffolding. */
static void close_partition_ctrees_hdf5(void) {
  if (CTH.h5_forests_group != NULL) {
    for (int i = 0; i < CTH.totnfiles; i++) {
      if (CTH.h5_forests_group[i] >= 0) {
        H5Gclose(CTH.h5_forests_group[i]);
      }
    }
    myfree(CTH.h5_forests_group);
  }
  if (CTH.h5_file_groups != NULL) {
    for (int i = 0; i < CTH.totnfiles; i++) {
      if (CTH.h5_file_groups[i] >= 0) {
        H5Gclose(CTH.h5_file_groups[i]);
      }
    }
    myfree(CTH.h5_file_groups);
  }
  if (CTH.contig_halo_props != NULL) {
    myfree(CTH.contig_halo_props);
  }
  if (CTH.forest_treenr_in_file != NULL) {
    myfree(CTH.forest_treenr_in_file);
  }
  if (CTH.forest_filenum != NULL) {
    myfree(CTH.forest_filenum);
  }
  if (CTH.meta_fd >= 0) {
    H5Fclose(CTH.meta_fd);
  }
  memset(&CTH, 0, sizeof(CTH));
}

/* Consistent-Trees forests-HDF5: forest-organised, merger pointers in-file. One
   partition per MPI task, one unit per forest; weighted forest distribution. See
   tree/registry.c. num_partitions/partition_output_id are unused for
   PARTITION_PER_TASK readers (the driver derives the output id from ThisTask). */
const struct TreeReader CTreesHDF5Reader = {
    .name = "consistent_trees_hdf5",
    .file_extension = ".h5",
    .partition_model = PARTITION_PER_TASK,
    .num_partitions = NULL,
    .partition_output_id = NULL,
    .open_partition = open_partition_ctrees_hdf5,
    .load_unit = load_unit_ctrees_hdf5,
    .close_partition = close_partition_ctrees_hdf5,
};

#endif /* HDF5 */
