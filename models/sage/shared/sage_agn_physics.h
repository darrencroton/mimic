#ifndef MIMIC_SHARED_SAGE_AGN_PHYSICS_H
#define MIMIC_SHARED_SAGE_AGN_PHYSICS_H

/*
 * AGN physics kernels: black hole growth and quasar-mode wind.
 * Used by sage_starburst_feedback and sage_quasar_mode.
 *
 * Split from merger_physics.h; BH/quasar half only.
 * For collisional starburst physics see sage_starburst_physics.h.
 */

#include <math.h>

#include "constants.h"
#include "module_interface.h"
#include "types.h"
#include "shared/metallicity.h"
#include "module_system/physical_constants.h"

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

  /* SAGE parity: c and UnitVelocity must share units. SAGE uses c in cm/s
   * (macros.h: C = 2.9979e10) divided by UnitVelocity_in_cm_per_s. Using c in
   * km/s (C_KM_S) here makes the quasar wind energy ~1e10x too small, so the
   * wind would essentially never eject gas. */
  c_over_unit_vel = C_CGS / ctx->params->UnitVelocity_in_cm_per_s;
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

#endif /* MIMIC_SHARED_SAGE_AGN_PHYSICS_H */
