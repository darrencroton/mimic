/**
 * @file    sage_ffb_regime.h
 * @brief   SAGE26's feedback-free-burst (FFB) classification arithmetic for
 *          EnhancedStarFormationOn 1: the Li et al. (2024) threshold mass and FFB fraction, the
 *          halo's SplitMix64 draw, and the EnhancedStarFormationOn validator
 *
 * Ports, from R/src/model_regimes.c (R = the staged SAGE26 source):
 *
 * - mimic_sage_ffb_threshold_mass(): calculate_ffb_threshold_mass (:298-313), Li et al. (2024)
 *   eq. 2 in code units (1e10 Msun/h):
 *
 *     z_norm            = (1.0 + z) / 10.0                                        (:309)
 *     log_Mvir_ffb_code = 0.8 + log10(h) + FFB_THRESHOLD_SLOPE * log10(z_norm)    (:310)
 *     return pow(10.0, log_Mvir_ffb_code)                                         (:312)
 *
 *   with FFB_THRESHOLD_SLOPE the double -6.2 (:50).
 * - mimic_sage_ffb_fraction(): calculate_ffb_fraction (:321-341) at EnhancedStarFormationOn 1,
 *   Li et al. (2024) eq. 3:
 *
 *     Mvir_ffb = the threshold at z                                               (:330)
 *     Mvir <= 0.0 || Mvir_ffb <= 0.0      -> 0                                    (:332-334)
 *     x        = log10(Mvir / Mvir_ffb) / 0.15                                    (:337-338)
 *     return 1.0 / (1.0 + exp(-x))                                                (:339)
 *
 *   Its EnhancedStarFormationOn == 0 return (:326-328) is not ported: in the package that
 *   setting is the caller's gate (sage26_determine_ffb_regime writes nothing), as SAGE26 never
 *   calls the classifier then (R/src/core_build_model.c:425).
 * - mimic_sage_ffb_halo_uniform_bits() and mimic_sage_ffb_uniform_from_bits(): SAGE26's static
 *   halo_uniform (:151-159), the SplitMix64 (Steele, Lea & Flood 2014) hash of a halo's 64-bit
 *   MostBoundID and the snapshot, and its deviate; mimic_sage_ffb_halo_uniform() composes the
 *   two. This is SAGE26's own draw, transcribed with its unsigned wrap-around, its conversion of
 *   the signed ID, its (uint32_t) snapshot, its constants, shifts and conversion order. It is not
 *   the package's CGM keyed draw (shared/sage_keyed_draw.h), and no rand()
 *   is called.
 * - mimic_sage_validate_enhanced_star_formation_on(): the required EnhancedStarFormationOn, 0
 *   (off, SAGE26 R0-R5) or 1 (the Li et al. 2024 classifier, R6). Mode 2, the Boylan-Kolchin
 *   (2025) acceleration criterion (:241-288), is not ported and is
 *   refused with every other value.
 *
 * Each helper takes its inputs as arguments and reads no module state. The caller passes the
 * row's core Mvir narrowed to the float SAGE26 holds (R/src/core_allvars.h:129) and widened, the
 * current redshift (SAGE26's Zcurr = ZZ[halo_snapnum], R/src/core_build_model.c:408) and the
 * run's Hubble_h.
 *
 * SAGE26's quirks, kept: the threshold is formed before the guards and the Mvir test comes first
 * (:330-332; both are pure, so the order is not observable); a NaN Mvir or threshold passes the
 * guards and gives a NaN fraction, which compares false with every draw; z = -1 gives z_norm 0,
 * log10 -inf and an infinite threshold, so every finite Mvir has fraction 0 through x = -inf,
 * not through the guard; a redshift below -1 gives a NaN threshold. The draw keeps its
 * arithmetic exactly as written, with no endpoint correction: (x >> 11) + 0.5 is rounded to a
 * double, so the largest 53-bit value 2^53 - 1 rounds to 2^53 and its deviate is exactly 1.0.
 *
 * Rounding points. SAGE26's object (model_regimes.o, read with objdump -d -l -r; the oracle
 * README's "The c13 Group") fuses the threshold's log term at all three sites it is compiled:
 * fmadd(log10(z_norm), -6.2, log10(h) + 0.8) at 0x47c (inlined in
 * determine_and_store_ffb_regime), 0x860 (calculate_ffb_fraction) and 0x908
 * (calculate_ffb_threshold_mass). The package writes that site as an explicit fma(), which is the
 * same single rounding on every platform whose fma is correctly rounded, as C requires. SAGE26's
 * object computes pow(10.0, log_Mvir_ffb_code) as a call of __exp10 at the same three sites; the
 * package writes pow(10.0, ...), which Mimic's -O2 build on Apple clang also compiles to __exp10
 * and its -O0 unit-test build to pow. The equality was established only for the redshifts of
 * mini-Millennium's snapshots at Hubble_h 0.73 and of the classifier's oracle fixtures, where
 * the two give the same double (the executed check in the oracle README). It does not hold for
 * every simulation
 * package: at micro-Uchuu's snapshot 3 (z 10.458691417440129, Hubble_h 0.6774) the argument
 * 0x1.0e8c8da54b3b8p-2 gives pow 0x1.d6610c1d85ba4p+0 and __exp10 0x1.d6610c1d85ba3p+0. The
 * fraction's exp(-x) is compiled as exp(log10(Mvir / Mvir_ffb) / -0.15), the division by the
 * negated constant (0x4a8, 0x88c), and x is never formed. That equals the source form bit for
 * bit for every non-NaN x (round-to-nearest division is sign-symmetric), but not for a NaN: a
 * negation flips its sign bit and the division carries it, so a source-form exp(-x) compiled
 * without that fold (Mimic's -O0 unit-test build) returns a NaN of the other sign. The package
 * therefore writes the object's form, and reports SAGE26's local x beside it. No other
 * expression has a multiply feeding an add or subtract. The draw's ((double)(x >> 11) + 0.5) *
 * 2^-53 is an add feeding a multiply, which no contraction fuses (ucvtf, fadd, fmul at
 * 0x444-0x44c).
 *
 * Reference: Li et al. (2024), eqs. 2-3, and Steele, Lea & Flood (2014), as SAGE26 cites them
 *            (R/src/model_regimes.c:46-50, :146, :167)
 *
 * @note Used by sage26_determine_ffb_regime and by its unit and oracle tests;
 *       _tests/test_unit_sage_ffb_regime.c tests each helper against literals and SAGE26's
 *       expressions written in the test.
 */

