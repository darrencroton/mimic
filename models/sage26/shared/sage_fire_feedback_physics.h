/**
 * @file sage_fire_feedback_physics.h
 * @brief SAGE26's FIRE supernova-feedback helpers (FIREmodeOn 1): the Muratov et al. (2015)
 *        velocity and redshift scaling, the capped reheating loading and the Hirschmann et al.
 *        (2016) energy-based ejection
 *
 * Implements what SAGE26's FIREmodeOn 1 switches on in its supernova feedback
 * (starformation_and_feedback, starformation_ffb and the burst's copy in
 * collisional_starburst_recipe), and SAGE26's capped_eta_reheat and sn_energy_coupling:
 *
 * - mimic_sage_fire_stored_redshift(): the redshift ZZ[SnapNum] of the galaxy's stored snapshot,
 *   never the current halo snapshot's or a substep's; a SnapNum outside the redshift table is
 *   refused (SAGE26 has no bounds check there). Also used by the cold streams of
 *   shared/sage_cooling_physics.h, so its log names no single caller.
 * - mimic_sage_fire_scaling(): the Muratov et al. (2015) scaling, zero unless the host's velocity
 *   is positive and the redshift not negative; otherwise (1 + z)^1.25 * (max(V, 1) / 60)^beta
 *   with beta = -3.2 below 60 km/s and -1.0 at and above.
 * - mimic_sage_capped_eta_reheat(): the reheating loading capped at
 *   2 * EtaSNcode * EnergySNcode / V^2, unchanged for V <= 0.
 * - mimic_sage_sn_energy_coupling(): FeedbackEjectionEfficiency * scaling capped at 2.0.
 * - mimic_sage_fire_energy_ejection(): the Hirschmann et al. (2016) energy-based ejection.
 *
 * SAGE26 holds Vvir as a float, so the caller narrows Mimic's double halo Vvir to float once and
 * every FIRE local widens that float to double.
 *
 * @note Used by sage26_calculate_supernova_feedback (the disk path), by the feedback-free-burst
 *       budget (shared/sage_ffb_feedback_physics.h, MassLoading written only with a valid
 *       scaling) and by mimic_apply_collisional_starburst (shared/sage_starburst_physics.h, the
 *       burst host's own Vvir and stored SnapNum, no MassLoading write).
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_FIRE_FEEDBACK_PHYSICS_H
#define MIMIC_SHARED_SAGE_FIRE_FEEDBACK_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "error.h"
#include "types.h"

/** @brief FIREmodeOn 0: SAGE26's fixed-epsilon Croton et al. (2016) feedback */
#define MIMIC_SAGE_FIRE_MODE_OFF 0
/** @brief FIREmodeOn 1: the FIRE scaling, capped loading and energy-based ejection */
#define MIMIC_SAGE_FIRE_MODE_ON 1

/** @brief SAGE26's FIRE_V_CRIT_KMS: the velocity separating the two slopes [km/s] */
#define MIMIC_SAGE_FIRE_V_CRIT_KMS 60.0
/** @brief SAGE26's FIRE_REDSHIFT_EXPONENT: eta ~ (1 + z)^1.25 */
#define MIMIC_SAGE_FIRE_REDSHIFT_EXPONENT 1.25
/** @brief The low-velocity slope, below MIMIC_SAGE_FIRE_V_CRIT_KMS */
#define MIMIC_SAGE_FIRE_LOW_V_EXPONENT -3.2
/** @brief The velocity floor of the scaling only [km/s] */
#define MIMIC_SAGE_FIRE_VELOCITY_FLOOR_KMS 1.0
/** @brief SAGE26's MAX_SN_ENERGY_COUPLING: the cap on the coupling and the loading budget */
#define MIMIC_SAGE_MAX_SN_ENERGY_COUPLING 2.0

/**
 * @brief Refuse a FIREmodeOn the package does not support
 *
 * FIREmodeOn is a required package parameter, 0 or 1; each module that reads it loads it with the
 * required-parameter path and passes it here.
 *
 * @param module_name  The calling module, for the error message
 * @param value        The loaded FIREmodeOn
 * @return 0 for 0 or 1; -1 (with an error naming the parameter) otherwise
 */
