/**
 * @file sage_agn_physics.h
 * @brief SAGE26's black-hole growth and quasar-mode wind (grow_black_hole, quasar_mode_wind)
 *
 * Implements SAGE26's grow_black_hole and quasar_mode_wind for CGMrecipeOn 0 or 1 and
 * SFprescription 0 or 1. Three package callers use them, as SAGE26 calls grow_black_hole from
 * three places: sage26_quasar_mode for a merger and for a disk instability (through the
 * UnstableDiskGasFraction hand-off), and the collisional burst's post-minor-merger instability
 * recheck in shared/sage_starburst_physics.h.
 *
 * grow_black_hole, for a galaxy with positive ColdGas:
 *
 *     BHaccrete = BlackHoleGrowthRate * mass_ratio / (1 + (280 / Vvir)^2) * ColdGas
 *     BHaccrete = min(BHaccrete, ColdGas)
 *     BlackHoleMass += BHaccrete; ColdGas -= BHaccrete;
 *     MetalsColdGas -= Z_cold * BHaccrete, floored at 0
 *     if SFprescription != 0 && != 2: H2gas, H1gas each capped at max_h_bh
 *     quasar_mode_wind(BHaccrete)
 *     QuasarModeBHaccretionMass += BHaccrete
 *
 * quasar_mode_wind ejects the host's cold gas, and then its CGM or hot pair, when the wind
 * energy exceeds their binding energies (see mimic_apply_quasar_mode_wind). The host is the
 * galaxy that grew the black hole, never the FoF central as such.
 *
 * Every reservoir is a float and every SAGE26 local a double, so each `+=`/`-=` widens the
 * reservoir, operates in double and narrows the result. Vvir is read as SAGE26's float.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
 */

#ifndef MIMIC_SHARED_SAGE_AGN_PHYSICS_H
#define MIMIC_SHARED_SAGE_AGN_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "constants.h"
#include "module_interface.h"
#include "types.h"
#include "shared/metallicity.h"
#include "shared/sage_cgm_routing.h"
#include "shared/sage_constants.h"
#include "shared/sage_sf_feedback_physics.h"
#include "module_system/physical_constants.h"

/* SAGE26's BH_GROWTH_V_KMS, the halo velocity below which black-hole growth is
 * suppressed, km/s */
#define SAGE_BH_GROWTH_V_KMS 280.0

/**
 * @brief The locals of one quasar_mode_wind call, for tests that inspect the intermediate values
 *
 * A value the call does not form is left as NaN, a flag it does not evaluate as -1.
 */
struct MimicSageQuasarWindLocals {
  double quasar_energy;   /**< QuasarModeEfficiency * 0.1 * BHaccrete * (C / UnitVelocity)^2 */
  double cold_gas_energy; /**< 0.5 * ColdGas * Vvir * Vvir before the cold ejection */
  double cgm_gas_energy;  /**< 0.5 * CGMgas * Vvir * Vvir, CGM pair selected */
  double hot_gas_energy;  /**< 0.5 * HotGas * Vvir * Vvir, hot pair selected */
  int reservoir_cgm;      /**< 1 if the host's CGM pair was selected (CGMrecipeOn 1, Regime 0) */
  int ejects_cold;        /**< 1 if quasar_energy > cold_gas_energy */
  int ejects_selected;    /**< 1 if quasar_energy > cold_gas_energy + the selected pair's
                               energy */
};

/**
 * @brief The locals of one grow_black_hole call, for tests that inspect the intermediate values
 *
 * A value the call does not form is left as NaN, a flag it does not evaluate as -1.
 */
