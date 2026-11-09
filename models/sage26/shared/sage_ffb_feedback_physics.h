/**
 * @file sage_ffb_feedback_physics.h
 * @brief SAGE26's feedback-free-burst supernova budget, compute_sn_feedback_ffb (FIREmodeOn 1)
 *
 * Ports SAGE26's static compute_sn_feedback_ffb (R/src/model_starformation_and_feedback.c:901-972,
 * R = the staged SAGE26 source, S = that file, X = R/src/model_misc.h), which starformation_ffb
 * calls after it has committed the star formation (S:1344). The sibling of shared/sage_ffb_regime.h
 * (the classifier's arithmetic); used by sage26_ffb_star_formation. With SupernovaRecipeOn 1 (the
 * package's only supernova recipe) and FIREmodeOn 1 (which the FFB transaction requires), in
 * SAGE26's order:
 *
 * - reheated and ejected mass 0, the scaling 0 and its validity 0 (S:905-913);
 * - the redshift of the galaxy's stored snapshot ZZ[SnapNum] (S:915, which the caller looks up
 *   through mimic_sage_fire_stored_redshift) and the galaxy's own float
 *   Vvir (S:916); the scaling is valid only when Vvir > 0 and z >= 0 (S:917), and then is
 *   Muratov et al. (2015) scaling: the velocity floored at 1 km/s, (1 + z)^1.25 and the velocity
 *   term pow(x, -3.2) below 60 km/s and the division 1.0 / x the object computes for
 *   pow(x, -1.0) at and above it (S:918-923; shared/sage_fire_feedback_physics.h);
 * - only with a valid scaling, the loading capped by capped_eta_reheat (X:141-149), written
 *   to MassLoading as a float, and reheated = eta * stars (S:929-933). SAGE parity: unlike the
 *   ordinary path (S:762-764), an invalid scaling leaves MassLoading unwritten (stale) and the
 *   reheated mass 0 (S:929-934): the quirk SAGE26's comment states (S:895-899);
 * - the renormalisation of stars and reheated mass against the cold gas the caller passes (the
 *   cold gas left after the star-formation commit), under both strict guards (S:943-947);
 * - only for a positive own Vvir and a valid scaling, the Hirschmann et al. (2016) energy ejection
 *   with coupling cap (X:118-124) and the separately rounded E_FB and E_lift (S:950-958);
 * - the floor of the ejected mass at 0 (S:967).
 *
 * There is no Karpov branch, and the fixed-epsilon branch (FIREmodeOn 0, S:935-937, S:960-965) is
 * on no rung and not ported: sage26_ffb_star_formation refuses FIREmodeOn 0
 * at EnhancedStarFormationOn 1. SAGE26's assertion that the reheated mass is not negative
 * (S:940-941) aborts its run and guards a state it does not produce; it is not mirrored.
 *
 * Precision. Every local is a double; Vvir is SAGE26's float, widened at each read (S:916, S:931,
 * S:950, S:953); ColdGas is the float the caller passes, widened for the renormalisation; only
 * MassLoading is a float store. Rounding points: each expression is one C expression with
 * SAGE26's operand order and types, through c09's helpers; SAGE26's object fuses no
 * multiply-add on these lines (the oracle README's c13 table), and none of these expressions can
 * contract.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_FFB_FEEDBACK_PHYSICS_H
#define MIMIC_SHARED_SAGE_FFB_FEEDBACK_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "shared/sage_fire_feedback_physics.h"

/**
 * @brief The locals of one FFB feedback budget, for tests that compare them with the oracle
 *
 * A double is NAN, and a flag -1, where the path does not form it. `fire` holds the FIRE locals
 * (the scaling, the loading and the ejection); its `redshift` and `scaling_valid` are set
 * on every call.
 */
struct MimicSageFfbFeedbackLocals {
  struct MimicSageFireLocals fire; /**< the FIRE locals: z, validity, scaling, eta, E_FB, E_lift */
  double stars_in;                 /**< the stars formed, as passed (S:905) */
  int mass_loading_written;        /**< 1 if MassLoading was written (valid scaling, S:932) */
  double reheated_pre_renorm;      /**< eta * stars (S:933), 0 with an invalid scaling */
  int renorm_binds;                /**< the two guards of S:943 both true */
  double renorm_fac;               /**< ColdGas / (stars + reheated) (S:944) */
  double stars_out;                /**< the stars after the renormalisation (S:945) */
  double reheated_out;             /**< the reheated mass after the renormalisation (S:946) */
  int ejection_evaluated;          /**< Vvir > 0 and a valid scaling: E_FB and E_lift formed */
  double ejected_pre_floor;        /**< the ejected mass before the floor (S:958) */
  int ejected_floored;             /**< ejected < 0.0: floored at 0 (S:967) */
  double ejected_out;              /**< the ejected mass after the floor */
};

