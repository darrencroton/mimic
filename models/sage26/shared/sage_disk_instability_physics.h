/**
 * @file sage_disk_instability_physics.h
 * @brief SAGE26's disk-instability stellar transfer (Mo, Mao & White 1998 criterion)
 *
 * Ports the part of SAGE26's check_disk_instability before its black-hole and burst
 * calls (R/src/model_disk_instability.c:47-101): the disk mass, the critical mass Mcrit,
 * the transfer of unstable disk stars and their metals into the bulge and its instability
 * channel, the incremental instability-bulge radius that follows the transfer (:81-83,
 * update_instability_bulge_radius, sage_bulge_size_physics.h), and the unstable cold-gas
 * fraction the gas branch hands to grow_black_hole and collisional_starburst_recipe
 * (:101-108). In the package those two continuations are sage26_quasar_mode and
 * sage26_starburst_feedback, which read the returned fraction through the
 * UnstableDiskGasFraction transport property.
 *
 * Arithmetic follows SAGE26's expression shapes and types: the float disk sums, the
 * float Vmax * Vmax product, the double fractions, and double unstable_stars added
 * straight into the float bulge fields. SAGE26's compiled object fuses exactly one
 * multiply-add in this routine (:79, MetalsBulgeMass += metallicity * unstable_stars);
 * it is written below as an explicit fma() and every other expression rounds as
 * written. The radius update's own fused sites are in sage_bulge_size_physics.h.
 *
 * @note Used by sage26_disk_instability (every substep) and by the shared burst
 *       transaction in sage_starburst_physics.h (the minor-merger instability recheck,
 *       SAGE26's R/src/model_mergers.c:807-809, which calls the same
 *       check_disk_instability and continues its gas branch itself).
 */

#ifndef MIMIC_SHARED_SAGE_DISK_INSTABILITY_PHYSICS_H
#define MIMIC_SHARED_SAGE_DISK_INSTABILITY_PHYSICS_H

#include <math.h>

#include "error.h"
#include "module_interface.h"
#include "types.h"
#include "shared/metallicity.h"
#include "shared/sage_bulge_size_physics.h"

/**
 * @brief The locals of SAGE26's check_disk_instability, reported for tests
 *
 * Each value is SAGE26's double local of the same name. A value the path does not
 * form (the routine returns before it, or its branch is not taken) is left as NaN.
 */
struct MimicSageInstabilityLocals {
  double diskmass;              /**< ColdGas + (StellarMass - BulgeMass), formed in float (:54) */
  double mcrit;                 /**< Critical mass after its cap at diskmass (:57-60) */
  double unstable_gas;          /**< gas_fraction * (diskmass - Mcrit) (:64) */
  double unstable_stars;        /**< star_fraction * (diskmass - Mcrit) (:66) */
  double metallicity;           /**< Disk stellar metallicity of the transfer (:75) */
  double unstable_gas_fraction; /**< unstable_gas / ColdGas handed on (:101) */
};

/**
 * @brief SAGE26's check_disk_instability up to its black-hole and burst calls
 *
 * Transfers the unstable disk stars and their metals into BulgeMass, MetalsBulgeMass
 * and InstabilityBulgeMass exactly as SAGE26 does, with no cap on the transferred mass
 * and no clamp of the bulge to the stellar totals, then updates the instability-bulge
 * radius from the disk radius saved before the transfer (a no-op in BulgeSizeOn 0), and
 * returns the unstable cold-gas fraction where SAGE26's gas branch is taken.
 *
 * @param halo                     Halo holding the galaxy (its own Vmax is read)
 * @param ctx                      Module context with the gravitational constant G
 * @param star_forming_disk_factor SAGE26's TOOMRE_DISK_FACTOR, 3.0
 * (R/src/model_disk_instability.c:35); the run files' StarFormingDiskFactor
 * @param sizes                    The bulge-size configuration (BulgeSizeOn, Hubble_h)
 * @param locals                   Optional (NULL allowed): receives SAGE26's locals
 * @return unstable_gas / ColdGas where unstable_gas > 0 and ColdGas > 0 (:90, :101), else 0.0
 */