struct MimicSageBlackHoleLocals {
  double bh_accrete_pre_cap; /**< BHaccrete before its cap at ColdGas */
  double bh_accrete;         /**< BHaccrete after the cap */
  double bh_metallicity;     /**< cold-gas metallicity after the cap, before the debit */
  int coldgas_positive;      /**< 1 if ColdGas > 0.0 */
  int accretion_cap_binds;   /**< 1 if BHaccrete > ColdGas */
  int metals_floor_binds;    /**< 1 if MetalsColdGas < 0.0 after the debit */
  struct MimicSageHydrogenCapLocals hydrogen_caps; /**< the accretion's hydrogen cap; tracks_h2
                                                        is its guard, SFprescription != 0 && != 2 */
  struct MimicSageQuasarWindLocals wind;           /**< the wind the growth calls */
};

/** @brief Mark every value of a wind locals record as not formed */
static inline void mimic_sage_quasar_wind_locals_reset(struct MimicSageQuasarWindLocals *locals) {
  if (locals != NULL) {
    locals->quasar_energy = NAN;
    locals->cold_gas_energy = NAN;
    locals->cgm_gas_energy = NAN;
    locals->hot_gas_energy = NAN;
    locals->reservoir_cgm = -1;
    locals->ejects_cold = -1;
    locals->ejects_selected = -1;
  }
}

/**
 * @brief SAGE26's quasar_mode_wind
 *
 * Ejects the host's whole cold gas when the wind energy exceeds its cold binding energy, and
 * then its whole selected pair (the CGM pair with CGMrecipeOn 1 and the HOST's own Regime 0, the
 * hot pair otherwise) when the wind energy exceeds the ORIGINAL cold binding energy plus that
 * pair's binding energy, both into the host's own EjectedGas, with their metals. No guard on
 * the accreted mass, the efficiency or Vvir: SAGE26 has none.
 *
 * @param halo                    Halo holding the wind host (SAGE26's gal); its float Vvir and its
 *                                galaxy's Regime are read
 * @param bh_accrete              The accreted mass (SAGE26's BHaccrete), 1e10 Msun/h
 * @param quasar_mode_efficiency  QuasarModeEfficiency
 * @param ctx                     Module context with UnitVelocity_in_cm_per_s
 * @param cgm_recipe_on           CGMrecipeOn (0 or 1), which with the host's Regime selects the
 *                                pair of the second test
 * @param locals                  Optional (NULL in production): receives the call's locals
 */
static inline void mimic_apply_quasar_mode_wind(struct Halo *halo, const double bh_accrete,
                                                const double quasar_mode_efficiency,
                                                const struct ModuleContext *ctx,
                                                const int cgm_recipe_on,
                                                struct MimicSageQuasarWindLocals *locals) {
  mimic_sage_quasar_wind_locals_reset(locals);
  /* Calling-contract guards only: SAGE26 is always called with a galaxy and its run
   * parameters, so a valid run never reaches these returns */
  if (halo == NULL || halo->galaxy == NULL || ctx == NULL || ctx->params == NULL) {
    return;
  }

  struct GalaxyData *gal = halo->galaxy;
  const float vvir = (float)halo->Vvir;
  const double unit_velocity = ctx->params->UnitVelocity_in_cm_per_s;

  /* SAGE parity: the wind energy eta * m * c^2 with SAGE26's C = 2.9979e10 cm/s (C_CGS) over
   * UnitVelocity_in_cm_per_s, formed left to right as SAGE26 writes it */
  const double quasar_energy =
      quasar_mode_efficiency * 0.1 * bh_accrete * (C_CGS / unit_velocity) * (C_CGS / unit_velocity);
  const double cold_gas_energy = 0.5 * gal->ColdGas * vvir * vvir;

  // Compare the wind and cold gas energies and eject the cold gas
  const int ejects_cold = quasar_energy > cold_gas_energy;
  if (ejects_cold) {
    gal->EjectedGas += gal->ColdGas;
    gal->MetalsEjectedGas += gal->MetalsColdGas;

    gal->ColdGas = 0.0;
    gal->MetalsColdGas = 0.0;
  }

  /* SAGE parity: the WIND HOST's own Regime selects the pair; the test adds the ORIGINAL cold
   * binding energy and compares the unreduced wind energy, as SAGE26 does */
  const int reservoir_cgm = mimic_sage_cgm_reservoir_selected(cgm_recipe_on, gal->Regime);
  double cgm_gas_energy = NAN;
  double hot_gas_energy = NAN;
  int ejects_selected;
  if (reservoir_cgm) {
    cgm_gas_energy = 0.5 * gal->CGMgas * vvir * vvir;
    ejects_selected = quasar_energy > cold_gas_energy + cgm_gas_energy;
    if (ejects_selected) {
      gal->EjectedGas += gal->CGMgas;
      gal->MetalsEjectedGas += gal->MetalsCGMgas;

      gal->CGMgas = 0.0;
      gal->MetalsCGMgas = 0.0;
    }
  } else {
    hot_gas_energy = 0.5 * gal->HotGas * vvir * vvir;
    ejects_selected = quasar_energy > cold_gas_energy + hot_gas_energy;
    if (ejects_selected) {
      gal->EjectedGas += gal->HotGas;
      gal->MetalsEjectedGas += gal->MetalsHotGas;

      gal->HotGas = 0.0;
      gal->MetalsHotGas = 0.0;
    }
  }

  if (locals != NULL) {
    locals->quasar_energy = quasar_energy;
    locals->cold_gas_energy = cold_gas_energy;
    locals->cgm_gas_energy = cgm_gas_energy;
    locals->hot_gas_energy = hot_gas_energy;
    locals->reservoir_cgm = reservoir_cgm;
    locals->ejects_cold = ejects_cold;
    locals->ejects_selected = ejects_selected;
  }
}