/**
 * @brief SAGE26's compute_sn_feedback_ffb (S:901-972) with SupernovaRecipeOn 1 and FIREmodeOn 1
 *
 * @param vvir                          The forming galaxy's own Vvir, as SAGE26's float
 * @param redshift                      ZZ of its stored SnapNum (mimic_sage_fire_stored_redshift)
 * @param cold_gas                      Its ColdGas after the star-formation commit (SAGE26's
 *                                      galaxies[p].ColdGas at S:943), as the float it is
 * @param feedback_reheating_epsilon    FeedbackReheatingEpsilon
 * @param feedback_ejection_efficiency  FeedbackEjectionEfficiency
 * @param eta_sn_code                   SAGE26's EtaSNcode
 * @param energy_sn_code                SAGE26's EnergySNcode
 * @param stars_inout                   In: the stars formed; out: the renormalised stars
 * @param reheated_out                  Out: the reheated mass
 * @param ejected_out                   Out: the ejected mass
 * @param mass_loading                  The galaxy's MassLoading, written only with a valid scaling
 * @param locals                        Optional, may be NULL: receives the call's locals
 */
static inline void mimic_sage_ffb_sn_feedback(const float vvir, const double redshift,
                                              const float cold_gas,
                                              const double feedback_reheating_epsilon,
                                              const double feedback_ejection_efficiency,
                                              const double eta_sn_code, const double energy_sn_code,
                                              double *stars_inout, double *reheated_out,
                                              double *ejected_out, float *mass_loading,
                                              struct MimicSageFfbFeedbackLocals *locals) {
  struct MimicSageFfbFeedbackLocals l;
  mimic_sage_fire_locals_init(&l.fire);
  l.mass_loading_written = 0;
  l.reheated_pre_renorm = NAN;
  l.renorm_binds = 0;
  l.renorm_fac = NAN;
  l.ejection_evaluated = 0;
  l.ejected_pre_floor = NAN;
  l.ejected_floored = -1;

  double stars = *stars_inout;
  double reheated_mass = 0.0;
  double ejected_mass = 0.0;
  l.stars_in = stars;

  /* The FIRE scaling, computed once (S:910-924): 0 and invalid unless the galaxy's own Vvir is
   * positive and the stored snapshot's redshift is not negative (S:915-917) */
  double fire_scaling = 0.0;
  const double z = redshift;
  const double vc = vvir;
  const int fire_scaling_valid = vc > 0.0 && z >= 0.0;
  if (fire_scaling_valid) {
    fire_scaling = mimic_sage_fire_scaling(vvir, z, &l.fire);
  }
  l.fire.redshift = z;
  l.fire.scaling_valid = fire_scaling_valid;
  l.fire.scaling = fire_scaling;

  if (fire_scaling_valid) {
    /* SAGE parity: the capped loading is written to MassLoading as a float, and the reheated mass
     * formed, only with a valid scaling (S:929-933); an invalid scaling leaves MassLoading as it
     * was and the reheated mass 0, where the ordinary path writes MassLoading 0 (S:762-764) */
    const double eta_reheat = mimic_sage_capped_eta_reheat(
        feedback_reheating_epsilon * fire_scaling, vvir, eta_sn_code, energy_sn_code, &l.fire);
    *mass_loading = (float)eta_reheat;
    reheated_mass = eta_reheat * stars;
    l.mass_loading_written = 1;
  }
  l.reheated_pre_renorm = reheated_mass;

  /* SAGE parity: the renormalisation is against the cold gas already depleted by the
   * star-formation commit, under both strict guards (S:943-947) */
  if ((stars + reheated_mass) > cold_gas && (stars + reheated_mass) > 0.0) {
    const double fac = cold_gas / (stars + reheated_mass);
    stars *= fac;
    reheated_mass *= fac;
    l.renorm_binds = 1;
    l.renorm_fac = fac;
  }

  /* The energy ejection, only for a positive own Vvir and a valid scaling (S:949-958): the FIRE
   * helpers' coupling cap and the separately rounded E_FB and E_lift */
  if (vvir > 0.0) {
    if (fire_scaling_valid) {
      const double coupling =
          mimic_sage_sn_energy_coupling(fire_scaling, feedback_ejection_efficiency, &l.fire);
      ejected_mass = mimic_sage_fire_energy_ejection(coupling, stars, reheated_mass, vc,
                                                     eta_sn_code, energy_sn_code, &l.fire);
      l.ejection_evaluated = 1;
    }
  }
  l.ejected_pre_floor = ejected_mass;
  l.ejected_floored = ejected_mass < 0.0;
  if (ejected_mass < 0.0) {
    ejected_mass = 0.0;
  }

  *stars_inout = stars;
  *reheated_out = reheated_mass;
  *ejected_out = ejected_mass;

  if (locals != NULL) {
    l.stars_out = stars;
    l.reheated_out = reheated_mass;
    l.ejected_out = ejected_mass;
    *locals = l;
  }
}

#endif /* MIMIC_SHARED_SAGE_FFB_FEEDBACK_PHYSICS_H */