static inline double mimic_sage_check_disk_instability_with_sizes(
    struct Halo *halo, const struct ModuleContext *ctx, const double star_forming_disk_factor,
    const struct MimicSageBulgeSize *sizes, struct MimicSageInstabilityLocals *locals) {
  if (locals != NULL) {
    locals->diskmass = NAN;
    locals->mcrit = NAN;
    locals->unstable_gas = NAN;
    locals->unstable_stars = NAN;
    locals->metallicity = NAN;
    locals->unstable_gas_fraction = NAN;
  }
  /* Calling-contract guards: SAGE26 is always called with a galaxy and a positive G */
  if (halo == NULL || halo->galaxy == NULL || ctx == NULL || ctx->params == NULL ||
      ctx->params->G <= 0.0 || sizes == NULL) {
    return 0.0;
  }

  struct GalaxyData *gal = halo->galaxy;
  double unstable_gas_fraction = 0.0;

  /* SAGE parity: the disk mass is a float sum of the float reservoirs, widened to
   * double once (R/src/model_disk_instability.c:54) */
  const double diskmass = gal->ColdGas + (gal->StellarMass - gal->BulgeMass);
  if (locals != NULL) {
    locals->diskmass = diskmass;
  }
  /* SAGE parity: SAGE26 tests diskmass > 0.0 (R/src/model_disk_instability.c:55), so a NaN disk
   * mass skips the routine */
  if (diskmass > 0.0) {
    /* SAGE parity: the float Vmax * Vmax product, then SAGE26's double 3.0 times the float
     * DiskScaleRadius, divided by G (R/src/model_disk_instability.c:57, TOOMRE_DISK_FACTOR :35);
     * capped at the disk mass (:58-60) */
    double mcrit = halo->Vmax * halo->Vmax * (star_forming_disk_factor * gal->DiskScaleRadius) /
                   ctx->params->G;
    if (mcrit > diskmass) {
      mcrit = diskmass;
    }

    const double gas_fraction = gal->ColdGas / diskmass;
    const double unstable_gas = gas_fraction * (diskmass - mcrit);
    const double star_fraction = 1.0 - gas_fraction;
    const double unstable_stars = star_fraction * (diskmass - mcrit);
    if (locals != NULL) {
      locals->mcrit = mcrit;
      locals->unstable_gas = unstable_gas;
      locals->unstable_stars = unstable_stars;
    }

    /* SAGE parity: the disk radius is saved before any transfer, for the incremental
     * instability-bulge radius (R/src/model_disk_instability.c:68-70) */
    const double old_disk_radius = gal->DiskScaleRadius;

    if (unstable_stars > 0.0) {
      /* SAGE parity: the disk metallicity from the float disk differences
       * (R/src/model_disk_instability.c:75) */
      const double metallicity = mimic_get_metallicity(
          gal->StellarMass - gal->BulgeMass, gal->MetalsStellarMass - gal->MetalsBulgeMass);
      if (locals != NULL) {
        locals->metallicity = metallicity;
      }

      /* SAGE parity: double unstable_stars is added straight into the float bulge, its
       * instability channel and the bulge metals, with no cap on the transfer and no
       * clamp of the bulge to StellarMass or MetalsStellarMass
       * (R/src/model_disk_instability.c:77-79). The float stores can leave BulgeMass or
       * MetalsBulgeMass one rounding above the stellar totals, as SAGE26 does. SAGE26's object
       * fuses the metals update (:79). */
      gal->BulgeMass += unstable_stars;
      gal->InstabilityBulgeMass += unstable_stars;
      gal->MetalsBulgeMass = (float)fma(metallicity, unstable_stars, (double)gal->MetalsBulgeMass);

      /* SAGE parity: the incremental instability-bulge radius follows the three stores,
       * from the post-store channel mass and the disk radius saved above
       * (R/src/model_disk_instability.c:81-83). In BulgeSizeOn 0 it returns at once
       * (R/src/model_misc.c:395), so every bulge radius keeps the value it had (not
       * zeroed) while the channel mass above is written */
      mimic_sage_update_instability_bulge_radius(gal, unstable_stars, old_disk_radius, sizes, NULL);

      DEBUG_LOG("Halo %lld: Disk unstable - transferred %.3e to bulge", halo->HaloNr,
                unstable_stars);
    }

    /* SAGE parity: the gas branch needs unstable_gas > 0.0 and ColdGas > 0.0
     * (R/src/model_disk_instability.c:90); its fraction (:101) goes to grow_black_hole
     * and collisional_starburst_recipe (:102-108), which are sage26_quasar_mode and
     * sage26_starburst_feedback in the package */
    if (unstable_gas > 0.0 && gal->ColdGas > 0.0) {
      unstable_gas_fraction = unstable_gas / gal->ColdGas;
      if (locals != NULL) {
        locals->unstable_gas_fraction = unstable_gas_fraction;
      }
      DEBUG_LOG("Halo %lld: Unstable gas fraction = %.4f", halo->HaloNr, unstable_gas_fraction);
    }
  }

  return unstable_gas_fraction;
}