/**
 * @brief SAGE26's grow_black_hole
 *
 * Accretes cold gas onto the black hole, caps the hydrogen for SFprescription 1, runs the
 * quasar wind with the accreted mass and then adds it to QuasarModeBHaccretionMass. A galaxy
 * with no positive ColdGas is left untouched. No guard on the trigger, the growth rate or Vvir:
 * SAGE26 has none, so a zero ratio accretes zero and still runs the wind.
 *
 * @param halo                    Halo holding the galaxy (SAGE26's merger_centralgal or p);
 *                                its float Vvir is read
 * @param mass_ratio              The merger mass ratio or the unstable gas fraction
 * @param black_hole_growth_rate  BlackHoleGrowthRate
 * @param quasar_mode_efficiency  QuasarModeEfficiency, for the wind
 * @param ctx                     Module context with UnitVelocity_in_cm_per_s, for the wind
 * @param sf_prescription         SFprescription (0 or 1), which selects the hydrogen cap
 * @param cgm_recipe_on           CGMrecipeOn (0 or 1), passed to the wind, whose second test
 *                                selects the host's CGM or hot pair by it and the host's Regime
 * @param locals                  Optional (NULL in production): receives the call's locals
 * @return The accreted mass, 0.0 where ColdGas is not positive
 */
