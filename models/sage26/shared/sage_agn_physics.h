/**
 * @file sage_agn_physics.h
 * @brief SAGE26's black-hole growth and quasar-mode wind (grow_black_hole, quasar_mode_wind)
 *
 * Ports SAGE26's grow_black_hole and quasar_mode_wind (R/src/model_mergers.c:346-441,
 * R = the staged SAGE26 source) with CGMrecipeOn 0 and SFprescription 0 (R0, R1) or 1 (R2).
 * Three package callers use them, as SAGE26 calls grow_black_hole from three places:
 * sage26_quasar_mode for a merger (R/src/model_mergers.c:262-264) and for a disk
 * instability (R/src/model_disk_instability.c:102-104, through the
 * UnstableDiskGasFraction hand-off), and the collisional burst's post-minor-merger
 * instability recheck in shared/sage_starburst_physics.h (the same
 * check_disk_instability, reached from R/src/model_mergers.c:807-809).
 *
 * grow_black_hole, for a galaxy with positive ColdGas (:350):
 *
 *     BHaccrete = BlackHoleGrowthRate * mass_ratio / (1 + (280 / Vvir)^2) * ColdGas   (:351-352)
 *     BHaccrete = min(BHaccrete, ColdGas)                                         (:354-357)
 *     BlackHoleMass += BHaccrete; ColdGas -= BHaccrete;                           (:360-361)
 *     MetalsColdGas -= Z_cold * BHaccrete, floored at 0                           (:362-365)
 *     if SFprescription != 0 && != 2: H2gas, H1gas each capped at max_h_bh         (:366-372)
 *     quasar_mode_wind(BHaccrete)                                                  (:374)
 *     QuasarModeBHaccretionMass += BHaccrete                                      (:376)
 *
 * quasar_mode_wind (:390-441, the CGMrecipeOn 0 branch :430-440): the wind energy
 * QuasarModeEfficiency * 0.1 * BHaccrete * (C / UnitVelocity)^2 is compared with the cold
 * binding energy 0.5 * ColdGas * Vvir^2 and, separately, with that cold binding energy plus
 * the hot binding energy 0.5 * HotGas * Vvir^2; each comparison it exceeds ejects the whole
 * reservoir into the galaxy's own EjectedGas. The energy used by the cold ejection is not
 * subtracted before the hot comparison.
 *
 * The hydrogen cap (SFprescription 1, R2). After the accretion and the negative-metal floor and
 * before the wind, SAGE26 caps H2gas and H1gas, each on its own and strictly, at the float
 * max_h_bh = (ColdGas > 0.0f) ? ColdGas * 0.74 : 0.0f (the double product narrowed) of the cold
 * gas the accretion leaves (:366-372). Its guard is written at the site as SFprescription != 0
 * && SFprescription != 2, not through sf_prescription_tracks_h2 (the burst refresh's guard);
 * the package writes it the same way. The wind may then eject the whole cold gas, and nothing
 * refreshes the hydrogen after it (:374, :397-403): the caps are not a joint H1 + H2 budget and
 * the wind adds no synchronisation.
 *
 * Rounding points. Every reservoir is a float and every SAGE26 local a double, so each
 * `+=`/`-=` widens the reservoir, operates in double and narrows the result. SAGE26's
 * object fuses two multiply-adds here (the oracle README's c06 fused-instruction table):
 * the denominator 1.0 + SQR(280 / Vvir) (:352, fmadd) and the cold-metal debit (:362,
 * fmsub). Both are written as explicit fma() calls; every other expression rounds as
 * written, one C expression per SAGE26 expression, and quasar_mode_wind fuses nothing.
 * Vvir is read as the float SAGE26 holds (R/src/core_allvars.h:131), as the package's
 * other ports read it.
 */

#ifndef MIMIC_SHARED_SAGE_AGN_PHYSICS_H
#define MIMIC_SHARED_SAGE_AGN_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "constants.h"
#include "module_interface.h"
#include "types.h"
#include "shared/metallicity.h"
#include "shared/sage_constants.h"
#include "shared/sage_sf_feedback_physics.h"
#include "module_system/physical_constants.h"

/* SAGE26's BH_GROWTH_V_KMS, the halo velocity below which black-hole growth is
 * suppressed (R/src/model_mergers.c:45), km/s */
#define SAGE_BH_GROWTH_V_KMS 280.0

/**
 * @brief The locals of one quasar_mode_wind call, for tests that compare them with the oracle
 *
 * A value the call does not form is left as NaN, a flag it does not evaluate as -1.
 */
