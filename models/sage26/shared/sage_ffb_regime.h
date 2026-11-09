/**
 * @file    sage_ffb_regime.h
 * @brief   SAGE26's feedback-free-burst (FFB) classification arithmetic for
 *          EnhancedStarFormationOn 1: the Li et al. (2024) threshold mass and FFB fraction, the
 *          halo's SplitMix64 draw, and the EnhancedStarFormationOn validator
 *
 * Implements the following routines of SAGE26's model_regimes.c:
 *
 * - mimic_sage_ffb_threshold_mass(): calculate_ffb_threshold_mass, Li et al. (2024) eq. 2 in code
 *   units (1e10 Msun/h):
 *
 *     z_norm            = (1.0 + z) / 10.0
 *     log_Mvir_ffb_code = 0.8 + log10(h) + FFB_THRESHOLD_SLOPE * log10(z_norm)
 *     return pow(10.0, log_Mvir_ffb_code)
 *
 *   with FFB_THRESHOLD_SLOPE the double -6.2.
 * - mimic_sage_ffb_fraction(): calculate_ffb_fraction at EnhancedStarFormationOn 1, Li et al.
 *   (2024) eq. 3:
 *
 *     Mvir_ffb = the threshold at z
 *     Mvir <= 0.0 || Mvir_ffb <= 0.0      -> 0
 *     x        = log10(Mvir / Mvir_ffb) / 0.15
 *     return 1.0 / (1.0 + exp(-x))
 *
 *   Its EnhancedStarFormationOn == 0 return is not implemented: in the package that setting is
 *   the caller's gate (sage26_determine_ffb_regime writes nothing), as SAGE26 never calls the
 *   classifier then.
 * - mimic_sage_ffb_halo_uniform_bits() and mimic_sage_ffb_uniform_from_bits(): SAGE26's static
 *   halo_uniform, the SplitMix64 (Steele, Lea & Flood 2014) hash of a halo's 64-bit MostBoundID
 *   and the snapshot, and its deviate; mimic_sage_ffb_halo_uniform() composes the two. This is
 *   SAGE26's own draw, with its unsigned wrap-around, its conversion of the signed ID and its
 *   (uint32_t) snapshot. It is not the package's CGM keyed draw (shared/sage_keyed_draw.h), and
 *   no rand() is called.
 * - mimic_sage_validate_enhanced_star_formation_on(): the required EnhancedStarFormationOn, 0
 *   (off) or 1 (the Li et al. 2024 classifier, SAGE26's default). Mode 2, the Boylan-Kolchin
 *   (2025) acceleration criterion, is not supported and is refused with every other value.
 *
 * Each helper takes its inputs as arguments and reads no module state. The caller passes the
 * row's core Mvir narrowed to the float SAGE26 holds and widened, the current redshift (SAGE26's
 * Zcurr) and the run's Hubble_h.
 *
 * SAGE26's quirks, kept: a NaN Mvir or threshold passes the guards and gives a NaN fraction,
 * which compares false with every draw; z = -1 gives an infinite threshold, so every finite Mvir
 * has fraction 0 through x = -inf, not through the guard; a redshift below -1 gives a NaN
 * threshold. The draw has no endpoint correction: (x >> 11) + 0.5 is rounded to a double, so the
 * largest 53-bit value 2^53 - 1 rounds to 2^53 and its deviate is exactly 1.0.
 *
 * Reference: Li et al. (2024), eqs. 2-3, and Steele, Lea & Flood (2014), as SAGE26 cites them
 *
 * @note Used by sage26_determine_ffb_regime; _tests/test_unit_sage_ffb_regime.c tests each
 *       helper.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_FFB_REGIME_H
#define MIMIC_SHARED_SAGE_FFB_REGIME_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"
#include "shared/sage_exp10.h"

/** EnhancedStarFormationOn 0: no FFB classification or FFB star formation */
#define MIMIC_SAGE_FFB_OFF 0
/** EnhancedStarFormationOn 1: the Li et al. (2024) classifier (SAGE26's default) */
#define MIMIC_SAGE_FFB_LI24 1

