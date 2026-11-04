/**
 * @file sage_sf_feedback_physics.h
 * @brief SAGE26's star-formation and supernova-feedback commits, shared by disk SF and bursts
 *
 * SAGE26 commits every star-formation event through update_from_star_formation()
 * and every supernova-feedback event through update_from_feedback()
 * (R/src/model_starformation_and_feedback.c:1119-1136 and :1146-1251, R = the
 * staged SAGE26 source). Disk star formation calls them from
 * starformation_and_feedback() (:1062-1085) and every collisional starburst from
 * collisional_starburst_recipe() (R/src/model_mergers.c:759-760, :788-791). This
 * header holds the package's one implementation of both, used by
 * sage26_apply_star_formation_supernova for disk star formation and by
 * mimic_apply_collisional_starburst (sage_starburst_physics.h) for both burst
 * channels, so that every caller commits an event exactly as SAGE26 does. It also
 * holds the supernova code constants both feedback callers derive at init.
 *
 * Only the R0 path is ported: SFprescription 0, SupernovaRecipeOn 1, CGMrecipeOn 0.
 *
 * Rounding points. Every reservoir is a float, every mass and metallicity a double,
 * so each `+=`/`-=` widens the reservoir, operates in double and narrows the result.
 * SAGE26's reference objects (Apple clang, -ffp-contract=on, arm64; enumerated with
 * objdump -d -l) fuse six multiply-adds here, each a single correctly rounded
 * operation: all four updates of update_from_star_formation (:1123-1126, two
 * fmla.2d over the float pairs ColdGas/StellarMass and MetalsColdGas/MetalsStellarMass
 * with coefficients [-(1 - R), (1 - R)] and [-(Z * (1 - R)), Z * (1 - R)] times stars,
 * Z * (1 - R) rounded first), and the cold-metal debit (:1168) and the hot-metal
 * credit (:1229) of update_from_feedback. They are written as explicit fma() calls,
 * which round identically on every platform whose fma is correctly rounded.
 * Everything SAGE26 does not fuse (the metal transfer of the ejection, the gas
 * moves) is formed in separate statements, so no compiler contracts it either. The
 * oracle's uff_rounding_* fixtures pin the two feedback sites; no oracle fixture
 * pins update_from_star_formation's, which the apply step's unit tests pin with bit
 * patterns produced by SAGE26's own compiled routine.
 *
 * @note SAGE26's prescription-0 path does not resynchronise H2gas and H1gas after the
 *       depletion (:1128-1135, :1169-1172 run only for SFprescription values that track
 *       H2), so on R0 neither routine writes H1gas or H2gas.
 */

#ifndef MIMIC_SHARED_SAGE_SF_FEEDBACK_PHYSICS_H
#define MIMIC_SHARED_SAGE_SF_FEEDBACK_PHYSICS_H

#include <math.h>
#include <stddef.h>

#include "types.h"
#include "shared/metallicity.h"
#include "module_system/physical_constants.h"

/**
 * @brief SAGE26's supernova energy in code units, EnergySNcode
 *
 * SAGE26 forms EnergySN / UnitEnergy_in_cgs * Hubble_h left to right
 * (R/src/core_init.c:102), with EnergySN 1.0e51 erg (R/input/millennium.par:88),
 * the package's ENERGY_SN.
 */
static inline double mimic_sage_energy_sn_code(const double unit_energy_in_cgs,
                                               const double hubble_h) {
  return ENERGY_SN / unit_energy_in_cgs * hubble_h;
}

/**
 * @brief SAGE26's supernova efficiency in code units, EtaSNcode
 *
 * SAGE26 forms EtaSN * (UnitMass_in_g / SOLAR_MASS) / Hubble_h
 * (R/src/core_init.c:103), with EtaSN 5.0e-3 (R/input/millennium.par:89), the
 * package's ETA_SN, and SOLAR_MASS 1.989e33 g (R/src/macros.h:24), the package's
 * SOLAR_MASS.
 */
static inline double mimic_sage_eta_sn_code(const double unit_mass_in_g, const double hubble_h) {
  return ETA_SN * (unit_mass_in_g / SOLAR_MASS) / hubble_h;
}

