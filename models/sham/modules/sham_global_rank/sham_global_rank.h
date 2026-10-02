/**
 * @file    sham_global_rank.h
 * @brief   Rank helper of the sham_global_rank snapshot module
 *
 * The module's process_snapshot() callback is a thin wrapper around
 * sham_global_rank_assign(), which is declared here so the module's unit tests
 * can pass synthetic populations and targets to it directly. It is not
 * framework API: nothing outside models/sham/modules/sham_global_rank/ may
 * include this header.
 */

#ifndef SHAM_GLOBAL_RANK_H
#define SHAM_GLOBAL_RANK_H

#include <stdint.h>

#include "types.h"

/** Upper bound of an assigned stellar mass, in internal mass units (1e10 Msun/h) */
#define SHAM_GLOBAL_MAX_STELLAR_MASS 100000.0

/**
 * @brief Analytic cumulative stellar mass function n(>M) = n0 (M / M0)^(-alpha)
 *
 * Values are taken as given; sham_global_rank_init() owns their domain checks.
 */
struct ShamGlobalRankTarget {
  double mass_scale;     /**< M0, internal mass units (1e10 Msun/h), in (0, 100000] */
  double number_density; /**< n0, (Mpc/h)^-3, in [1e-12, 1e3] */
  double slope;          /**< alpha, dimensionless, in [0.1, 10] */
  double box_size;       /**< Comoving box side, Mpc/h; finite, positive, finite positive cube */
};

/**
 * @brief Validate, update peaks, rank and assign stellar mass over one population
 *
 * Every entry must be Type 0, 1 or 2 with a non-NULL galaxy and a positive
 * UniqueGalaxyID that is unique within the population, and every consumed
 * proxy must be finite and nonnegative; any violation fails the call before
 * any galaxy is written. Type 0/1 entries then update ShamVpeak/ShamMpeak from
 * Vmax/Mvir, Type 2 entries keep their inherited peaks, and every entry has its
 * stellar fields reset to zero. Entries with ShamVpeak > 0 are ranked by
 * descending ShamVpeak, ties by ascending UniqueGalaxyID, and zero-based rank r
 * receives M_r with
 *
 *   ln M_r = ln M0 - [ln(r + 0.5) - 3 ln BoxSize - ln n0] / alpha,
 *
 * in StellarMass and ShamStellarMassNoScatter. A rank with ln M_r above
 * ln(100000), or whose float mass is not finite, is zero or exceeds 100000,
 * fails the call before any stellar mass is assigned.
 *
 * The population is borrowed and never reordered; sorting happens on tracked
 * scratch released before return. An empty population allocates nothing.
 *
 * @param   target  Analytic target (see struct ShamGlobalRankTarget)
 * @param   halos   Borrowed population; may be NULL when count is 0
 * @param   count   Number of entries
 * @return  0 on success, -1 on any violation (logged with its cause)
 */
int sham_global_rank_assign(const struct ShamGlobalRankTarget *target, const struct Halo *halos,
                            int64_t count);

#endif /* SHAM_GLOBAL_RANK_H */