struct MimicSageQuasarWindLocals {
  double quasar_energy;   /**< QuasarModeEfficiency * 0.1 * BHaccrete * (C / UnitVelocity)^2 */
  double cold_gas_energy; /**< 0.5 * ColdGas * Vvir * Vvir before the cold ejection (:394) */
  double hot_gas_energy;  /**< 0.5 * HotGas * Vvir * Vvir (:432) */
  int ejects_cold;        /**< 1 if quasar_energy > cold_gas_energy (:397) */
  int ejects_hot;         /**< 1 if quasar_energy > cold_gas_energy + hot_gas_energy (:434) */
};

/**
 * @brief The locals of one grow_black_hole call, for tests that compare them with the oracle
 *
 * A value the call does not form is left as NaN, a flag it does not evaluate as -1.
 */
struct MimicSageBlackHoleLocals {
  double bh_accrete_pre_cap; /**< BHaccrete before its cap at ColdGas (:351-352) */
  double bh_accrete;         /**< BHaccrete after the cap (:354-357) */
  double bh_metallicity;     /**< cold-gas metallicity after the cap, before the debit (:359) */
  int coldgas_positive;      /**< 1 if ColdGas > 0.0 (:350) */
  int accretion_cap_binds;   /**< 1 if BHaccrete > ColdGas (:355) */
  int metals_floor_binds;    /**< 1 if MetalsColdGas < 0.0 after the debit (:363) */
  struct MimicSageHydrogenCapLocals hydrogen_caps; /**< the accretion's hydrogen cap
                                                        (:366-372); tracks_h2 is its guard,
                                                        SFprescription != 0 && != 2 */
  struct MimicSageQuasarWindLocals wind;           /**< the wind the growth calls (:374) */
};

/** @brief Mark every value of a wind locals record as not formed */
static inline void mimic_sage_quasar_wind_locals_reset(struct MimicSageQuasarWindLocals *locals) {
  if (locals != NULL) {
    locals->quasar_energy = NAN;
    locals->cold_gas_energy = NAN;
    locals->hot_gas_energy = NAN;
    locals->ejects_cold = -1;
    locals->ejects_hot = -1;
  }
}

/**
 * @brief SAGE26's quasar_mode_wind, non-CGM branch (R/src/model_mergers.c:390-441)
 *
 * Ejects the galaxy's whole cold gas when the wind energy exceeds its cold binding energy,
 * and its whole hot gas when the wind energy exceeds the cold plus hot binding energy, both
 * into the galaxy's own EjectedGas, with their metals. No guard on the accreted mass, the
 * efficiency or Vvir: SAGE26 has none (:390-441).
 *
 * @param halo                    Halo holding the galaxy (SAGE26's gal); its float Vvir is read
 * @param bh_accrete              The accreted mass (SAGE26's BHaccrete), 1e10 Msun/h
 * @param quasar_mode_efficiency  QuasarModeEfficiency
 * @param ctx                     Module context with UnitVelocity_in_cm_per_s
 * @param locals                  Optional (NULL in production): receives the call's locals
 */
static inline void mimic_apply_quasar_mode_wind(struct Halo *halo, const double bh_accrete,
                                                const double quasar_mode_efficiency,
                                                const struct ModuleContext *ctx,
                                                struct MimicSageQuasarWindLocals *locals) {
  mimic_sage_quasar_wind_locals_reset(locals);
  /* Calling-contract guards only: SAGE26 is always called with a galaxy and its run
   * parameters; no R0 call reaches these returns */
  if (halo == NULL || halo->galaxy == NULL || ctx == NULL || ctx->params == NULL) {
    return;
  }

  struct GalaxyData *gal = halo->galaxy;
  const float vvir = (float)halo->Vvir;
  const double unit_velocity = ctx->params->UnitVelocity_in_cm_per_s;

  /* SAGE parity: the wind energy eta * m * c^2 with C = 2.9979e10 cm/s
   * (R/src/macros.h:30, the package's C_CGS) over UnitVelocity_in_cm_per_s, formed left
   * to right as SAGE26 writes it (R/src/model_mergers.c:393) */
  const double quasar_energy =
      quasar_mode_efficiency * 0.1 * bh_accrete * (C_CGS / unit_velocity) * (C_CGS / unit_velocity);
  const double cold_gas_energy = 0.5 * gal->ColdGas * vvir * vvir;

  // Compare the wind and cold gas energies and eject the cold gas (:396-403)
  const int ejects_cold = quasar_energy > cold_gas_energy;
  if (ejects_cold) {
    gal->EjectedGas += gal->ColdGas;
    gal->MetalsEjectedGas += gal->MetalsColdGas;

    gal->ColdGas = 0.0;
    gal->MetalsColdGas = 0.0;
  }

  /* SAGE parity: the hot comparison adds the ORIGINAL cold binding energy to the hot
   * binding energy and compares the unreduced wind energy (R/src/model_mergers.c:430-440,
   * CGMrecipeOn 0); the cold ejection above uses none of the wind energy up */
  const double hot_gas_energy = 0.5 * gal->HotGas * vvir * vvir;
  const int ejects_hot = quasar_energy > cold_gas_energy + hot_gas_energy;
  if (ejects_hot) {
    gal->EjectedGas += gal->HotGas;
    gal->MetalsEjectedGas += gal->MetalsHotGas;

    gal->HotGas = 0.0;
    gal->MetalsHotGas = 0.0;
  }

  if (locals != NULL) {
    locals->quasar_energy = quasar_energy;
    locals->cold_gas_energy = cold_gas_energy;
    locals->hot_gas_energy = hot_gas_energy;
    locals->ejects_cold = ejects_cold;
    locals->ejects_hot = ejects_hot;
  }
}

