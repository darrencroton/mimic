/**
 * @file sage_sf_feedback_physics.h
 * @brief SAGE26's star-formation and supernova-feedback commits, shared by disk SF and bursts
 *
 * Implements SAGE26's update_from_star_formation and update_from_feedback, through which SAGE26
 * commits every star-formation and supernova-feedback event, plus the supernova code constants
 * (EnergySNcode, EtaSNcode) and the FeedbackReheatingEpsilon validator. Callers:
 * sage26_apply_star_formation_supernova (disk star formation, starformation_and_feedback),
 * sage26_ffb_star_formation (starformation_ffb) and mimic_apply_collisional_starburst
 * (shared/sage_starburst_physics.h, collisional_starburst_recipe) for both burst channels.
 *
 * Supported: SupernovaRecipeOn 1 with CGMrecipeOn 0 or 1 and SFprescription 0 or 1. With
 * CGMrecipeOn 1 update_from_feedback routes the reheated gas and the ejection through the FoF
 * central's CGM pair (CGMgas, MetalsCGMgas) when the central is in the CGM regime (Regime 0), and
 * through its hot pair otherwise; with CGMrecipeOn 0 always through its hot pair. Under
 * SFprescription 1, which tracks H2, each routine then caps H2gas and H1gas, each on its own, at
 * the float hydrogen of the cold gas it leaves (never their sum). Under SFprescription 0 neither
 * routine writes H1gas or H2gas.
 *
 * Every reservoir is a float and every mass and metallicity a double, so each `+=`/`-=` widens the
 * reservoir, operates in double and narrows the result.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_SF_FEEDBACK_PHYSICS_H
#define MIMIC_SHARED_SAGE_SF_FEEDBACK_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "error.h"
#include "types.h"
#include "shared/metallicity.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_constants.h"
#include "shared/sage_h2_physics.h"
#include "module_system/physical_constants.h"

/**
 * @brief SAGE26's supernova energy in code units, EnergySNcode
 *
 * SAGE26 forms EnergySN / UnitEnergy_in_cgs * Hubble_h left to right, with EnergySN
 * 1.0e51 erg, the package's ENERGY_SN.
 */
static inline double mimic_sage_energy_sn_code(const double unit_energy_in_cgs,
                                               const double hubble_h) {
  return ENERGY_SN / unit_energy_in_cgs * hubble_h;
}

/**
 * @brief SAGE26's supernova efficiency in code units, EtaSNcode
 *
 * SAGE26 forms EtaSN * (UnitMass_in_g / SOLAR_MASS) / Hubble_h, with EtaSN 5.0e-3,
 * the package's ETA_SN, and SOLAR_MASS 1.989e33 g.
 */
static inline double mimic_sage_eta_sn_code(const double unit_mass_in_g, const double hubble_h) {
  return ETA_SN * (unit_mass_in_g / SOLAR_MASS) / hubble_h;
}

/**
 * @brief Refuse a FeedbackReheatingEpsilon outside the package's one accepted range
 *
 * FeedbackReheatingEpsilon is SAGE26's reheated mass per unit of stars formed, and the
 * coefficient of the FIRE loading. SAGE26 registers it with no range check. Every reader in the
 * package (the supernova, burst and feedback-free-burst modules) accepts the same range: any
 * finite value >= 0, so that no physically meaningful value SAGE26 runs with is refused; a
 * negative, NaN or infinite loading is refused, naming the parameter.
 *
 * @param module_name  The calling module, for the error message
 * @param value        The loaded FeedbackReheatingEpsilon
 * @return 0 for a finite value >= 0; -1 (with an error naming the parameter) otherwise
 */
static inline int mimic_sage_validate_feedback_reheating_epsilon(const char *module_name,
                                                                 const double value) {
  if (!isfinite(value) || !(value >= 0.0)) {
    ERROR_LOG("%s: FeedbackReheatingEpsilon = %g is not valid: the reheating loading must be "
              "finite and >= 0",
              module_name, value);
    return -1;
  }
  return 0;
}

