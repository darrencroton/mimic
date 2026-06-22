#ifndef IO_TREE_READ_CTREES_HDF5_H
#define IO_TREE_READ_CTREES_HDF5_H

/**
 * @file    tree/read_ctrees_hdf5.h
 * @brief   Consistent-Trees forests-HDF5 merger-tree reader (HDF5 builds only).
 *
 * The reader's open/load/close callbacks are static (reached only through the
 * registered TreeReader, see tree/registry.c). The reader and its registration
 * are compiled only when HDF5 is enabled. The scientifically-meaningful seams it
 * shares with the ASCII reader (the halo_data -> RawHalo bridge and the
 * Consistent-Trees -> L-Halo value conventions) are exposed by
 * read_ctrees_common.h. Production builds expose no separate public surface from
 * this header; unit-test builds expose narrow validation hooks below.
 */

#if defined(HDF5) && defined(MIMIC_TEST_BUILD)

#include <stdint.h>

#include "tree/ctrees/ctrees_compat.h"

int ctrees_hdf5_test_read_nhalos_per_forest(const char *filename, int64_t expected_nforests,
                                            int64_t *nhalos_per_forest);
int ctrees_hdf5_test_read_forestinfo_cache(const char *filename, int64_t expected_nforests,
                                           int64_t row, int64_t *halosoffset, int64_t *nhalos);
int ctrees_hdf5_test_validate_forest_slab(const char *filename, int64_t halosoffset,
                                          int64_t nhalos);
int ctrees_hdf5_test_open_field_cache(const char *filename, const char *snap_field_name,
                                      int8_t snap_field_is_double);
int ctrees_hdf5_test_read_forest(const char *filename, const char *snap_field_name,
                                 int8_t snap_field_is_double, int64_t halosoffset, int64_t nhalos,
                                 struct halo_data *halos);
int ctrees_hdf5_test_read_two_forests_windowed(const char *filename, const char *snap_field_name,
                                               int8_t snap_field_is_double,
                                               int64_t first_halosoffset, int64_t first_nhalos,
                                               struct halo_data *first_halos,
                                               int64_t second_halosoffset, int64_t second_nhalos,
                                               struct halo_data *second_halos);

#endif /* HDF5 && MIMIC_TEST_BUILD */

#endif /* IO_TREE_READ_CTREES_HDF5_H */