#ifndef MIMIC_SHARED_SAGE_FFB_REGIME_H
#define MIMIC_SHARED_SAGE_FFB_REGIME_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "error.h"

/** EnhancedStarFormationOn 0: no FFB classification or FFB star formation (SAGE26 R0-R5) */
#define MIMIC_SAGE_FFB_OFF 0
/** EnhancedStarFormationOn 1: the Li et al. (2024) classifier (SAGE26 R6, its default) */
#define MIMIC_SAGE_FFB_LI24 1

/* SAGE parity: the double FFB_THRESHOLD_SLOPE, -6.2 (R/src/model_regimes.c:50) */
#define MIMIC_SAGE_FFB_THRESHOLD_SLOPE (-6.2)
/* SAGE parity: the threshold's normalisation at z = 9, 10^10.8 Msun as the literal 0.8 of the
 * code-unit log (R/src/model_regimes.c:310) */
#define MIMIC_SAGE_FFB_THRESHOLD_LOG_NORM 0.8
/* SAGE parity: the sigmoid width delta_log_M, 0.15 dex (R/src/model_regimes.c:337) */
#define MIMIC_SAGE_FFB_DELTA_LOG_M 0.15

/* SAGE parity: halo_uniform's constants (R/src/model_regimes.c:153-158), SplitMix64's golden
 * increment and its two finaliser multipliers, and 2^53 as the double literal of :158 */
#define MIMIC_SAGE_FFB_GOLDEN 0x9E3779B97F4A7C15ULL
#define MIMIC_SAGE_FFB_MIX1 0xBF58476D1CE4E5B9ULL
#define MIMIC_SAGE_FFB_MIX2 0x94D049BB133111EBULL
#define MIMIC_SAGE_FFB_TWO_POW_53 9007199254740992.0

/** SAGE26's locals of one calculate_ffb_threshold_mass call (each double as SAGE26 forms it) */
struct MimicSageFfbThresholdLocals {
  double z_norm;            /**< (1.0 + z) / 10.0 (:309) */
  double log_Mvir_ffb_code; /**< 0.8 + log10(h) + FFB_THRESHOLD_SLOPE * log10(z_norm) (:310) */
};

/** SAGE26's locals of one calculate_ffb_fraction call */
struct MimicSageFfbFractionLocals {
  struct MimicSageFfbThresholdLocals threshold; /**< The threshold's locals (:330) */
  double Mvir_ffb;                              /**< The threshold mass, 1e10 Msun/h (:330) */
  int guard_returned; /**< 1 when Mvir <= 0.0 || Mvir_ffb <= 0.0 returned 0 (:332-334) */
  double x;           /**< log10(Mvir / Mvir_ffb) / 0.15 (:338); NaN when the guard returned */
};

/**
 * @brief SAGE26's calculate_ffb_threshold_mass (R/src/model_regimes.c:298-313)
 *
 * @param z         The redshift (SAGE26 passes Zcurr = ZZ[halo_snapnum])
 * @param hubble_h  The run's Hubble_h
 * @param locals    Optional (NULL): receives z_norm and log_Mvir_ffb_code
 * @return The FFB threshold virial mass in 1e10 Msun/h
 */