/**
 * @brief The locals of one hydrogen cap, for tests that inspect the intermediate values
 *
 * One record per cap site: update_from_star_formation's, update_from_feedback's and
 * grow_black_hole's (sage_agn_physics.h). max_h is NAN, and the two flags -1, where the guard is
 * false.
 */
struct MimicSageHydrogenCapLocals {
  int tracks_h2;    /**< the site's guard as SAGE26 writes it: sf_prescription_tracks_h2 at the
                         commits, SFprescription != 0 && != 2 at the black-hole accretion */
  float max_h;      /**< (ColdGas > 0.0f) ? ColdGas * 0.74 : 0.0f, narrowed to float */
  int h2_cap_binds; /**< 1 if H2gas > max_h: H2gas set to max_h */
  int h1_cap_binds; /**< 1 if H1gas > max_h: H1gas set to max_h */
};

/**
 * @brief Commit one star-formation event: SAGE26's update_from_star_formation
 *
 * Locks (1 - RecycleFraction) * stars of cold gas into stars, with the cold gas's
 * metallicity. The recycled fraction stays in the cold gas. Then, for a prescription
 * that tracks H2, H2gas and H1gas are each capped at the hydrogen of the remaining
 * cold gas.
 *
 * @param gal               Galaxy forming the stars
 * @param stars             Stellar mass formed, 1e10 Msun/h (double, as SAGE26's argument)
 * @param metallicity       Cold-gas metallicity at the call (get_metallicity of the cold gas)
 * @param recycle_fraction  RecycleFraction
 * @param sf_prescription   SFprescription (0 or 1), which selects the hydrogen cap
 * @param caps              Optional, may be NULL: receives the cap's locals
 */
static inline void mimic_sage_update_from_star_formation(struct GalaxyData *gal, const double stars,
                                                         const double metallicity,
                                                         const double recycle_fraction,
                                                         const int sf_prescription,
                                                         struct MimicSageHydrogenCapLocals *caps) {
  // SAGE parity: each of the four updates is one multiply-add, as SAGE26's build computes them
  const double locked_fraction = 1.0 - recycle_fraction;
  const double metals_fraction = metallicity * locked_fraction;

  gal->ColdGas = (float)fma(-locked_fraction, stars, (double)gal->ColdGas);
  gal->StellarMass = (float)fma(locked_fraction, stars, (double)gal->StellarMass);
  gal->MetalsColdGas = (float)fma(-metals_fraction, stars, (double)gal->MetalsColdGas);
  gal->MetalsStellarMass = (float)fma(metals_fraction, stars, (double)gal->MetalsStellarMass);

  /* SAGE parity: H2gas and H1gas predate the depletion, so each is capped on its own at the float
   * hydrogen of the cold gas now left; the sum is not capped */
  struct MimicSageHydrogenCapLocals cap = {0, NAN, -1, -1};
  cap.tracks_h2 = mimic_sage_sf_prescription_tracks_h2(sf_prescription);
  if (cap.tracks_h2) {
    const float max_h =
        (float)((gal->ColdGas > 0.0f) ? gal->ColdGas * SAGE_HYDROGEN_MASS_FRAC : 0.0f);
    cap.max_h = max_h;
    cap.h2_cap_binds = gal->H2gas > max_h;
    if (cap.h2_cap_binds) {
      gal->H2gas = max_h;
    }
    cap.h1_cap_binds = gal->H1gas > max_h;
    if (cap.h1_cap_binds) {
      gal->H1gas = max_h;
    }
  }
  if (caps != NULL) {
    *caps = cap;
  }
}

/**
 * @brief The locals of one update_from_feedback call, for tests that inspect intermediate values
 *
 * The selected pair is the FoF central's CGM pair (CGMgas, MetalsCGMgas) with CGMrecipeOn 1 and
 * the central's Regime 0, its hot pair (HotGas, MetalsHotGas) otherwise.
 * `selected_gas_at_cap` and `selected_metals_at_cap` are that pair after the reheated gas is added
 * and before the ejection debit: the reservoirs the two ejection caps compare against.
 */