/* SAGE parity: the double FFB_THRESHOLD_SLOPE, -6.2 */
#define MIMIC_SAGE_FFB_THRESHOLD_SLOPE (-6.2)
/* SAGE parity: the threshold's normalisation at z = 9, 10^10.8 Msun as the literal 0.8 of the
 * code-unit log */
#define MIMIC_SAGE_FFB_THRESHOLD_LOG_NORM 0.8
/* SAGE parity: the sigmoid width delta_log_M, 0.15 dex */
#define MIMIC_SAGE_FFB_DELTA_LOG_M 0.15

/* SAGE parity: halo_uniform's constants, SplitMix64's golden increment and its two finaliser
 * multipliers, and 2^53 as a double literal */
#define MIMIC_SAGE_FFB_GOLDEN 0x9E3779B97F4A7C15ULL
#define MIMIC_SAGE_FFB_MIX1 0xBF58476D1CE4E5B9ULL
#define MIMIC_SAGE_FFB_MIX2 0x94D049BB133111EBULL
#define MIMIC_SAGE_FFB_TWO_POW_53 9007199254740992.0

/** SAGE26's locals of one calculate_ffb_threshold_mass call (each double as SAGE26 forms it) */
struct MimicSageFfbThresholdLocals {
  double z_norm;            /**< (1.0 + z) / 10.0 */
  double log_Mvir_ffb_code; /**< 0.8 + log10(h) + FFB_THRESHOLD_SLOPE * log10(z_norm) */
};

/** SAGE26's locals of one calculate_ffb_fraction call */
struct MimicSageFfbFractionLocals {
  struct MimicSageFfbThresholdLocals threshold; /**< The threshold's locals */
  double Mvir_ffb;                              /**< The threshold mass, 1e10 Msun/h */
  int guard_returned; /**< 1 when Mvir <= 0.0 || Mvir_ffb <= 0.0 returned 0 */
  double x;           /**< log10(Mvir / Mvir_ffb) / 0.15; NaN when the guard returned */
};

/**
 * @brief SAGE26's calculate_ffb_threshold_mass
 *
 * @param z         The redshift (SAGE26 passes Zcurr = ZZ[halo_snapnum])
 * @param hubble_h  The run's Hubble_h
 * @param locals    Optional (NULL): receives z_norm and log_Mvir_ffb_code
 * @return The FFB threshold virial mass in 1e10 Msun/h
 */
static inline double mimic_sage_ffb_threshold_mass(const double z, const double hubble_h,
                                                   struct MimicSageFfbThresholdLocals *locals) {
  const double h = hubble_h;
  const double z_norm = (1.0 + z) / 10.0;
  /* SAGE parity: the slope's product and the sum are one fma, as SAGE26's build evaluates it */
  const double log_Mvir_ffb_code = fma(MIMIC_SAGE_FFB_THRESHOLD_SLOPE, log10(z_norm),
                                       MIMIC_SAGE_FFB_THRESHOLD_LOG_NORM + log10(h));
  if (locals != NULL) {
    locals->z_norm = z_norm;
    locals->log_Mvir_ffb_code = log_Mvir_ffb_code;
  }
  /* SAGE parity: exp10, as SAGE26's build evaluates pow(10.0, ...) here */
  return mimic_sage_exp10(log_Mvir_ffb_code);
}

/**
 * @brief SAGE26's calculate_ffb_fraction at EnhancedStarFormationOn 1 (without its
 *        EnhancedStarFormationOn 0 return, the caller's gate)
 *
 * @param Mvir      The row's Mvir in 1e10 Msun/h: the float SAGE26 holds, widened
 * @param z         The current redshift
 * @param hubble_h  The run's Hubble_h
 * @param locals    Optional (NULL): receives the threshold, its locals, the guard flag and x
 * @return The FFB fraction in [0, 1], or NaN for a NaN Mvir or threshold
 */
