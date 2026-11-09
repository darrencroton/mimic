/**
 * @file    sage_halo_concentration.h
 * @brief   SAGE26's NFW halo concentration for ConcentrationOn 0 and 3: the Vmax/Vvir inversion,
 *          the mode-3 velocity selection and the ConcentrationOn validator
 *
 * Implements SAGE26's concentration_from_vmax_vvir, which inverts
 * (Vmax / Vvir)^2 = A c / mu(c), mu(c) = ln(1 + c) - c / (1 + c), by a fixed 60-step bisection on
 * [1, 200], and get_halo_concentration for the two modes the package supports: ConcentrationOn 0
 * returns 0, and ConcentrationOn 3 inverts the galaxy's Vmax/Vvir (Type 0) or its infall-frozen
 * infallVmax/infallVvir (any other Type). Modes 1 (the Ishiyama et al. 2021 table) and 2
 * (Vmax/Vvir for every Type) are not supported and are refused by
 * mimic_sage_validate_concentration_on.
 *
 * Each helper takes its inputs as arguments and reads no module state. The caller narrows each
 * core-owned double velocity to the float SAGE26 holds and passes the Type the galaxy had before
 * progenitor joining, as SAGE26 calls get_halo_concentration before it sets Type = 0.
 *
 * SAGE26's quirks, kept: exactly 60 iterations with no tolerance; the strict f_mid < ratio_sq, so
 * a tie moves c_hi; the literal 0.21621 (SAGE26's comments say 0.2162); 0 for ratio_sq below 1
 * and no rejection of a large ratio (a ratio_sq beyond f(200) saturates at the upper bound). A c /
 * mu(c) is not monotonic on [1, about 2.16], so every ratio_sq from 1 up to f(2.5546875) returns
 * exactly 1.0 and no concentration strictly between 1 and 2.5546875 is ever produced. NaN
 * velocities pass the guards, so a NaN ratio_sq returns 1.0.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_HALO_CONCENTRATION_H
#define MIMIC_SHARED_SAGE_HALO_CONCENTRATION_H

#include <math.h>
#include <stddef.h>

#include "error.h"

/** ConcentrationOn 0: no concentration */
#define MIMIC_SAGE_CONCENTRATION_OFF 0
/** ConcentrationOn 3: the Vmax/Vvir inversion, infall-frozen for a former satellite */
#define MIMIC_SAGE_CONCENTRATION_VMAX_VVIR 3

/* SAGE parity: SAGE26's literal 0.21621 (its comments say 0.2162) */
#define MIMIC_SAGE_CONCENTRATION_A 0.21621
/* SAGE parity: the initial bracket [1, 200], with no rejection of a ratio outside f's range */
#define MIMIC_SAGE_CONCENTRATION_C_LO 1.0
#define MIMIC_SAGE_CONCENTRATION_C_HI 200.0
/* SAGE parity: exactly 60 bisection steps, with no convergence test */
#define MIMIC_SAGE_CONCENTRATION_ITERATIONS 60

/**
 * @brief SAGE26's locals of one concentration_from_vmax_vvir call, for the tests
 *
 * A flag is -1 where the call returns before evaluating it. A value the call never forms holds
 * NaN with its absent marker 1: ratio_sq after the velocity guard returns, c_lo and c_hi after
 * either early return.
 */
struct MimicSageConcentrationLocals {
  int guard_returned;  /**< 1: Vvir <= 0.0 || Vmax <= 0.0 returned 0 */
  int below_one;       /**< 1: ratio_sq < 1.0 returned 0; -1 when not evaluated */
  int iterations;      /**< Bisection steps taken: 60, or 0 after an early return */
  int ratio_sq_absent; /**< 1 when ratio_sq was not formed */
  double ratio_sq;     /**< (Vmax / Vvir) * (Vmax / Vvir) */
  int bracket_absent;  /**< 1 when the bisection did not run */
  double c_lo;         /**< The lower bracket after the last step */
  double c_hi;         /**< The upper bracket after the last step */
};

/**
 * @brief The velocity pair the ConcentrationOn 3 dispatch passes to the inversion
 */
