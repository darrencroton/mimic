#ifndef IO_OUTPUT_HDF5_INTERNAL_H
#define IO_OUTPUT_HDF5_INTERNAL_H

/**
 * @file    hdf5_internal.h
 * @brief   Shared internals of the HDF5 output writers
 *
 * Private interface between the HDF5 table writer (hdf5.c), the run-metadata
 * writers (metadata_hdf5.c), and the master-file aggregator (master_hdf5.c).
 * Not for use outside src/io/output/.
 */

#include <hdf5.h>

/* Attach a scalar string "description" attribute to an HDF5 object (fatal on failure) */
void write_description_attr(hid_t obj_id, const char *text);

/* Write the RunProperties group (version, modules, contracts, parameters,
 * redshifts, field schema) into a per-file output for self-containment */
void write_perfile_metadata(hid_t file_id);

/* Write the full RunProperties group (config attributes + extended metadata)
 * into the master file */
void store_run_properties(hid_t master_file_id);

#endif /* IO_OUTPUT_HDF5_INTERNAL_H */
