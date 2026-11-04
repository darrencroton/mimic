/**
 * @file sage_starburst_physics.h
 * @brief Collisional starburst kernel with SN feedback and metal enrichment
 *
 * Provides the starburst recipe shared by sage26_starburst_feedback for both the
 * disk-instability channel and the merger channel. For BH growth and quasar-mode
 * wind see sage_agn_physics.h.
 *
 * The burst commits its star formation and its supernova feedback through the
 * package's one implementation of SAGE26's update_from_star_formation and
 * update_from_feedback (sage_sf_feedback_physics.h), the functions SAGE26's own
 * collisional_starburst_recipe calls (R/src/model_mergers.c:759-760, :788-791), so
 * a burst commits each event exactly as disk star formation does. Before that
 * commit the burst samples the bursting galaxy's bulge star-formation history as
 * SAGE26 does (R/src/model_mergers.c:755-757), through shared/sage_sfr_history.h.
 * The burst's own arithmetic (its mass, its feedback inputs, its bulge growth and
 * its yield) is the merger node's to port and is unchanged here.
 *
 * @note Split from merger_physics.h; starburst/SN half only. Although
 *       sage26_starburst_feedback is currently the sole consumer, this header is
 *       the architectural counterpart to sage_agn_physics.h — both halves of the
 *       split belong here to keep the boundary symmetric.
 */

#ifndef MIMIC_SHARED_SAGE_STARBURST_PHYSICS_H
#define MIMIC_SHARED_SAGE_STARBURST_PHYSICS_H

#include <math.h>

#include "constants.h"
#include "types.h"
#include "shared/metallicity.h"
#include "shared/sage_constants.h"
#include "shared/sage_sf_feedback_physics.h"
#include "shared/sage_sfr_history.h"

/**
 * @brief Parameter bundle for merger/disk-instability starburst physics
 */
struct MimicStarburstParams {
  double feedback_reheating_epsilon;
  double feedback_ejection_efficiency;
  double recycle_fraction;
  double yield;
  double frac_z_leave_disk;
  double threshold_major_merger;
  double energy_sn_code;
  double eta_sn_code;
};

/**
 * @brief Apply collisional-starburst recipe for one event
 *
 * @param efficiency_factor  Trigger efficiency (merger mass ratio or unstable disk fraction)
 * @param gal                Galaxy receiving the starburst (may differ from central)
 * @param central_gal        FoF central galaxy receiving reheated/ejected gas
 * @param central_halo       FoF central halo (Vvir used for energy budget)
 * @param mode               1 = disk instability (efficiency direct); 0 = merger (Somerville
 * scaling)
 * @param rate_dt            Interval for the StarFormationRate diagnostic (0 skips it); the
 *                           outflow is accumulated undivided by the feedback commit
 * @param sfr_bin            History bin of the substep the burst happens in
 *                           (mimic_sage_sfr_step_bin), SAGE26's step
 * @param sfr_dt             The substep interval, SAGE26's dt = deltaT / effective_steps,
 *                           the denominator of the bulge history rate
 * @param p                  Model parameter bundle (see struct MimicStarburstParams)
 */
static inline void mimic_apply_collisional_starburst(double efficiency_factor,
                                                     struct GalaxyData *gal,
                                                     struct GalaxyData *central_gal,
                                                     const struct Halo *central_halo, int mode,
                                                     double rate_dt, int sfr_bin, double sfr_dt,
                                                     const struct MimicStarburstParams *p) {
  double eburst;
  double stars;
  double reheated_mass;
  double ejected_mass;
  double vvir_central;
  double metallicity;
  double metallicity_cold;

  if (gal == NULL || central_gal == NULL || central_halo == NULL || p == NULL ||
      efficiency_factor <= 0.0) {
    return;
  }

  if (mode == 1) {
    eburst = efficiency_factor;
  } else {
    eburst = 0.56 * pow(efficiency_factor, 0.7);
  }

  stars = eburst * gal->ColdGas;
  if (stars < 0.0) {
    stars = 0.0;
  }

  reheated_mass = p->feedback_reheating_epsilon * stars;
  if ((stars + reheated_mass) > gal->ColdGas && (stars + reheated_mass) > 0.0) {
    const double fac = gal->ColdGas / (stars + reheated_mass);
    stars *= fac;
    reheated_mass *= fac;
  }

  vvir_central = central_halo->Vvir;
  if (vvir_central > 0.0) {
    ejected_mass = (p->feedback_ejection_efficiency * (p->eta_sn_code * p->energy_sn_code) /
                        (vvir_central * vvir_central) -
                    p->feedback_reheating_epsilon) *
                   stars;
  } else {
    ejected_mass = 0.0;
  }

  if (ejected_mass < 0.0) {
    ejected_mass = 0.0;
  }

  /* SAGE parity: the bulge history is sampled for the bursting galaxy once the burst
   * mass is final and before update_from_star_formation, its cold gas and metals
   * accumulated rather than assigned (R/src/model_mergers.c:754-757). The callers
   * validate the bin and the substep interval; an invalid one samples nothing. */
  if (sfr_dt > 0.0) {
    (void)mimic_sage_sample_bulge_sfr(gal, sfr_bin, stars, sfr_dt);
  }

  metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  mimic_sage_update_from_star_formation(gal, stars, metallicity, p->recycle_fraction);
  gal->BulgeMass += (1.0 - p->recycle_fraction) * stars;
  gal->MetalsBulgeMass += metallicity * (1.0 - p->recycle_fraction) * stars;

  if (rate_dt > 0.0) {
    gal->StarFormationRate += stars / rate_dt;
  }

  metallicity_cold = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  mimic_sage_update_from_feedback(gal, central_gal, reheated_mass, ejected_mass, metallicity_cold,
                                  NULL);

  if (gal->ColdGas > SAGE_COLD_GAS_YIELD_THRESHOLD &&
      efficiency_factor < p->threshold_major_merger) {
    const double frac_z_leave_disk_val =
        p->frac_z_leave_disk * exp(-1.0 * central_halo->Mvir / SAGE_METAL_EJECTION_MVIR_SCALE);
    gal->MetalsColdGas += p->yield * (1.0 - frac_z_leave_disk_val) * stars;
    central_gal->MetalsHotGas += p->yield * frac_z_leave_disk_val * stars;
  } else {
    central_gal->MetalsHotGas += p->yield * stars;
  }
}

#endif /* MIMIC_SHARED_SAGE_STARBURST_PHYSICS_H */