struct MimicSageConcentrationPair {
  int infall;  /**< 0: the current Vmax and Vvir (Type 0); 1: infallVmax and infallVvir */
  double vmax; /**< The selected float Vmax widened to double */
  double vvir; /**< The selected float Vvir widened to double */
};

/**
 * @brief The locals of one get_halo_concentration call, for the tests
 *
 * With ConcentrationOn 0 nothing is formed: selected_infall is -1, pair_absent is 1 and the
 * inversion's locals report an early return with nothing formed.
 */
struct MimicSageHaloConcentrationLocals {
  int selected_infall;                           /**< -1 (mode 0), 0 current pair, 1 infall pair */
  int pair_absent;                               /**< 1 when no pair was selected (mode 0) */
  struct MimicSageConcentrationPair pair;        /**< The selected pair */
  struct MimicSageConcentrationLocals inversion; /**< The inversion's locals */
};

/**
 * @brief Refuse a ConcentrationOn the package does not support, naming the parameter
 *
 * The package supports SAGE26's modes 0 (off) and 3 (the Vmax/Vvir inversion). Mode 1 (the
 * Ishiyama et al. 2021 table) and mode 2 (Vmax/Vvir for every Type) are not supported; there is
 * no fallback default.
 *
 * @param module_name  The calling module, for the error message
 * @param value        The loaded ConcentrationOn
 * @return 0 for 0 or 3; -1 (with an error naming the parameter) otherwise
 */