/**
 * @brief SAGE26's check_disk_instability in the R0 bulge-size configuration (BulgeSizeOn 0)
 *
 * mimic_sage_check_disk_instability_with_sizes() with BulgeSizeOn 0, in which the radius
 * update returns at once (R/src/model_misc.c:395): for callers that run R0's sizes.
 *
 * @param halo                     Halo holding the galaxy
 * @param ctx                      Module context with the gravitational constant G
 * @param star_forming_disk_factor SAGE26's TOOMRE_DISK_FACTOR, 3.0
 * (R/src/model_disk_instability.c:35)
 * @param locals                   Optional (NULL allowed): receives SAGE26's locals
 * @return The unstable cold-gas fraction where SAGE26's gas branch is taken, else 0.0
 */
static inline double mimic_sage_check_disk_instability(struct Halo *halo,
                                                       const struct ModuleContext *ctx,
                                                       const double star_forming_disk_factor,
                                                       struct MimicSageInstabilityLocals *locals) {
  const struct MimicSageBulgeSize r0_sizes = {MIMIC_SAGE_BULGE_SIZE_OFF, 0.0};
  return mimic_sage_check_disk_instability_with_sizes(halo, ctx, star_forming_disk_factor,
                                                      &r0_sizes, locals);
}

/**
 * @brief Apply SAGE26's disk-instability stellar transfer and return the unstable gas fraction
 *
 * mimic_sage_check_disk_instability_with_sizes() without its locals; the entry point
 * sage26_disk_instability uses. The burst transaction's post-minor-merger recheck
 * (sage_starburst_physics.h) calls mimic_sage_check_disk_instability_with_sizes() directly,
 * for its locals' record of whether SAGE26's gas branch is taken.
 *
 * @param halo                     Halo holding the galaxy
 * @param ctx                      Module context with the gravitational constant G
 * @param star_forming_disk_factor SAGE26's TOOMRE_DISK_FACTOR, 3.0
 * (R/src/model_disk_instability.c:35)
 * @param sizes                    The bulge-size configuration (BulgeSizeOn, Hubble_h)
 * @return The unstable cold-gas fraction where SAGE26's gas branch is taken, else 0.0
 */
static inline double mimic_sage_apply_disk_instability(struct Halo *halo,
                                                       const struct ModuleContext *ctx,
                                                       const double star_forming_disk_factor,
                                                       const struct MimicSageBulgeSize *sizes) {
  return mimic_sage_check_disk_instability_with_sizes(halo, ctx, star_forming_disk_factor, sizes,
                                                      NULL);
}

#endif /* MIMIC_SHARED_SAGE_DISK_INSTABILITY_PHYSICS_H */