static inline double mimic_apply_black_hole_growth(
    struct Halo *halo, const double mass_ratio, const double black_hole_growth_rate,
    const double quasar_mode_efficiency, const struct ModuleContext *ctx, const int sf_prescription,
    const int cgm_recipe_on, struct MimicSageBlackHoleLocals *locals) {
  if (locals != NULL) {
    locals->bh_accrete_pre_cap = NAN;
    locals->bh_accrete = NAN;
    locals->bh_metallicity = NAN;
    locals->coldgas_positive = -1;
    locals->accretion_cap_binds = -1;
    locals->metals_floor_binds = -1;
    locals->hydrogen_caps.tracks_h2 = -1;
    locals->hydrogen_caps.max_h = NAN;
    locals->hydrogen_caps.h2_cap_binds = -1;
    locals->hydrogen_caps.h1_cap_binds = -1;
    mimic_sage_quasar_wind_locals_reset(&locals->wind);
  }
  /* Calling-contract guards only: SAGE26 is always called with a galaxy and its run
   * parameters, so a valid run never reaches this return */
  if (halo == NULL || halo->galaxy == NULL || ctx == NULL || ctx->params == NULL) {
    return 0.0;
  }

  struct GalaxyData *gal = halo->galaxy;
  const int coldgas_positive = gal->ColdGas > 0.0;
  if (locals != NULL) {
    locals->coldgas_positive = coldgas_positive;
  }
  if (!coldgas_positive) {
    return 0.0;
  }

  /* Kauffmann & Haehnelt (2000) with the low-mass suppression at BH_GROWTH_V_KMS = 280 km/s.
   * SAGE parity: 1.0 + SQR(280.0 / Vvir) is one multiply-add, as SAGE26's build computes it */
  const float vvir = (float)halo->Vvir;
  const double v_ratio = SAGE_BH_GROWTH_V_KMS / vvir;
  const double bh_accrete_pre_cap =
      black_hole_growth_rate * mass_ratio / fma(v_ratio, v_ratio, 1.0) * gal->ColdGas;

  // Cannot accrete more gas than is available
  double bh_accrete = bh_accrete_pre_cap;
  const int accretion_cap_binds = bh_accrete > gal->ColdGas;
  if (accretion_cap_binds) {
    bh_accrete = gal->ColdGas;
  }

  const double metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  gal->BlackHoleMass += bh_accrete;
  gal->ColdGas -= bh_accrete;
  /* SAGE parity: the metal debit is one multiply-subtract, as SAGE26's build computes it; a
   * negative result is floored at zero */
  gal->MetalsColdGas = (float)fma(-metallicity, bh_accrete, (double)gal->MetalsColdGas);
  const int metals_floor_binds = gal->MetalsColdGas < 0.0;
  if (metals_floor_binds) {
    gal->MetalsColdGas = 0.0;
  }
  /* SAGE parity: before the wind, H2gas and H1gas are each capped on their own at the float
   * max_h_bh of the remaining cold gas; SAGE26's guard here is SFprescription != 0 && != 2 */
  struct MimicSageHydrogenCapLocals hydrogen_caps = {0, NAN, -1, -1};
  hydrogen_caps.tracks_h2 = sf_prescription != 0 && sf_prescription != 2;
  if (hydrogen_caps.tracks_h2) {
    const float max_h_bh =
        (float)((gal->ColdGas > 0.0f) ? gal->ColdGas * SAGE_HYDROGEN_MASS_FRAC : 0.0f);
    hydrogen_caps.max_h = max_h_bh;
    hydrogen_caps.h2_cap_binds = gal->H2gas > max_h_bh;
    if (hydrogen_caps.h2_cap_binds) {
      gal->H2gas = max_h_bh;
    }
    hydrogen_caps.h1_cap_binds = gal->H1gas > max_h_bh;
    if (hydrogen_caps.h1_cap_binds) {
      gal->H1gas = max_h_bh;
    }
  }

  /* SAGE parity: the wind runs before the accreted mass is recorded, and nothing refreshes
   * H2gas and H1gas after a wind that ejects the whole cold gas */
  mimic_apply_quasar_mode_wind(halo, bh_accrete, quasar_mode_efficiency, ctx, cgm_recipe_on,
                               locals != NULL ? &locals->wind : NULL);

  gal->QuasarModeBHaccretionMass += bh_accrete;

  if (locals != NULL) {
    locals->bh_accrete_pre_cap = bh_accrete_pre_cap;
    locals->bh_accrete = bh_accrete;
    locals->bh_metallicity = metallicity;
    locals->accretion_cap_binds = accretion_cap_binds;
    locals->metals_floor_binds = metals_floor_binds;
    locals->hydrogen_caps = hydrogen_caps;
  }
  return bh_accrete;
}

#endif /* MIMIC_SHARED_SAGE_AGN_PHYSICS_H */