static inline double mimic_sage_ffb_fraction(const double Mvir, const double z,
                                             const double hubble_h,
                                             struct MimicSageFfbFractionLocals *locals) {
  const double Mvir_ffb =
      mimic_sage_ffb_threshold_mass(z, hubble_h, locals != NULL ? &locals->threshold : NULL);
  if (locals != NULL) {
    locals->Mvir_ffb = Mvir_ffb;
    locals->guard_returned = 0;
    locals->x = NAN;
  }

  /* SAGE parity: only a non-positive Mvir or threshold returns 0; a NaN passes */
  if (Mvir <= 0.0 || Mvir_ffb <= 0.0) {
    if (locals != NULL) {
      locals->guard_returned = 1;
    }
    return 0.0;
  }

  /* x is SAGE26's local, log10(Mvir / Mvir_ffb) / delta_log_M */
  const double delta_log_M = MIMIC_SAGE_FFB_DELTA_LOG_M;
  const double log_ratio = log10(Mvir / Mvir_ffb);
  const double x = log_ratio / delta_log_M;
  if (locals != NULL) {
    locals->x = x;
  }
  /* SAGE parity: exp(-x) is evaluated as a division by the negated width, as SAGE26's build
   * computes it; this keeps SAGE26's NaN sign for a NaN Mvir or threshold */
  const double f_ffb = 1.0 / (1.0 + exp(log_ratio / -delta_log_M));
  return f_ffb;
}

/**
 * @brief The 64 hashed bits of SAGE26's halo_uniform
 *
 * The signed ID is converted to uint64_t (two's complement, so a negative ID keeps its bits), the
 * snapshot to uint32_t and then uint64_t, and every product and sum wraps modulo 2^64.
 *
 * @param most_bound_id  The halo's MostBoundID (SAGE26's long long galaxies[p].MostBoundID)
 * @param snapnum        The current snapshot (SAGE26's halo_snapnum)
 * @return x after the final xorshift, before the deviate's >> 11
 */
static inline uint64_t mimic_sage_ffb_halo_uniform_bits(const long long most_bound_id,
                                                        const int snapnum) {
  uint64_t x = (uint64_t)most_bound_id ^ ((uint64_t)(uint32_t)snapnum * MIMIC_SAGE_FFB_GOLDEN);
  x += MIMIC_SAGE_FFB_GOLDEN;
  x = (x ^ (x >> 30)) * MIMIC_SAGE_FFB_MIX1;
  x = (x ^ (x >> 27)) * MIMIC_SAGE_FFB_MIX2;
  x ^= x >> 31;
  return x;
}

/**
 * @brief halo_uniform's deviate from its hashed bits
 *
 * SAGE parity: the top 53 bits plus one half, rounded to a double,
 * times 2^-53, with no endpoint correction (2^53 - 1 + 0.5 rounds to 2^53, giving exactly 1.0).
 */
static inline double mimic_sage_ffb_uniform_from_bits(const uint64_t x) {
  return ((double)(x >> 11) + 0.5) * (1.0 / MIMIC_SAGE_FFB_TWO_POW_53);
}

/** @brief SAGE26's halo_uniform */
static inline double mimic_sage_ffb_halo_uniform(const long long most_bound_id, const int snapnum) {
  return mimic_sage_ffb_uniform_from_bits(mimic_sage_ffb_halo_uniform_bits(most_bound_id, snapnum));
}

/**
 * @brief Refuse an EnhancedStarFormationOn other than 0 or 1, naming the parameter
 *
 * @param module_name  The calling module, for the message
 * @param value        The run's EnhancedStarFormationOn
 * @return 0 when supported; -1 otherwise
 */
static inline int mimic_sage_validate_enhanced_star_formation_on(const char *module_name,
                                                                 const int value) {
  if (value != MIMIC_SAGE_FFB_OFF && value != MIMIC_SAGE_FFB_LI24) {
    ERROR_LOG("%s: EnhancedStarFormationOn = %d is not supported: the package supports only 0 "
              "(no feedback-free bursts) and 1 (SAGE26's Li et al. 2024 classifier); 2 (the "
              "Boylan-Kolchin 2025 acceleration criterion) is not supported",
              module_name, value);
    return -1;
  }
  return 0;
}

#endif /* MIMIC_SHARED_SAGE_FFB_REGIME_H */
