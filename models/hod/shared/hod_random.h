/**
 * @file    hod_random.h
 * @brief   Counter-based random numbers for the HOD model package (model-private)
 *
 * A stateless generator: every value is a pure function of a 64-bit stream key
 * and a draw index, so a draw never depends on how many draws came before it,
 * on traversal order or on partitioning. hod_populate keys one stream per host
 * from (HODSeed, snapshot number, host UniqueGalaxyID) and numbers its draws
 * within that stream (hod_populate.h documents the index layout).
 *
 * - splitmix64 finaliser (Steele, Lea & Flood 2014) as the mixing function,
 *   after the pattern of the legacy SHAM scatter generator
 * - uniforms strictly inside (0, 1): 52 random bits centred in their cells, so
 *   the extremes are 2^-53 and 1 - 2^-53 and a logarithm is always finite
 * - Box-Muller standard Gaussians from two uniforms at consecutive indices
 * - Poisson counts by inversion from one uniform with a sequential cumulative
 *   search, each term formed in logarithms so a large mean cannot underflow
 *
 * Header-only and private to models/hod; it is not framework API.
 */

#ifndef HOD_RANDOM_H
#define HOD_RANDOM_H

#include <math.h>
#include <stdint.h>

/** Weyl increment of splitmix64 (2^64 / golden ratio) */
#define HOD_RANDOM_GOLDEN UINT64_C(0x9e3779b97f4a7c15)

/** Domain-separation salts for the three key components */
#define HOD_RANDOM_SALT_SEED UINT64_C(0x5d1a6b0c3f2e4987)
#define HOD_RANDOM_SALT_SNAPSHOT UINT64_C(0xa3c59ac2f13b7e61)
#define HOD_RANDOM_SALT_HOST UINT64_C(0x2f8b13d7c0e9a465)

/** 2 pi, for the Box-Muller angle */
#define HOD_RANDOM_TWO_PI 6.283185307179586476925286766559

/**
 * @brief   splitmix64 finaliser: a bijective, well-mixed 64-bit hash
 *
 * @param   x  Value to mix
 * @return  Mixed value
 */
static inline uint64_t hod_random_mix(uint64_t x) {
  x += HOD_RANDOM_GOLDEN;
  x = (x ^ (x >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
  x = (x ^ (x >> 27)) * UINT64_C(0x94d049bb133111eb);
  return x ^ (x >> 31);
}

/**
 * @brief   Stream key for one host draw: (seed, snapshot, host ID)
 *
 * Each component is salted and folded through the mixer in turn, so changing
 * any one of them gives an unrelated stream. Negative host IDs (records created
 * by a module) are folded by their two's-complement bits.
 *
 * @param   seed      Run seed (HODSeed, >= 0)
 * @param   snapshot  Snapshot number
 * @param   host_id   Host UniqueGalaxyID
 * @return  Stream key
 */
static inline uint64_t hod_random_key(int64_t seed, int snapshot, long long host_id) {
  uint64_t key = hod_random_mix((uint64_t)seed ^ HOD_RANDOM_SALT_SEED);
  key = hod_random_mix(key ^ ((uint64_t)(int64_t)snapshot + HOD_RANDOM_SALT_SNAPSHOT));
  return hod_random_mix(key ^ ((uint64_t)host_id + HOD_RANDOM_SALT_HOST));
}

/**
 * @brief   Raw 64 random bits for draw @p index of stream @p key
 *
 * Counter mode: the index advances the splitmix64 Weyl sequence from the key,
 * and the mixer turns each position into an independent-looking value.
 */
static inline uint64_t hod_random_bits(uint64_t key, uint64_t index) {
  return hod_random_mix(key + index * HOD_RANDOM_GOLDEN);
}

/**
 * @brief   Map 64 random bits to a double strictly inside (0, 1)
 *
 * Keeps the top 52 bits and centres them in their cell: (k + 0.5) 2^-52 for
 * k in [0, 2^52). Both k + 0.5 and the product are exact, so the result lies in
 * [2^-53, 1 - 2^-53] and can be neither 0 nor 1.
 */
static inline double hod_random_uniform_from_bits(uint64_t bits) {
  return ((double)(bits >> 12) + 0.5) * 0x1.0p-52;
}

/** @brief Uniform deviate in the open interval (0, 1) for draw @p index of stream @p key */
static inline double hod_random_uniform(uint64_t key, uint64_t index) {
  return hod_random_uniform_from_bits(hod_random_bits(key, index));
}

/**
 * @brief   Standard Gaussian deviate (Box-Muller) from draws @p index and @p index + 1
 *
 * Uses the cosine branch only, so each Gaussian owns its two uniforms and no
 * value is cached between calls. log(u1) is finite because u1 > 0.
 */
static inline double hod_random_gaussian(uint64_t key, uint64_t index) {
  const double u1 = hod_random_uniform(key, index);
  const double u2 = hod_random_uniform(key, index + 1);
  return sqrt(-2.0 * log(u1)) * cos(HOD_RANDOM_TWO_PI * u2);
}

/**
 * @brief   Poisson count by inversion: the smallest k with P(N <= k) >= u
 *
 * Sequential cumulative search from k = 0. Each probability is formed as
 * exp(k ln lambda - lambda - lgamma(k + 1)), so a mean of several hundred,
 * whose exp(-lambda) underflows, still gives correct terms. The search stops
 * early, returning the current k, once k is above the mean and the next term
 * no longer changes the cumulative sum (the remaining tail is below double
 * resolution), so a u within rounding of 1 cannot run away.
 *
 * @param   lambda     Mean, finite and >= 0 (0 always gives 0)
 * @param   u          Uniform deviate in (0, 1)
 * @param   max_count  Largest count the caller accepts
 * @return  The count in [0, max_count], or max_count + 1 when the count would
 *          exceed max_count (the search stops there)
 */
static inline int hod_random_poisson(double lambda, double u, int max_count) {
  if (!(lambda > 0.0)) {
    return 0;
  }
  const double log_lambda = log(lambda);
  double cumulative = exp(-lambda);
  int k = 0;
  while (cumulative < u) {
    if (k >= max_count) {
      return max_count + 1;
    }
    k++;
    const double term = exp((double)k * log_lambda - lambda - lgamma((double)k + 1.0));
    if ((double)k > lambda && cumulative + term == cumulative) {
      break;
    }
    cumulative += term;
  }
  return k;
}

#endif /* HOD_RANDOM_H */
