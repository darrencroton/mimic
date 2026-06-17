#ifndef IO_TREE_READ_CTREES_ASCII_H
#define IO_TREE_READ_CTREES_ASCII_H

/**
 * @file    tree/read_ctrees_ascii.h
 * @brief   Testable seams of the Consistent-Trees ASCII reader.
 *
 * The reader's open/load/close callbacks are static (reached only through the
 * registered TreeReader). The ASCII-specific convention transform below is
 * exposed here so it can be unit-tested directly without a simulation package or
 * an end-to-end run. The bridge and the shared value conventions live in
 * read_ctrees_common.h (used by the HDF5 reader too).
 */

#include <stdint.h>

#include "tree/ctrees/ctrees_compat.h" /* struct halo_data */
#include "tree/ctrees/ctrees_utils.h"  /* struct additional_info */
#include "tree/read_ctrees_common.h"   /* apply_ctrees_value_conventions, bridge */

/* Apply the full Consistent-Trees ASCII -> L-Halo conventions to one tree's
   halos in place: the shared value conventions (spin normalisation and the
   particle-count estimate), the carried-through ctrees id as MostBoundID, and
   the pre-topology link sentinels. Operates on the NATIVE Mvir; unit scaling is
   left to the generated reference-unit accessors. */
void convert_ctrees_to_lht(struct halo_data *halos, const struct additional_info *info,
                           int64_t nhalos);

#endif /* IO_TREE_READ_CTREES_ASCII_H */