/**
 * @brief SAGE26's grow_black_hole (R/src/model_mergers.c:346-378)
 *
 * Accretes cold gas onto the black hole, caps the hydrogen for SFprescription 1, runs the
 * quasar wind with the accreted mass and then adds it to QuasarModeBHaccretionMass. A galaxy
 * with no positive ColdGas is left untouched (:350). No guard on the trigger, the growth rate
 * or Vvir: SAGE26 has none, so a zero ratio accretes zero and still runs the wind (:350-376).
 *
 * @param halo                    Halo holding the galaxy (SAGE26's merger_centralgal or p);
 *                                its float Vvir is read
 * @param mass_ratio              The merger mass ratio or the unstable gas fraction
 * @param black_hole_growth_rate  BlackHoleGrowthRate
 * @param quasar_mode_efficiency  QuasarModeEfficiency, for the wind
 * @param ctx                     Module context with UnitVelocity_in_cm_per_s, for the wind
 * @param sf_prescription         SFprescription (0 or 1), which selects the hydrogen cap
 * @param locals                  Optional (NULL in production): receives the call's locals
 * @return The accreted mass, 0.0 where ColdGas is not positive
 */
static inline double mimic_apply_black_hole_growth(struct Halo *halo, const double mass_ratio,
                                                   const double black_hole_growth_rate,
                                                   const double quasar_mode_efficiency,
                                                   const struct ModuleContext *ctx,
                                                   const int sf_prescription,
                                                   struct MimicSageBlackHoleLocals *locals) {
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
   * parameters; no R0 call reaches this return */
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

  /* SAGE parity: Kauffmann & Haehnelt (2000) with the low-mass suppression at
   * BH_GROWTH_V_KMS = 280 km/s (R/src/model_mergers.c:45, :351-352), formed left to right;
   * SAGE26's object fuses 1.0 + SQR(280.0 / Vvir) into one multiply-add, written here as
   * fma() */
  const float vvir = (float)halo->Vvir;
  const double v_ratio = SAGE_BH_GROWTH_V_KMS / vvir;
  const double bh_accrete_pre_cap =
      black_hole_growth_rate * mass_ratio / fma(v_ratio, v_ratio, 1.0) * gal->ColdGas;

  // Cannot accrete more gas than is available (:354-357)
  double bh_accrete = bh_accrete_pre_cap;
  const int accretion_cap_binds = bh_accrete > gal->ColdGas;
  if (accretion_cap_binds) {
    bh_accrete = gal->ColdGas;
  }

  const double metallicity = mimic_get_metallicity(gal->ColdGas, gal->MetalsColdGas);
  gal->BlackHoleMass += bh_accrete;
  gal->ColdGas -= bh_accrete;
  /* SAGE parity: SAGE26's object fuses the metal debit into one multiply-subtract
   * (R/src/model_mergers.c:362), and floors a negative result at zero (:363-365) */
  gal->MetalsColdGas = (float)fma(-metallicity, bh_accrete, (double)gal->MetalsColdGas);
  const int metals_floor_binds = gal->MetalsColdGas < 0.0;
  if (metals_floor_binds) {
    gal->MetalsColdGas = 0.0;
  }
  /* SAGE parity: after the accretion and the metal floor, and before the wind, the hydrogen of
   * the remaining cold gas caps H2gas and H1gas, each on its own and strictly, at the float
   * max_h_bh, 0 when no cold gas is left; the guard is the site's own SFprescription != 0 &&
   * SFprescription != 2 (R/src/model_mergers.c:366-372), so SFprescription 0 (R0, R1) skips it */
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

  /* SAGE parity: the wind runs before the accreted mass is recorded
   * (R/src/model_mergers.c:374, :376); a wind that ejects the whole cold gas leaves H2gas and
   * H1gas as the cap left them, with no refresh after it (:397-403) */
  mimic_apply_quasar_mode_wind(halo, bh_accrete, quasar_mode_efficiency, ctx,
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