struct MimicSageFeedbackLocals {
  double reheated_mass_clamped;  /**< reheated mass after the clamp to ColdGas */
  double ejected_mass_capped;    /**< ejected mass after the cap to the selected gas */
  double metallicity_selected;   /**< the selected pair's metallicity after the reheating
                                      (SAGE26's metallicityCGM or metallicityHot) */
  double metals_to_eject;        /**< metallicity_selected * ejected mass, before its cap */
  double metals_to_eject_capped; /**< after the cap to the selected metals */
  float selected_gas_at_cap;     /**< the selected gas the gas cap compares against */
  float selected_metals_at_cap;  /**< the selected metals the metal cap compares against */
  int reservoir_cgm;             /**< 1 if the CGM pair was selected (CGMrecipeOn 1, Regime 0) */
  int reheat_clamp_binds;        /**< 1 if reheated mass > ColdGas */
  int ejected_cap_binds;         /**< 1 if ejected mass > the selected gas */
  int metals_cap_binds;          /**< 1 if the metal transfer > the selected metals */
  struct MimicSageHydrogenCapLocals hydrogen_caps; /**< the post-reheating cap */
};

/**
 * @brief Commit one supernova-feedback event: SAGE26's update_from_feedback
 *
 * Reheats cold gas of `gal` into the FoF central's selected pair, ejects gas of that pair into the
 * central's ejected reservoir, and accumulates the reheated mass into `gal`'s
 * SupernovaOutflowRate. The selected pair is the central's CGM pair when CGMrecipeOn is 1 and the
 * CENTRAL's Regime is 0, and its hot pair otherwise; the forming galaxy's own Regime is never
 * read. In order:
 *
 * - the reheated mass is clamped to the forming galaxy's ColdGas;
 * - the forming galaxy's cold gas and metals are debited, and a prescription that tracks H2 caps
 *   its H2gas and H1gas, each on its own, at the hydrogen of its remaining cold gas, before the
 *   reheated gas reaches the central;
 * - the selected pair is credited with the reheated gas and metals;
 * - the ejected mass is capped to the selected gas AFTER the reheating, the selected pair's new
 *   double metallicity formed, and the metal transfer metallicity * ejected mass capped
 *   separately to the selected metals, so metals above gas cannot drive them negative;
 * - the selected pair is debited and the central's ejected pair credited; the unselected pair is
 *   not touched;
 * - OutflowRate += reheated mass is a float accumulation of the undivided mass;
 *   sage26_finalize_cooling_heating multiplies it by 1/deltaT once per halo, as SAGE26 does
 *   after its last substep.
 *
 * SAGE26's assertions on the reheated mass abort the run; they guard states SAGE26 does not
 * produce and are not mirrored.
 *
 * @param gal            Galaxy whose cold gas is reheated (SAGE26's p)
 * @param central_gal    The FoF central receiving the reheated and ejected gas
 *                       (SAGE26's centralgal), whose Regime selects the pair; may be gal itself
 * @param reheated_mass  Reheated mass, 1e10 Msun/h (double)
 * @param ejected_mass   Ejected mass, 1e10 Msun/h (double)
 * @param metallicity    Cold-gas metallicity of gal at the call (get_metallicity)
 * @param sf_prescription SFprescription (0 or 1), which selects the hydrogen cap
 * @param cgm_recipe_on  CGMrecipeOn (0 or 1), which with the central's Regime selects the pair
 * @param locals         Optional, may be NULL: receives the call's locals
 */
