#ifndef IO_TREE_READ_CTREES_ASCII_H
#define IO_TREE_READ_CTREES_ASCII_H

/**
 * @file    tree/read_ctrees_ascii.h
 * @brief   Testable seams of the Consistent-Trees ASCII reader.
 *
 * The reader's open/load/close callbacks are static (reached only through the
 * registered TreeReader). The two functions below encode the reader's
 * scientifically-meaningful conventions — the Consistent-Trees -> L-Halo value
 * transforms and the halo_data -> RawHalo bridge — and are exposed here so they
 * can be unit-tested directly without a simulation package or an end-to-end run.
 */

#include <stdint.h>

#include "tree/ctrees/ctrees_compat.h" /* struct halo_data */
#include "tree/ctrees/ctrees_utils.h"  /* struct additional_info */
#include "types.h"                     /* struct RawHalo */

/* Apply the Consistent-Trees -> L-Halo conventions to one tree's halos in place:
   spin normalisation (J / Mvir_native), the particle-count estimate
   (round(Mvir_native / particle_mass)), the carried-through id, and the
   pre-topology link sentinels. Operates on the NATIVE Mvir; unit scaling is left
   to the generated reference-unit accessors. */
void convert_ctrees_to_lht(struct halo_data *halos, const struct additional_info *info,
                           int64_t nhalos);

/* Copy one reconstructed halo_data record into the generated RawHalo layout. */
void bridge_halo_data_to_rawhalo(struct RawHalo *out, const struct halo_data *in);

#endif /* IO_TREE_READ_CTREES_ASCII_H */
