#ifndef IO_TREE_READ_CTREES_COMMON_H
#define IO_TREE_READ_CTREES_COMMON_H

/**
 * @file    tree/read_ctrees_common.h
 * @brief   Shared seam for the Consistent-Trees readers (ASCII + HDF5).
 *
 * Both ctrees readers present the format as the partition/unit model (one
 * partition per MPI task, one unit per forest) and end by bridging a
 * reconstructed/loaded `struct halo_data` forest into the generated
 * per-simulation `struct RawHalo`. The pieces that are identical between the two
 * readers live here:
 *   - the halo_data -> RawHalo bridge;
 *   - the order-dependent value conventions applied on the NATIVE Mvir (spin
 *     normalisation and the particle-count estimate);
 *   - the shared UniqueGalaxyID capacity helper used before publishing
 *     GlobalForestOffset.
 *
 * Implementations live in read_ctrees_ascii.c (the always-compiled, unit-tested
 * ctrees reader translation unit); the HDF5 reader links against them. Unit
 * scaling (mass * 1e-10, positions to Mpc/h) is deliberately NOT done here — it
 * is handled downstream by the generated reference-unit accessors.
 */

#include <stdint.h>

#include "galaxy_id.h"                 /* UniqueGalaxyID capacity helper */
#include "tree/ctrees/ctrees_compat.h" /* struct halo_data */
#include "types.h"                     /* struct RawHalo */

/* Apply the Consistent-Trees -> L-Halo value conventions in place, operating on
   the NATIVE Mvir: spin normalisation (Spin /= Mvir) and the particle-count
   estimate (Len = round(Mvir * 1e-10 / particle_mass)). Shared by both readers;
   each reader supplies the merger pointers and the carried-through id separately
   (reconstructed for ASCII, read from file for HDF5). */
void apply_ctrees_value_conventions(struct halo_data *halos, int64_t nhalos);

/* Copy one reconstructed/loaded halo_data record into the generated RawHalo
   layout. The target field names are the readers' contract on the simulation
   package (see docs/dev/CTREES-UCHUU-VALIDATION.md). */
void bridge_halo_data_to_rawhalo(struct RawHalo *out, const struct halo_data *in);

#endif /* IO_TREE_READ_CTREES_COMMON_H */
