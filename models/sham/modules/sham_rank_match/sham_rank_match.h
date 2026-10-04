/**
 * @file    sham_rank_match.h
 * @brief   Target, inversion and rank helpers of the sham_rank_match module
 *
 * The module's callbacks are thin wrappers around the helpers declared here,
 * which are exposed so the module's unit tests can check the converted target,
 * its tabulated cumulative density and its inversion directly. It is not
 * framework API: nothing outside models/sham/modules/sham_rank_match/ may
 * include this header.
 *
 * Units. Target masses are physical Msun and densities physical Mpc^-3, both
 * for the simulation's Hubble parameter h_sim once converted. StellarMass is
 * stored in Mimic's internal 1e10 Msun/h, so M_internal = M_phys * h_sim / 1e10.
 */

#ifndef SHAM_RANK_MATCH_H
#define SHAM_RANK_MATCH_H

#include <stdbool.h>
#include <stdint.h>

/** Upper limit of the n(>M) integral and of the table, in units of the Schechter mass Ms */
#define SHAM_TARGET_MAX_MASS_RATIO 120.0

/** Nodes of the tabulated n(>M), log-spaced from 10^(floor - 1) to 120 Ms (at least 4096) */
#define SHAM_TARGET_TABLE_POINTS 4096

/** Physical Msun per internal mass unit, before the factor h (internal unit 1e10 Msun/h) */
#define SHAM_MASS_UNIT_MSUN 1.0e10

/** Outcomes of sham_rank_match_mass_at_density() */
#define SHAM_RANK_MASKED 0
#define SHAM_RANK_ASSIGNED 1

/**
 * @brief Run-file parameters of the target and the candidate cut, as published
 *
 * The double Schechter values are those of the source paper at h_obs; they are
 * converted once to the simulation's h by sham_rank_match_build_target().
 */
struct ShamRankMatchParameters {
  double log_mstar;      /**< log10 Ms, physical Msun at h_obs */
  double phi1;           /**< phi1, physical Mpc^-3 at h_obs; > 0 */
  double alpha1;         /**< alpha1, dimensionless */
  double phi2;           /**< phi2, physical Mpc^-3 at h_obs; > 0 */
  double alpha2;         /**< alpha2, dimensionless */
  double hubble_obs;     /**< h_obs of the published fit, in (0, 2) */
  double log_mass_floor; /**< log10 of the lowest assignable mass, physical Msun at h_sim */
  double redshift_max;   /**< Largest output-snapshot redshift the target may be applied at */
  double min_vpeak;      /**< Candidate completeness floor on ShamVpeak, km/s; > 0 */
};

/**
 * @brief The target converted to the simulation's h, with its tabulated n(>M)
 *
 * log_mass[k] = ln M_k is uniformly spaced from ln 10^(floor - 1) to
 * ln(120 Ms_sim); density[k] = n(>M_k) is strictly decreasing, positive below
 * the last node and exactly 0 at it; log_density[k] = ln density[k] for
 * k < num_points - 1. Owned by whoever built it; sham_rank_match_free_target()
 * releases the arrays.
 */
struct ShamRankMatchTarget {
  double hubble_sim;     /**< h_sim, the simulation's Hubble parameter */
  double log_mstar;      /**< log10 Ms_sim = log10 Ms + 2 log10(h_obs / h_sim), physical Msun */
  double phi1;           /**< phi1_sim = phi1 (h_sim / h_obs)^3, physical Mpc^-3 */
  double alpha1;         /**< alpha1, unchanged */
  double phi2;           /**< phi2_sim = phi2 (h_sim / h_obs)^3, physical Mpc^-3 */
  double alpha2;         /**< alpha2, unchanged */
  double log_mass_floor; /**< log10 of the mass floor, physical Msun */
  double floor_density;  /**< n(>10^log_mass_floor), physical Mpc^-3; > 0 */
  int num_points;        /**< Table nodes */
  double *log_mass;      /**< ln M_k, physical Msun */
  double *density;       /**< n(>M_k), physical Mpc^-3 */
  double *log_density;   /**< ln n(>M_k) for every node but the last */
};

/**
 * @brief Convert the published target to h_sim and tabulate n(>M)
 *
 * log10 Ms_sim = log_mstar + 2 log10(h_obs / h_sim) and phi_i_sim = phi_i
 * (h_sim / h_obs)^3. Each table interval of
 *
 *   n(>M) = integral_M^(120 Ms) exp(-x) [phi1 x^alpha1 + phi2 x^alpha2] dx,  x = M' / Ms,
 *
 * is integrated in ln M' (integrand exp(-x) [phi1 x^(alpha1+1) + phi2 x^(alpha2+1)])
 * by Simpson's rule on the interval's two halves, and the intervals are summed
 * from the top node down. The table must be finite, positive and strictly
 * decreasing below the last node, and the floor must lie strictly inside it;
 * otherwise the call fails and nothing is left allocated.
 *
 * @param   params      Published target (domain-checked by the caller)
 * @param   hubble_sim  The simulation's h; finite and positive
 * @param   target      Output; its arrays are tracked MEM_UTILITY allocations
 * @return  0 on success, -1 on failure (logged)
 */
int sham_rank_match_build_target(const struct ShamRankMatchParameters *params, double hubble_sim,
                                 struct ShamRankMatchTarget *target);

/** @brief Release a target's arrays and zero it; safe on a zeroed or released target */
void sham_rank_match_free_target(struct ShamRankMatchTarget *target);

/**
 * @brief n(>M) in physical Mpc^-3 from the table, by log-log interpolation
 *
 * Linear in ln M on the last interval, whose upper node is 0.
 *
 * @return  The density; NAN for a mass below the table or not finite and positive, 0 at or
 *          above 120 Ms
 */
double sham_rank_match_cumulative_density(const struct ShamRankMatchTarget *target, double mass);

/**
 * @brief Invert the table: the physical mass M with n(>M) = density
 *
 * Bisection over the table's nodes finds the interval whose end densities
 * bracket @p density, then ln M is interpolated linearly in ln n.
 *
 * @return  0 on success, -1 when @p density is not finite or lies outside
 *          [n(>M_(N-2)), n(>M_0)] (nothing is extrapolated)
 */
int sham_rank_match_inverse(const struct ShamRankMatchTarget *target, double density, double *mass);

/**
 * @brief The rank-match decision for one rank density
 *
 * A density above the floor density is masked; otherwise *mass is n^-1(density).
 *
 * @param   target   Built target
 * @param   density  Rank density, physical Mpc^-3
 * @param   mass     Output, physical Msun, written only when the density is assigned
 * @return  SHAM_RANK_ASSIGNED, SHAM_RANK_MASKED, or -1 when the inversion fails
 */
int sham_rank_match_mass_at_density(const struct ShamRankMatchTarget *target, double density,
                                    double *mass);

/**
 * @brief Physical rank density (r + 0.5) / BoxSize^3 * h_sim^3, Mpc^-3, for zero-based rank r
 *
 * Formed as (r + 0.5) (h_sim / BoxSize)^3 so a large box cannot overflow the cube.
 */
double sham_rank_match_rank_density(int64_t rank, double box_size, double hubble_sim);

/** @brief Whether @p snapshot is an output snapshot of MimicConfig (NOUT == 0 means all) */
bool sham_rank_match_is_output_snapshot(int snapshot);

/** @brief The target init() built, or NULL before a successful init() or after cleanup() */
const struct ShamRankMatchTarget *sham_rank_match_active_target(void);

#endif /* SHAM_RANK_MATCH_H */