/**
 * @brief Commit one star-formation event: SAGE26's update_from_star_formation
 *
 * Locks (1 - RecycleFraction) * stars of cold gas into stars, with the cold gas's
 * metallicity (R/src/model_starformation_and_feedback.c:1119-1126). The recycled
 * fraction stays in the cold gas. Each of the four updates is one fused multiply-add,
 * as in SAGE26's build.
 *
 * @param gal               Galaxy forming the stars
 * @param stars             Stellar mass formed, 1e10 Msun/h (double, as SAGE26's argument)
 * @param metallicity       Cold-gas metallicity at the call (get_metallicity of the cold gas)
 * @param recycle_fraction  RecycleFraction
 */
static inline void mimic_sage_update_from_star_formation(struct GalaxyData *gal, const double stars,
                                                         const double metallicity,
                                                         const double recycle_fraction) {
  /* SAGE parity: SAGE26's build fuses all four updates (:1123-1126): (1 - R) and
   * Z * (1 - R) are rounded first, then each widened float reservoir takes one fused
   * multiply-add with stars, the debits with the negated coefficient (fmla.2d over
   * the lanes [ColdGas, StellarMass] and [MetalsColdGas, MetalsStellarMass]).
   * (1 - RecycleFraction) is formed from the int literal 1, which converts exactly
   * to 1.0. */
  const double locked_fraction = 1.0 - recycle_fraction;
  const double metals_fraction = metallicity * locked_fraction;

  gal->ColdGas = (float)fma(-locked_fraction, stars, (double)gal->ColdGas);
  gal->StellarMass = (float)fma(locked_fraction, stars, (double)gal->StellarMass);
  gal->MetalsColdGas = (float)fma(-metals_fraction, stars, (double)gal->MetalsColdGas);
  gal->MetalsStellarMass = (float)fma(metals_fraction, stars, (double)gal->MetalsStellarMass);
}

/**
 * @brief The locals of one update_from_feedback call, for tests that compare them with the oracle
 *
 * `hot_gas_at_cap` and `metals_hot_gas_at_cap` are the central's HotGas and
 * MetalsHotGas after the reheated gas is added and before the ejection debit: the
 * reservoirs the two ejection caps compare against (:1232-1240).
 */
struct MimicSageFeedbackLocals {
  double reheated_mass_clamped;      /**< reheated mass after the clamp to ColdGas (:1153-1156) */
  double ejected_mass_capped;        /**< ejected mass after the cap to the central's HotGas */
  double metallicity_hot;            /**< the central's hot-gas metallicity after the reheating */
  double metals_hot_to_eject;        /**< metallicity_hot * ejected mass, before its cap */
  double metals_hot_to_eject_capped; /**< after the cap to the central's MetalsHotGas */
  float hot_gas_at_cap;              /**< the central's HotGas the gas cap compares against */
  float metals_hot_gas_at_cap; /**< the central's MetalsHotGas the metal cap compares against */
  int reheat_clamp_binds;      /**< 1 if reheated mass > ColdGas */
  int ejected_cap_binds;       /**< 1 if ejected mass > HotGas */
  int metals_cap_binds;        /**< 1 if the metal transfer > MetalsHotGas */
};

/**
 * @brief Commit one supernova-feedback event: SAGE26's update_from_feedback, non-CGM path
 *
 * Reheats cold gas of `gal` into the FoF central's hot halo, ejects hot gas of the
 * central into its ejected reservoir, and accumulates the reheated mass into
 * `gal`'s SupernovaOutflowRate (R/src/model_starformation_and_feedback.c:1146-1251,
 * CGMrecipeOn 0 branch :1224-1247):
 *
 * - the reheated mass is clamped to the forming galaxy's ColdGas (:1153-1156);
 * - the ejected mass is capped to the central's HotGas after the reheating
 *   (:1232-1234), and the metal transfer metallicityHot * ejected mass is capped
 *   separately to the central's MetalsHotGas (:1237-1240), so metals above gas
 *   cannot drive MetalsHotGas negative;
 * - OutflowRate += reheated mass is a float accumulation of the undivided mass
 *   (:1249); sage26_finalize_cooling_heating multiplies it by 1/deltaT once per
 *   halo, as SAGE26 does after its last substep (R/src/core_build_model.c:603).
 *
 * SAGE26's assertions on the reheated mass (:1158-1163) abort the run; they guard
 * states R0 does not produce and are not mirrored.
 *
 * @param gal            Galaxy whose cold gas is reheated (SAGE26's p)
 * @param central_gal    The FoF central receiving the reheated and ejected gas
 *                       (SAGE26's centralgal); may be gal itself
 * @param reheated_mass  Reheated mass, 1e10 Msun/h (double)
 * @param ejected_mass   Ejected mass, 1e10 Msun/h (double)
 * @param metallicity    Cold-gas metallicity of gal at the call (get_metallicity)
 * @param locals         Optional: receives the call's locals (NULL in production)
 */