static inline double mimic_sage_ffb_threshold_mass(const double z, const double hubble_h,
                                                   struct MimicSageFfbThresholdLocals *locals) {
  /* :308 */
  const double h = hubble_h;
  /* :309 */
  const double z_norm = (1.0 + z) / 10.0;
  /* SAGE parity (:310): SAGE26's object fuses the slope's product into the sum
   * (model_regimes.o 0x47c, 0x860, 0x908: fmadd of log10(z_norm), -6.2 and log10(h) + 0.8); the
   * explicit fma() is that one rounding on every platform */
  const double log_Mvir_ffb_code = fma(MIMIC_SAGE_FFB_THRESHOLD_SLOPE, log10(z_norm),
                                       MIMIC_SAGE_FFB_THRESHOLD_LOG_NORM + log10(h));
  if (locals != NULL) {
    locals->z_norm = z_norm;
    locals->log_Mvir_ffb_code = log_Mvir_ffb_code;
  }
  /* :312; SAGE26's object calls __exp10 here. pow equals it at every mini-Millennium snapshot
   * redshift (Hubble_h 0.73) and every redshift of the classifier's oracle fixtures, but not at
   * every argument of every simulation package (one ulp apart at micro-Uchuu's snapshot 3;
   * oracle README, "The c13 Group") */
  return pow(10.0, log_Mvir_ffb_code);
}

/**
 * @brief SAGE26's calculate_ffb_fraction at EnhancedStarFormationOn 1
 *        (R/src/model_regimes.c:321-341, without the :326-328 return, the caller's gate)
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
  /* :330 */
  const double Mvir_ffb =
      mimic_sage_ffb_threshold_mass(z, hubble_h, locals != NULL ? &locals->threshold : NULL);
  if (locals != NULL) {
    locals->Mvir_ffb = Mvir_ffb;
    locals->guard_returned = 0;
    locals->x = NAN;
  }

  /* SAGE parity (:332-334): only a non-positive Mvir or threshold returns 0; a NaN passes */
  if (Mvir <= 0.0 || Mvir_ffb <= 0.0) {
    if (locals != NULL) {
      locals->guard_returned = 1;
    }
    return 0.0;
  }

  /* :337-338: x is SAGE26's local, log10(Mvir / Mvir_ffb) / delta_log_M */
  const double delta_log_M = MIMIC_SAGE_FFB_DELTA_LOG_M;
  const double log_ratio = log10(Mvir / Mvir_ffb);
  const double x = log_ratio / delta_log_M;
  if (locals != NULL) {
    locals->x = x;
  }
  /* SAGE parity (:339): SAGE26's object computes exp(-x) as exp(log10(Mvir / Mvir_ffb) / -0.15),
   * a division by the negated width (model_regimes.o 0x4a8, 0x88c), and never forms x. For every
   * non-NaN x the two are equal bit for bit (round-to-nearest division is sign-symmetric), but a
   * negation flips a NaN's sign bit while the division carries it, so the package writes the
   * object's form and returns SAGE26's NaN for a NaN Mvir or threshold */
  const double f_ffb = 1.0 / (1.0 + exp(log_ratio / -delta_log_M));
  return f_ffb;
}

/**
 * @brief The 64 hashed bits of SAGE26's halo_uniform (R/src/model_regimes.c:153-157)
 *
 * The signed ID is converted to uint64_t (two's complement, so a negative ID keeps its bits), the
 * snapshot to uint32_t and then uint64_t, and every product and sum wraps modulo 2^64.
 *
 * @param most_bound_id  The halo's MostBoundID (SAGE26's long long galaxies[p].MostBoundID)
 * @param snapnum        The current snapshot (SAGE26's halo_snapnum)
 * @return x after the final xorshift, before the >> 11 of :158
 */
static inline uint64_t mimic_sage_ffb_halo_uniform_bits(const long long most_bound_id,
                                                        const int snapnum) {
  /* :153 */
  uint64_t x = (uint64_t)most_bound_id ^ ((uint64_t)(uint32_t)snapnum * MIMIC_SAGE_FFB_GOLDEN);
  /* :154 */
  x += MIMIC_SAGE_FFB_GOLDEN;
  /* :155 */
  x = (x ^ (x >> 30)) * MIMIC_SAGE_FFB_MIX1;
  /* :156 */
  x = (x ^ (x >> 27)) * MIMIC_SAGE_FFB_MIX2;
  /* :157 */
  x ^= x >> 31;
  return x;
}

/**
 * @brief halo_uniform's deviate from its hashed bits (R/src/model_regimes.c:158)
 *
 * SAGE parity (R/src/model_regimes.c:158): the top 53 bits plus one half, rounded to a double,
 * times 2^-53, with no endpoint correction (2^53 - 1 + 0.5 rounds to 2^53, giving exactly 1.0).
 */
static inline double mimic_sage_ffb_uniform_from_bits(const uint64_t x) {
  /* :158 */
  return ((double)(x >> 11) + 0.5) * (1.0 / MIMIC_SAGE_FFB_TWO_POW_53);
}

/** @brief SAGE26's halo_uniform (R/src/model_regimes.c:151-159) */
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
    ERROR_LOG("%s: EnhancedStarFormationOn = %d is not supported: the package ports only 0 (no "
              "feedback-free bursts) and 1 (SAGE26's Li et al. 2024 classifier); 2 (the "
              "Boylan-Kolchin 2025 acceleration criterion) is not ported",
              module_name, value);
    return -1;
  }
  return 0;
}

#endif /* MIMIC_SHARED_SAGE_FFB_REGIME_H */
