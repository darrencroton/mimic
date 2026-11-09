/**
 * @file sage_ffb_feedback_physics.h
 * @brief SAGE26's feedback-free-burst supernova budget, compute_sn_feedback_ffb (FIREmodeOn 1)
 *
 * Implements SAGE26's static compute_sn_feedback_ffb, which starformation_ffb calls after it has
 * committed the star formation; used by sage26_ffb_star_formation. The sibling of
 * shared/sage_ffb_regime.h (the classifier's arithmetic). With SupernovaRecipeOn 1 (the package's
 * only supernova recipe) and FIREmodeOn 1 (which the FFB transaction requires), in SAGE26's order:
 *
 * - reheated and ejected mass 0, the scaling 0 and its validity 0;
 * - the redshift of the galaxy's stored snapshot (which the caller looks up through
 *   mimic_sage_fire_stored_redshift) and the galaxy's own Vvir; the scaling is valid only when
 *   Vvir > 0 and z >= 0, and then is the Muratov et al. (2015) scaling of
 *   shared/sage_fire_feedback_physics.h;
 * - only with a valid scaling, the loading capped by capped_eta_reheat, written to MassLoading as
 *   a float, and reheated = eta * stars. An invalid scaling leaves MassLoading unwritten (stale)
 *   and the reheated mass 0, a quirk SAGE26's own comment states;
 * - the renormalisation of stars and reheated mass against the cold gas the caller passes (the
 *   cold gas left after the star-formation commit), under both strict guards;
 * - only for a positive own Vvir and a valid scaling, the Hirschmann et al. (2016) energy ejection
 *   with its coupling cap and the separately rounded E_FB and E_lift;
 * - the floor of the ejected mass at 0.
 *
 * There is no Karpov branch, and the fixed-epsilon branch (FIREmodeOn 0) is not supported:
 * sage26_ffb_star_formation refuses FIREmodeOn 0 at EnhancedStarFormationOn 1. SAGE26's assertion
 * that the reheated mass is not negative guards a state it does not produce and is not mirrored.
 *
 * Every local is a double; Vvir and ColdGas are SAGE26's floats, widened at each read; only
 * MassLoading is a float store.
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
 * @brief The locals of one FFB feedback budget, for tests that compare them
 *
 * A double is NAN, and a flag -1, where the path does not form it. `fire` holds the FIRE locals
 * (the scaling, the loading and the ejection); its `redshift` and `scaling_valid` are set
 * on every call.
 */
struct MimicSageFfbFeedbackLocals {
  struct MimicSageFireLocals fire; /**< the FIRE locals: z, validity, scaling, eta, E_FB, E_lift */
  double stars_in;                 /**< the stars formed, as passed */
  int mass_loading_written;        /**< 1 if MassLoading was written (valid scaling) */
  double reheated_pre_renorm;      /**< eta * stars, 0 with an invalid scaling */
  int renorm_binds;                /**< both renormalisation guards true */
  double renorm_fac;               /**< ColdGas / (stars + reheated) */
  double stars_out;                /**< the stars after the renormalisation */
  double reheated_out;             /**< the reheated mass after the renormalisation */
  int ejection_evaluated;          /**< Vvir > 0 and a valid scaling: E_FB and E_lift formed */
  double ejected_pre_floor;        /**< the ejected mass before the floor */
  int ejected_floored;             /**< ejected < 0.0: floored at 0 */
  double ejected_out;              /**< the ejected mass after the floor */
};

/**
 * @brief SAGE26's compute_sn_feedback_ffb with SupernovaRecipeOn 1 and FIREmodeOn 1
 *
 * @param vvir                          The forming galaxy's own Vvir, as SAGE26's float
 * @param redshift                      ZZ of its stored SnapNum (mimic_sage_fire_stored_redshift)
 * @param cold_gas                      Its ColdGas after the star-formation commit, as the
 *                                      float it is
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

  /* The FIRE scaling, computed once: 0 and invalid unless the galaxy's own Vvir is positive and
   * the stored snapshot's redshift is not negative */
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
    /* SAGE parity: MassLoading is written, and the reheated mass formed, only with a valid
     * scaling; an invalid scaling leaves MassLoading as it was and the reheated mass 0 */
    const double eta_reheat = mimic_sage_capped_eta_reheat(
        feedback_reheating_epsilon * fire_scaling, vvir, eta_sn_code, energy_sn_code, &l.fire);
    *mass_loading = (float)eta_reheat;
    reheated_mass = eta_reheat * stars;
    l.mass_loading_written = 1;
  }
  l.reheated_pre_renorm = reheated_mass;

  /* SAGE parity: the renormalisation is against the cold gas already depleted by the
   * star-formation commit, under both strict guards */
  if ((stars + reheated_mass) > cold_gas && (stars + reheated_mass) > 0.0) {
    const double fac = cold_gas / (stars + reheated_mass);
    stars *= fac;
    reheated_mass *= fac;
    l.renorm_binds = 1;
    l.renorm_fac = fac;
  }

  /* The energy ejection, only for a positive own Vvir and a valid scaling: the FIRE helpers'
   * coupling cap and the separately rounded E_FB and E_lift */
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
