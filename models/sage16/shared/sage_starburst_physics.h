/**
 * @file sage_starburst_physics.h
 * @brief Collisional starburst kernel with SN feedback and metal enrichment
 *
 * Provides the starburst recipe shared by sage_starburst_feedback for both the
 * disk-instability channel and the merger channel. For BH growth and quasar-mode
 * wind see sage_agn_physics.h.
 *
 * @note Split from merger_physics.h; starburst/SN half only. Although
 *       sage_starburst_feedback is currently the sole consumer, this header is
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
 * @param rate_dt            Timestep length for rate accumulation (Gyr/h; 0 skips rate update)
 * @param p                  Model parameter bundle (see struct MimicStarburstParams)
 */
static inline void
mimic_apply_collisional_starburst(double efficiency_factor, struct GalaxyData *gal,
                                  struct GalaxyData *central_gal, const struct Halo *central_halo,
                                  int mode, double rate_dt, const struct MimicStarburstParams *p) {
  double eburst;
  double stars;
  double reheated_mass;
  double ejected_mass;
  double vvir_central;
  double metallicity;
  double metallicity_cold;
  double metallicity_hot;

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

  metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  gal->ColdGas -= (1.0 - p->recycle_fraction) * stars;
  gal->MetalsColdGas -= metallicity * (1.0 - p->recycle_fraction) * stars;
  gal->StellarMass += (1.0 - p->recycle_fraction) * stars;
  gal->MetalsStellarMass += metallicity * (1.0 - p->recycle_fraction) * stars;
  gal->BulgeMass += (1.0 - p->recycle_fraction) * stars;
  gal->MetalsBulgeMass += metallicity * (1.0 - p->recycle_fraction) * stars;

  if (rate_dt > 0.0) {
    gal->StarFormationRate += stars / rate_dt;
  }

  metallicity_cold = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  gal->ColdGas -= reheated_mass;
  gal->MetalsColdGas -= metallicity_cold * reheated_mass;

  central_gal->HotGas += reheated_mass;
  central_gal->MetalsHotGas += metallicity_cold * reheated_mass;

  if (ejected_mass > central_gal->HotGas) {
    ejected_mass = central_gal->HotGas;
  }

  metallicity_hot = mimic_get_metallicity(central_gal->HotGas, central_gal->MetalsHotGas);
  central_gal->HotGas -= ejected_mass;
  central_gal->MetalsHotGas -= metallicity_hot * ejected_mass;
  central_gal->EjectedGas += ejected_mass;
  central_gal->MetalsEjectedGas += metallicity_hot * ejected_mass;

  if (rate_dt > 0.0) {
    gal->SupernovaOutflowRate += reheated_mass / rate_dt;
  }

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