static inline int mimic_sage_validate_fire_mode(const char *module_name, const int value) {
  if (value != MIMIC_SAGE_FIRE_MODE_OFF && value != MIMIC_SAGE_FIRE_MODE_ON) {
    ERROR_LOG("%s: FIREmodeOn = %d is not supported: the package supports only 0 (fixed-epsilon "
              "feedback) and 1 (FIRE scaling with energy-based ejection)",
              module_name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief The redshift of a galaxy's stored snapshot, ZZ[SnapNum], or a refusal
 *
 * SAGE26 reads ZZ[galaxies[p].SnapNum] before testing the velocity or the redshift, with no bounds
 * check; an out-of-range SnapNum is an out-of-bounds read there, not a physics rule. A stored
 * SnapNum outside the run's redshift table (0 to Snaplistlen - 1, the entries the core filled from
 * the snapshot list) is refused with an error naming the SnapNum and the table size; no redshift
 * is invented for it.
 *
 * @param params   The run configuration (its ZZ table and Snaplistlen)
 * @param snapnum  The galaxy's stored SnapNum
 * @param z_out    The redshift (unchanged on a refusal)
 * @return 0 on success; -1 on a refusal
 */
static inline int mimic_sage_fire_stored_redshift(const struct MimicConfig *params,
                                                  const int snapnum, double *z_out) {
  if (params == NULL || z_out == NULL) {
    ERROR_LOG("Stored-snapshot redshift: no run configuration");
    return -1;
  }
  if (snapnum < 0 || snapnum >= params->Snaplistlen) {
    ERROR_LOG("Stored-snapshot redshift: the galaxy's stored SnapNum = %d is outside the "
              "redshift table ZZ of %d entries (snapshots 0 to %d); SAGE26 reads ZZ[SnapNum] "
              "without a bounds check (FIRE feedback, "
              "cold streams), so no redshift is defined for it",
              snapnum, params->Snaplistlen, params->Snaplistlen - 1);
    return -1;
  }
  *z_out = params->ZZ[snapnum];
  return 0;
}

/**
 * @brief The locals of one FIRE feedback budget, for tests that inspect intermediate values
 *
 * Each helper fills its own part. A double is NAN, and a flag -1, where its helper did not run or
 * its path does not form it.
 */
struct MimicSageFireLocals {
  double redshift;              /**< ZZ[SnapNum], the stored snapshot's redshift */
  int scaling_valid;            /**< vc > 0.0 && z >= 0.0 */
  int vc_floored;               /**< vc < 1.0: the scaling's velocity floored at 1 km/s */
  int high_velocity;            /**< vc_floored >= 60: the slope -1.0 */
  double v_term;                /**< the velocity term */
  double scaling;               /**< the FIRE scaling, 0.0 where not valid */
  double eta_raw;               /**< the argument FeedbackReheatingEpsilon * scaling */
  int eta_cap_vvir_nonpositive; /**< vvir <= 0.0: eta_raw returned unchanged */
  double eta_max;               /**< 2.0 * EtaSNcode * EnergySNcode / (vvir * vvir) */
  int eta_cap_binds;            /**< eta_raw > eta_max */
  double eta;                   /**< the capped loading, MassLoading's double */
  double eps_eff;               /**< FeedbackEjectionEfficiency * scaling */
  int coupling_capped;          /**< eps_eff > MAX_SN_ENERGY_COUPLING */
  double coupling;              /**< the capped coupling */
  double e_fb;                  /**< the coupled supernova energy */
  double e_lift;                /**< the energy lifting the reheated gas */
  int energy_ejects;            /**< E_FB > E_lift */
};

/** @brief Mark every local of a FIRE budget as not formed (NAN, flags -1) */
static inline void mimic_sage_fire_locals_init(struct MimicSageFireLocals *l) {
  l->redshift = NAN;
  l->scaling_valid = -1;
  l->vc_floored = -1;
  l->high_velocity = -1;
  l->v_term = NAN;
  l->scaling = NAN;
  l->eta_raw = NAN;
  l->eta_cap_vvir_nonpositive = -1;
  l->eta_max = NAN;
  l->eta_cap_binds = -1;
  l->eta = NAN;
  l->eps_eff = NAN;
  l->coupling_capped = -1;
  l->coupling = NAN;
  l->e_fb = NAN;
  l->e_lift = NAN;
  l->energy_ejects = -1;
}

/**
 * @brief SAGE26's FIRE velocity and redshift scaling
 *
 * fire_scaling = 0.0 unless vc > 0.0 and z >= 0.0; then the velocity floored at 1 km/s, the slope
 * -3.2 below 60 km/s and -1.0 at and above, and pow(1.0 + z, 1.25) * v_term. The velocity is
 * SAGE26's float Vvir widened to double: the caller passes Mimic's halo Vvir narrowed to float.
 *
 * @param vvir      The host galaxy's own Vvir, as SAGE26's float
 * @param redshift  Its stored snapshot's redshift (mimic_sage_fire_stored_redshift)
 * @param l         Optional locals (NULL to skip)
 * @return The scaling (double)
 */
static inline double mimic_sage_fire_scaling(const float vvir, const double redshift,
                                             struct MimicSageFireLocals *l) {
  double fire_scaling = 0.0;
  const double z_fire = redshift;
  const double vc_fire = vvir;
  const int valid = vc_fire > 0.0 && z_fire >= 0.0;
  if (l != NULL) {
    l->redshift = z_fire;
    l->scaling_valid = valid;
  }
  if (valid) {
    const double vc_floored = (vc_fire < MIMIC_SAGE_FIRE_VELOCITY_FLOOR_KMS)
                                  ? MIMIC_SAGE_FIRE_VELOCITY_FLOOR_KMS
                                  : vc_fire;
    /* SAGE parity: SAGE26's pow(x, -1.0) for the high-velocity term runs as the division
     * 1.0 / x in its build, so the division is written here; the low-velocity term stays pow */
    const double v_term =
        (vc_floored < MIMIC_SAGE_FIRE_V_CRIT_KMS)
            ? pow(vc_floored / MIMIC_SAGE_FIRE_V_CRIT_KMS, MIMIC_SAGE_FIRE_LOW_V_EXPONENT)
            : 1.0 / (vc_floored / MIMIC_SAGE_FIRE_V_CRIT_KMS);
    fire_scaling = pow(1.0 + z_fire, MIMIC_SAGE_FIRE_REDSHIFT_EXPONENT) * v_term;
    if (l != NULL) {
      l->vc_floored = vc_fire < MIMIC_SAGE_FIRE_VELOCITY_FLOOR_KMS;
      l->high_velocity = !(vc_floored < MIMIC_SAGE_FIRE_V_CRIT_KMS);
      l->v_term = v_term;
    }
  }
  if (l != NULL) {
    l->scaling = fire_scaling;
  }
  return fire_scaling;
}

/**
 * @brief SAGE26's capped_eta_reheat
 *
 * Returns eta_reheat unchanged for vvir <= 0.0; otherwise caps it at
 * eta_max = MAX_SN_ENERGY_COUPLING * esn_per_mass / (vvir * vvir), esn_per_mass =
 * EtaSNcode * EnergySNcode, all in double, with vvir the unfloored velocity. A strict comparison:
 * eta_reheat equal to eta_max is returned as eta_reheat (the same value).
 *
 * @param eta_reheat      FeedbackReheatingEpsilon * fire_scaling, as SAGE26 forms the argument
 * @param vvir            The host's float Vvir widened to double
 * @param eta_sn_code     SAGE26's EtaSNcode
 * @param energy_sn_code  SAGE26's EnergySNcode
 * @param l               Optional locals (NULL to skip)
 * @return The capped loading
 */
static inline double mimic_sage_capped_eta_reheat(const double eta_reheat, const double vvir,
                                                  const double eta_sn_code,
                                                  const double energy_sn_code,
                                                  struct MimicSageFireLocals *l) {
  if (l != NULL) {
    l->eta_raw = eta_reheat;
    l->eta_cap_vvir_nonpositive = vvir <= 0.0;
  }
  if (vvir <= 0.0) {
    if (l != NULL) {
      l->eta = eta_reheat;
    }
    return eta_reheat;
  }
  const double esn_per_mass = eta_sn_code * energy_sn_code;
  const double eta_max = MIMIC_SAGE_MAX_SN_ENERGY_COUPLING * esn_per_mass / (vvir * vvir);
  const double eta = (eta_reheat > eta_max) ? eta_max : eta_reheat;
  if (l != NULL) {
    l->eta_max = eta_max;
    l->eta_cap_binds = eta_reheat > eta_max;
    l->eta = eta;
  }
  return eta;
}

/**
 * @brief SAGE26's sn_energy_coupling
 *
 * eps_eff = FeedbackEjectionEfficiency * fire_scaling, capped (strictly) at
 * MAX_SN_ENERGY_COUPLING.
 *
 * @param fire_scaling                  The FIRE scaling
 * @param feedback_ejection_efficiency  FeedbackEjectionEfficiency
 * @param l                             Optional locals (NULL to skip)
 * @return The capped coupling
 */
static inline double mimic_sage_sn_energy_coupling(const double fire_scaling,
                                                   const double feedback_ejection_efficiency,
                                                   struct MimicSageFireLocals *l) {
  const double eps_eff = feedback_ejection_efficiency * fire_scaling;
  const double coupling =
      (eps_eff > MIMIC_SAGE_MAX_SN_ENERGY_COUPLING) ? MIMIC_SAGE_MAX_SN_ENERGY_COUPLING : eps_eff;
  if (l != NULL) {
    l->eps_eff = eps_eff;
    l->coupling_capped = eps_eff > MIMIC_SAGE_MAX_SN_ENERGY_COUPLING;
    l->coupling = coupling;
  }
  return coupling;
}

/**
 * @brief SAGE26's FIRE energy-based ejection (Hirschmann et al. 2016), for a positive host
 *        velocity
 *
 * E_FB = coupling * 0.5 * stars * (EtaSNcode * EnergySNcode) and E_lift = 0.5 * reheated * vc * vc,
 * from the renormalised stars and reheated mass; the ejected mass is
 * (E_FB - E_lift) / (0.5 * vc * vc) when E_FB > E_lift, else 0.0. The caller tests vc > 0.0
 * first and applies the negative-ejection floor after.
 *
 * @param coupling        mimic_sage_sn_energy_coupling() of the scaling
 * @param stars           The renormalised stars
 * @param reheated_mass   The renormalised reheated mass
 * @param vc              The host's float Vvir widened to double (positive)
 * @param eta_sn_code     SAGE26's EtaSNcode
 * @param energy_sn_code  SAGE26's EnergySNcode
 * @param l               Optional locals (NULL to skip)
 * @return The ejected mass
 */
static inline double mimic_sage_fire_energy_ejection(const double coupling, const double stars,
                                                     const double reheated_mass, const double vc,
                                                     const double eta_sn_code,
                                                     const double energy_sn_code,
                                                     struct MimicSageFireLocals *l) {
  const double E_FB = coupling * 0.5 * stars * (eta_sn_code * energy_sn_code);
  const double E_lift = 0.5 * reheated_mass * vc * vc;
  const double ejected_mass = (E_FB > E_lift) ? (E_FB - E_lift) / (0.5 * vc * vc) : 0.0;
  if (l != NULL) {
    l->e_fb = E_FB;
    l->e_lift = E_lift;
    l->energy_ejects = E_FB > E_lift;
  }
  return ejected_mass;
}

#endif /* MIMIC_SHARED_SAGE_FIRE_FEEDBACK_PHYSICS_H */
