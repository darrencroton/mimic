#ifndef MIMIC_SHARED_MERGER_PHYSICS_H
#define MIMIC_SHARED_MERGER_PHYSICS_H

#include <math.h>

#include "constants.h"
#include "module_interface.h"
#include "types.h"
#include "_shared/metallicity.h"
#include "_system/physical_constants.h"

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
 * @brief Grow BH mass from a trigger efficiency (merger ratio or disk instability)
 *
 * @return BH accretion mass for this event (1e10 Msun/h)
 */
static inline double mimic_apply_black_hole_growth(
    struct Halo *halo, double efficiency_factor, double black_hole_growth_rate) {
  struct GalaxyData *gal;
  double bh_accrete;
  double accrete;
  float metallicity;

  if (halo == NULL || halo->galaxy == NULL || efficiency_factor <= 0.0 ||
      black_hole_growth_rate <= 0.0) {
    return 0.0;
  }

  gal = halo->galaxy;
  if (gal->ColdGas <= 0.0 || halo->Vvir <= 0.0) {
    return 0.0;
  }

  /* Kauffmann & Haehnelt (2000), with low-mass halo suppression at 280 km/s. */
  bh_accrete = black_hole_growth_rate * efficiency_factor /
               (1.0 + (280.0 / halo->Vvir) * (280.0 / halo->Vvir)) *
               gal->ColdGas;

  accrete = (bh_accrete > gal->ColdGas) ? gal->ColdGas : bh_accrete;
  metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);

  gal->BlackHoleMass += accrete;
  gal->ColdGas -= accrete;
  gal->MetalsColdGas -= metallicity * accrete;
  gal->QuasarModeBHaccretionMass += accrete;

  return accrete;
}

/**
 * @brief Apply quasar-mode wind from BH accretion event
 */
static inline void mimic_apply_quasar_mode_wind(
    struct Halo *halo, double bh_accrete, double quasar_mode_efficiency,
    const struct ModuleContext *ctx) {
  struct GalaxyData *gal;
  double c_over_unit_vel;
  double quasar_energy;
  double cold_gas_energy;
  double hot_gas_energy;

  if (halo == NULL || halo->galaxy == NULL || ctx == NULL || ctx->params == NULL ||
      bh_accrete <= 0.0 || quasar_mode_efficiency <= 0.0) {
    return;
  }

  gal = halo->galaxy;
  if (halo->Vvir <= 0.0 || ctx->params->UnitVelocity_in_cm_per_s <= 0.0) {
    return;
  }

  c_over_unit_vel = C_KM_S / ctx->params->UnitVelocity_in_cm_per_s;
  quasar_energy = quasar_mode_efficiency * 0.1 * bh_accrete * c_over_unit_vel *
                  c_over_unit_vel;

  cold_gas_energy = 0.5 * gal->ColdGas * halo->Vvir * halo->Vvir;
  hot_gas_energy = 0.5 * gal->HotGas * halo->Vvir * halo->Vvir;

  if (quasar_energy > cold_gas_energy) {
    gal->EjectedGas += gal->ColdGas;
    gal->MetalsEjectedGas += gal->MetalsColdGas;
    gal->ColdGas = 0.0;
    gal->MetalsColdGas = 0.0;
  }

  if (quasar_energy > cold_gas_energy + hot_gas_energy) {
    gal->EjectedGas += gal->HotGas;
    gal->MetalsEjectedGas += gal->MetalsHotGas;
    gal->HotGas = 0.0;
    gal->MetalsHotGas = 0.0;
  }
}

/**
 * @brief Apply collisional-starburst recipe for one event
 *
 * @param mode 1=disk instability (efficiency direct), 0=merger (Somerville scaling)
 */
static inline void mimic_apply_collisional_starburst(
    double efficiency_factor, struct GalaxyData *gal,
    struct GalaxyData *central_gal, const struct ModuleContext *ctx, int mode,
    const struct MimicStarburstParams *p) {
  const struct Halo *central_halo;
  double eburst;
  double stars;
  double reheated_mass;
  double ejected_mass;
  double vvir_central;
  double metallicity;
  double metallicity_cold;
  double metallicity_hot;

  if (gal == NULL || central_gal == NULL || ctx == NULL || p == NULL ||
      efficiency_factor <= 0.0) {
    return;
  }

  central_halo = ctx->central_galaxy;
  if (central_halo == NULL) {
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
    ejected_mass =
        (p->feedback_ejection_efficiency * (p->eta_sn_code * p->energy_sn_code) /
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

  if (central_halo->dT > 0.0) {
    gal->StarFormationRate += stars / central_halo->dT;
  }

  metallicity_cold = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  gal->ColdGas -= reheated_mass;
  gal->MetalsColdGas -= metallicity_cold * reheated_mass;

  central_gal->HotGas += reheated_mass;
  central_gal->MetalsHotGas += metallicity_cold * reheated_mass;

  if (ejected_mass > central_gal->HotGas) {
    ejected_mass = central_gal->HotGas;
  }

  metallicity_hot =
      mimic_get_metallicity(central_gal->HotGas, central_gal->MetalsHotGas);
  central_gal->HotGas -= ejected_mass;
  central_gal->MetalsHotGas -= metallicity_hot * ejected_mass;
  central_gal->EjectedGas += ejected_mass;
  central_gal->MetalsEjectedGas += metallicity_hot * ejected_mass;

  if (central_halo->dT > 0.0) {
    gal->SupernovaOutflowRate += reheated_mass / central_halo->dT;
  }

  if (gal->ColdGas > EPSILON_SMALL &&
      efficiency_factor < p->threshold_major_merger) {
    const double frac_z_leave_disk_val =
        p->frac_z_leave_disk * exp(-1.0 * central_halo->Mvir / 30.0);
    gal->MetalsColdGas += p->yield * (1.0 - frac_z_leave_disk_val) * stars;
    central_gal->MetalsHotGas += p->yield * frac_z_leave_disk_val * stars;
  } else {
    central_gal->MetalsHotGas += p->yield * stars;
  }
}

#endif /* MIMIC_SHARED_MERGER_PHYSICS_H */
