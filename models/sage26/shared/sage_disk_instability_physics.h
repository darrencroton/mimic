/**
 * @file sage_disk_instability_physics.h
 * @brief SAGE26's disk-instability stellar transfer (Mo, Mao & White 1998 criterion)
 *
 * Implements the part of SAGE26's check_disk_instability before its black-hole and
 * burst calls: the disk mass, the critical mass Mcrit, the transfer of unstable disk
 * stars and their metals into the bulge and its instability channel, the
 * instability-bulge radius update that follows (update_instability_bulge_radius,
 * sage_bulge_size_physics.h), and the unstable cold-gas fraction the gas branch hands
 * to grow_black_hole and collisional_starburst_recipe. In the package those two
 * continuations are sage26_quasar_mode and sage26_starburst_feedback, which read the
 * fraction through the UnstableDiskGasFraction transport property.
 *
 * @note Used by sage26_disk_instability (every substep) and by the minor-merger
 *       instability recheck in sage_starburst_physics.h.
 *
 * Ported from SAGE26 (MIT; Copyright (c) 2019-2026 Semi-Analytic Galaxy Evolution (SAGE)); see
 * models/sage26/LICENSE-SAGE26.
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
  double diskmass;              /**< ColdGas + (StellarMass - BulgeMass), formed in float */
  double mcrit;                 /**< Critical mass after its cap at diskmass */
  double unstable_gas;          /**< gas_fraction * (diskmass - Mcrit) */
  double unstable_stars;        /**< star_fraction * (diskmass - Mcrit) */
  double metallicity;           /**< Disk stellar metallicity of the transfer */
  double unstable_gas_fraction; /**< unstable_gas / ColdGas handed on */
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
 * @param star_forming_disk_factor SAGE26's TOOMRE_DISK_FACTOR, 3.0; the run files'
 *                                 StarFormingDiskFactor
 * @param sizes                    The bulge-size configuration (BulgeSizeOn, Hubble_h)
 * @param locals                   Optional (NULL allowed): receives SAGE26's locals
 * @return unstable_gas / ColdGas where unstable_gas > 0 and ColdGas > 0, else 0.0
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

  /* SAGE parity: the disk mass is a float sum of the float reservoirs, widened once */
  const double diskmass = gal->ColdGas + (gal->StellarMass - gal->BulgeMass);
  if (locals != NULL) {
    locals->diskmass = diskmass;
  }
  /* SAGE parity: SAGE26 tests diskmass > 0.0, so a NaN disk mass skips the routine */
  if (diskmass > 0.0) {
    /* SAGE parity: the float Vmax * Vmax product times TOOMRE_DISK_FACTOR * DiskScaleRadius,
     * divided by G and capped at the disk mass */
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
     * instability-bulge radius */
    const double old_disk_radius = gal->DiskScaleRadius;

    if (unstable_stars > 0.0) {
      /* SAGE parity: the disk metallicity from the float disk differences */
      const double metallicity = mimic_get_metallicity(
          gal->StellarMass - gal->BulgeMass, gal->MetalsStellarMass - gal->MetalsBulgeMass);
      if (locals != NULL) {
        locals->metallicity = metallicity;
      }

      /* SAGE parity: no cap on the transfer and no clamp of the bulge to the stellar
       * totals, so BulgeMass can end one float rounding above StellarMass, as in SAGE26;
       * the metals update is one fma, as SAGE26 evaluates it */
      gal->BulgeMass += unstable_stars;
      gal->InstabilityBulgeMass += unstable_stars;
      gal->MetalsBulgeMass = (float)fma(metallicity, unstable_stars, (double)gal->MetalsBulgeMass);

      /* The instability-bulge radius follows the three stores; with BulgeSizeOn 0 it
       * returns at once and every bulge radius keeps its value */
      mimic_sage_update_instability_bulge_radius(gal, unstable_stars, old_disk_radius, sizes, NULL);

      DEBUG_LOG("Halo %lld: Disk unstable - transferred %.3e to bulge", halo->HaloNr,
                unstable_stars);
    }

    /* SAGE parity: the gas branch needs unstable_gas > 0.0 and ColdGas > 0.0; its fraction
     * goes on to sage26_quasar_mode and sage26_starburst_feedback */
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
 * @brief SAGE26's check_disk_instability with BulgeSizeOn 0
 *
 * mimic_sage_check_disk_instability_with_sizes() with BulgeSizeOn 0, in which the radius
 * update returns at once.
 *
 * @param halo                     Halo holding the galaxy
 * @param ctx                      Module context with the gravitational constant G
 * @param star_forming_disk_factor SAGE26's TOOMRE_DISK_FACTOR, 3.0
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