static inline int mimic_sage_validate_concentration_on(const char *module_name, const int value) {
  if (value != MIMIC_SAGE_CONCENTRATION_OFF && value != MIMIC_SAGE_CONCENTRATION_VMAX_VVIR) {
    ERROR_LOG("%s: ConcentrationOn = %d is not supported: the package supports only 0 (no "
              "concentration) and 3 (the Vmax/Vvir inversion); "
              "modes 1 (Ishiyama et al. 2021 table) and 2 are not supported",
              module_name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief Set the inversion's locals to "nothing evaluated, nothing formed"
 */
static inline void mimic_sage_concentration_locals_reset(struct MimicSageConcentrationLocals *l) {
  l->guard_returned = 0;
  l->below_one = -1;
  l->iterations = 0;
  l->ratio_sq_absent = 1;
  l->ratio_sq = NAN;
  l->bracket_absent = 1;
  l->c_lo = NAN;
  l->c_hi = NAN;
}

/**
 * @brief SAGE26's concentration_from_vmax_vvir
 *
 * @param Vmax    Maximum circular velocity, as the double SAGE26 receives
 * @param Vvir    Virial velocity, as the double SAGE26 receives
 * @param locals  Receives SAGE26's locals; may be NULL
 * @return The concentration: 0 for a non-positive velocity or ratio_sq < 1, otherwise the
 *         midpoint of the bracket after 60 bisection steps
 */
static inline double
mimic_sage_concentration_from_vmax_vvir(const double Vmax, const double Vvir,
                                        struct MimicSageConcentrationLocals *locals) {
  if (locals != NULL) {
    mimic_sage_concentration_locals_reset(locals);
  }

  // SAGE parity: Vvir tested first, then Vmax; a NaN velocity passes
  if (Vvir <= 0.0 || Vmax <= 0.0) {
    if (locals != NULL) {
      locals->guard_returned = 1;
    }
    return 0.0;
  }

  const double ratio_sq = (Vmax / Vvir) * (Vmax / Vvir);
  if (locals != NULL) {
    locals->ratio_sq_absent = 0;
    locals->ratio_sq = ratio_sq;
  }

  const double A = MIMIC_SAGE_CONCENTRATION_A;

  double c_lo = MIMIC_SAGE_CONCENTRATION_C_LO, c_hi = MIMIC_SAGE_CONCENTRATION_C_HI;

  // SAGE parity: only a ratio below 1 is rejected; a large ratio saturates at c_hi
  if (ratio_sq < 1.0) {
    if (locals != NULL) {
      locals->below_one = 1;
    }
    return 0.0;
  }
  if (locals != NULL) {
    locals->below_one = 0;
  }

  // SAGE parity: exactly 60 steps and the strict f_mid < ratio_sq, on an f that is not monotonic
  // below about 2.16 (see this file's header)
  int iterations = 0;
  for (int iter = 0; iter < MIMIC_SAGE_CONCENTRATION_ITERATIONS; iter++) {
    const double c_mid = 0.5 * (c_lo + c_hi);
    const double mu = log(1.0 + c_mid) - c_mid / (1.0 + c_mid);
    const double f_mid = A * c_mid / mu;
    if (f_mid < ratio_sq) {
      c_lo = c_mid;
    } else {
      c_hi = c_mid;
    }
    iterations++;
  }
  if (locals != NULL) {
    locals->iterations = iterations;
    locals->bracket_absent = 0;
    locals->c_lo = c_lo;
    locals->c_hi = c_hi;
  }
  return 0.5 * (c_lo + c_hi);
}

/**
 * @brief SAGE26's ConcentrationOn 3 velocity selection in get_halo_concentration
 *
 * @param type         The galaxy's Type at the call: the module passes PreviousType, the Type
 *                     before progenitor joining
 * @param vmax         The galaxy's float Vmax
 * @param vvir         The galaxy's float Vvir
 * @param infall_vmax  The galaxy's float infallVmax
 * @param infall_vvir  The galaxy's float infallVvir
 * @return The selected pair, each float widened to double separately
 */
static inline struct MimicSageConcentrationPair
mimic_sage_concentration_mode3_pair(const int type, const float vmax, const float vvir,
                                    const float infall_vmax, const float infall_vvir) {
  struct MimicSageConcentrationPair pair;
  // SAGE parity: only Type 0 takes the current pair; a former satellite returning to the centre
  // takes its infall-frozen pair
  if (type == 0) {
    pair.infall = 0;
    pair.vmax = vmax;
    pair.vvir = vvir;
  } else {
    pair.infall = 1;
    pair.vmax = infall_vmax;
    pair.vvir = infall_vvir;
  }
  return pair;
}

/**
 * @brief SAGE26's get_halo_concentration for ConcentrationOn 0 and 3
 *
 * The redshift SAGE26 passes is not read by either mode. Any other mode returns NaN: the caller
 * validates ConcentrationOn first (mimic_sage_validate_concentration_on).
 *
 * @param concentration_on  ConcentrationOn, 0 or 3
 * @param type              The galaxy's Type at the call (see mimic_sage_concentration_mode3_pair)
 * @param vmax              The galaxy's float Vmax
 * @param vvir              The galaxy's float Vvir
 * @param infall_vmax       The galaxy's float infallVmax
 * @param infall_vvir       The galaxy's float infallVvir
 * @param locals            Receives SAGE26's locals; may be NULL
 * @return The concentration as the double SAGE26 returns; the caller stores it as a float
 */
static inline double
mimic_sage_get_halo_concentration(const int concentration_on, const int type, const float vmax,
                                  const float vvir, const float infall_vmax,
                                  const float infall_vvir,
                                  struct MimicSageHaloConcentrationLocals *locals) {
  struct MimicSageConcentrationLocals *inversion = locals != NULL ? &locals->inversion : NULL;
  if (locals != NULL) {
    locals->selected_infall = -1;
    locals->pair_absent = 1;
    locals->pair.infall = -1;
    locals->pair.vmax = NAN;
    locals->pair.vvir = NAN;
    mimic_sage_concentration_locals_reset(inversion);
  }

  // SAGE parity: mode 0 returns 0
  if (concentration_on == MIMIC_SAGE_CONCENTRATION_OFF) {
    return 0.0;
  }
  if (concentration_on != MIMIC_SAGE_CONCENTRATION_VMAX_VVIR) {
    return NAN;
  }

  const struct MimicSageConcentrationPair pair =
      mimic_sage_concentration_mode3_pair(type, vmax, vvir, infall_vmax, infall_vvir);
  if (locals != NULL) {
    locals->selected_infall = pair.infall;
    locals->pair_absent = 0;
    locals->pair = pair;
  }
  return mimic_sage_concentration_from_vmax_vvir(pair.vmax, pair.vvir, inversion);
}

#endif /* MIMIC_SHARED_SAGE_HALO_CONCENTRATION_H */
