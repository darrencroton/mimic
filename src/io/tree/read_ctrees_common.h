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
 *   - the galaxy-id uniqueness bounds the readers assert.
 *
 * Implementations live in read_ctrees_ascii.c (the always-compiled, unit-tested
 * ctrees reader translation unit); the HDF5 reader links against them. Unit
 * scaling (mass * 1e-10, positions to Mpc/h) is deliberately NOT done here — it
 * is handled downstream by the generated reference-unit accessors.
 */

#include <limits.h>
#include <stdint.h>

#include "constants.h"                 /* FILENR_MUL_FAC, TREE_MUL_FAC */
#include "tree/ctrees/ctrees_compat.h" /* struct halo_data */
#include "types.h"                     /* struct RawHalo */

/* Galaxy-id uniqueness bounds. ids are halonr + TREE_MUL_FAC*forestnr_local +
   FILENR_MUL_FAC*ThisTask. make_unique_galaxy_id() uses the full FILENR_MUL_FAC
   stride for PARTITION_PER_TASK readers (it does not apply the L-Halo many-files
   reduction), so for ids to stay collision free a forest must have fewer than
   TREE_MUL_FAC halos, a task fewer than FILENR_MUL_FAC/TREE_MUL_FAC forests, and
   the task term FILENR_MUL_FAC*ThisTask must not overflow int64. Shared by both
   Consistent-Trees readers. */
#define CTREES_MAX_FORESTS_PER_TASK (FILENR_MUL_FAC / TREE_MUL_FAC)
#define CTREES_MAX_TASK_ID (LLONG_MAX / FILENR_MUL_FAC)

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