static inline void
mimic_sage_update_from_feedback(struct GalaxyData *gal, struct GalaxyData *central_gal,
                                double reheated_mass, double ejected_mass, const double metallicity,
                                const int sf_prescription, const int cgm_recipe_on,
                                struct MimicSageFeedbackLocals *locals) {
  // SAGE parity: the clamp to the remaining cold gas, for floating-point overshoot
  const int reheat_clamp_binds = reheated_mass > gal->ColdGas;
  if (reheat_clamp_binds) {
    reheated_mass = gal->ColdGas;
  }

  // Remove reheated mass from cold gas. SAGE parity: the metal debit is one multiply-subtract,
  // as SAGE26's build computes it.
  gal->ColdGas -= reheated_mass;
  gal->MetalsColdGas = (float)fma(-metallicity, reheated_mass, (double)gal->MetalsColdGas);

  /* SAGE parity: before the central's credit, H2gas and H1gas are each capped on their own at the
   * float hydrogen of the cold gas now left; the sum is not capped */
  struct MimicSageHydrogenCapLocals hydrogen_caps = {0, NAN, -1, -1};
  hydrogen_caps.tracks_h2 = mimic_sage_sf_prescription_tracks_h2(sf_prescription);
  if (hydrogen_caps.tracks_h2) {
    const float max_h_fb =
        (float)((gal->ColdGas > 0.0f) ? gal->ColdGas * SAGE_HYDROGEN_MASS_FRAC : 0.0f);
    hydrogen_caps.max_h = max_h_fb;
    hydrogen_caps.h2_cap_binds = gal->H2gas > max_h_fb;
    if (hydrogen_caps.h2_cap_binds) {
      gal->H2gas = max_h_fb;
    }
    hydrogen_caps.h1_cap_binds = gal->H1gas > max_h_fb;
    if (hydrogen_caps.h1_cap_binds) {
      gal->H1gas = max_h_fb;
    }
  }

  /* The FoF CENTRAL's regime selects the pair, CGM for CGMrecipeOn 1 and Regime 0, hot
   * otherwise; SAGE26's three branches are the same statements, written once on that pair */
  const int reservoir_cgm = mimic_sage_cgm_reservoir_selected(cgm_recipe_on, central_gal->Regime);
  float *const selected_gas = reservoir_cgm ? &central_gal->CGMgas : &central_gal->HotGas;
  float *const selected_metals =
      reservoir_cgm ? &central_gal->MetalsCGMgas : &central_gal->MetalsHotGas;

  // Add reheated gas to the selected pair. SAGE parity: the metal credit is one multiply-add,
  // as SAGE26's build computes it.
  *selected_gas += reheated_mass;
  *selected_metals = (float)fma(metallicity, reheated_mass, (double)*selected_metals);

  // SAGE parity: the ejection is capped to the selected gas AFTER the reheating
  const float selected_gas_at_cap = *selected_gas;
  const int ejected_cap_binds = ejected_mass > *selected_gas;
  if (ejected_cap_binds) {
    ejected_mass = *selected_gas;
  }
  const double metallicity_selected = mimic_get_metallicity(*selected_gas, *selected_metals);

  // SAGE parity: the metal transfer is capped on its own to the selected metals
  const double metals_to_eject = metallicity_selected * ejected_mass;
  double metals_to_eject_capped = metals_to_eject;
  const float selected_metals_at_cap = *selected_metals;
  const int metals_cap_binds = metals_to_eject_capped > *selected_metals;
  if (metals_cap_binds) {
    metals_to_eject_capped = *selected_metals;
  }

  // Eject from the selected pair to the central's EjectedMass
  *selected_gas -= ejected_mass;
  *selected_metals -= metals_to_eject_capped;
  central_gal->EjectedGas += ejected_mass;
  central_gal->MetalsEjectedGas += metals_to_eject_capped;

  /* SAGE parity: OutflowRate accumulates the undivided reheated mass in float; it becomes a
   * rate only at the end-of-halo normalisation */
  gal->SupernovaOutflowRate += reheated_mass;

  if (locals != NULL) {
    locals->reheated_mass_clamped = reheated_mass;
    locals->ejected_mass_capped = ejected_mass;
    locals->metallicity_selected = metallicity_selected;
    locals->metals_to_eject = metals_to_eject;
    locals->metals_to_eject_capped = metals_to_eject_capped;
    locals->selected_gas_at_cap = selected_gas_at_cap;
    locals->selected_metals_at_cap = selected_metals_at_cap;
    locals->reservoir_cgm = reservoir_cgm;
    locals->reheat_clamp_binds = reheat_clamp_binds;
    locals->ejected_cap_binds = ejected_cap_binds;
    locals->metals_cap_binds = metals_cap_binds;
    locals->hydrogen_caps = hydrogen_caps;
  }
}

#endif /* MIMIC_SHARED_SAGE_SF_FEEDBACK_PHYSICS_H */