static inline void mimic_sage_update_from_feedback(struct GalaxyData *gal,
                                                   struct GalaxyData *central_gal,
                                                   double reheated_mass, double ejected_mass,
                                                   const double metallicity,
                                                   struct MimicSageFeedbackLocals *locals) {
  /* SAGE parity: the clamp to the remaining cold gas, for floating-point overshoot
   * (R/src/model_starformation_and_feedback.c:1153-1156) */
  const int reheat_clamp_binds = reheated_mass > gal->ColdGas;
  if (reheat_clamp_binds) {
    reheated_mass = gal->ColdGas;
  }

  // Remove reheated mass from cold gas (:1167-1168). SAGE parity: SAGE26's build
  // fuses the metal debit into one multiply-subtract (:1168).
  gal->ColdGas -= reheated_mass;
  gal->MetalsColdGas = (float)fma(-metallicity, reheated_mass, (double)gal->MetalsColdGas);

  // Add reheated gas to the central's hot gas (:1228-1229). SAGE parity: the metal
  // credit is fused into one multiply-add in SAGE26's build (:1229).
  central_gal->HotGas += reheated_mass;
  central_gal->MetalsHotGas =
      (float)fma(metallicity, reheated_mass, (double)central_gal->MetalsHotGas);

  // Check if ejection is possible from HotGas (:1232-1234)
  const float hot_gas_at_cap = central_gal->HotGas;
  const int ejected_cap_binds = ejected_mass > central_gal->HotGas;
  if (ejected_cap_binds) {
    ejected_mass = central_gal->HotGas;
  }
  const double metallicity_hot =
      mimic_get_metallicity(central_gal->HotGas, central_gal->MetalsHotGas);

  /* SAGE parity: the metal transfer is capped on its own to MetalsHotGas (:1237-1240) */
  const double metals_hot_to_eject = metallicity_hot * ejected_mass;
  double metals_hot_to_eject_capped = metals_hot_to_eject;
  const float metals_hot_gas_at_cap = central_gal->MetalsHotGas;
  const int metals_cap_binds = metals_hot_to_eject_capped > central_gal->MetalsHotGas;
  if (metals_cap_binds) {
    metals_hot_to_eject_capped = central_gal->MetalsHotGas;
  }

  // Eject from HotGas to EjectedMass (:1243-1246)
  central_gal->HotGas -= ejected_mass;
  central_gal->MetalsHotGas -= metals_hot_to_eject_capped;
  central_gal->EjectedGas += ejected_mass;
  central_gal->MetalsEjectedGas += metals_hot_to_eject_capped;

  /* SAGE parity: OutflowRate accumulates the undivided reheated mass in float
   * (:1249); it becomes a rate only at the end-of-halo normalisation */
  gal->SupernovaOutflowRate += reheated_mass;

  if (locals != NULL) {
    locals->reheated_mass_clamped = reheated_mass;
    locals->ejected_mass_capped = ejected_mass;
    locals->metallicity_hot = metallicity_hot;
    locals->metals_hot_to_eject = metals_hot_to_eject;
    locals->metals_hot_to_eject_capped = metals_hot_to_eject_capped;
    locals->hot_gas_at_cap = hot_gas_at_cap;
    locals->metals_hot_gas_at_cap = metals_hot_gas_at_cap;
    locals->reheat_clamp_binds = reheat_clamp_binds;
    locals->ejected_cap_binds = ejected_cap_binds;
    locals->metals_cap_binds = metals_cap_binds;
  }
}

#endif /* MIMIC_SHARED_SAGE_SF_FEEDBACK_PHYSICS_H */
