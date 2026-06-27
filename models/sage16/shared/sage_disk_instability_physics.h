/**
 * @file sage_disk_instability_physics.h
 * @brief Disk instability structural response kernel (Efstathiou 1982 criterion)
 *
 * Applies the disk-instability stability criterion: if the disk mass exceeds the
 * critical mass for rotational support, the excess is transferred to the bulge.
 * Returns the unstable gas fraction for use by sage_quasar_mode and
 * sage_starburst_feedback in the same substep.
 *
 * @note Used by sage_disk_instability; result consumed by sage_quasar_mode and
 *       sage_starburst_feedback via the UnstableDiskGasFraction transport property.
 */

#ifndef MIMIC_SHARED_SAGE_DISK_INSTABILITY_PHYSICS_H
#define MIMIC_SHARED_SAGE_DISK_INSTABILITY_PHYSICS_H

#include "error.h"
#include "module_interface.h"
#include "types.h"
#include "shared/metallicity.h"

/**
 * @brief Apply disk-instability structural response to a halo
 *
 * Computes the Efstathiou disk stability criterion and transfers unstable stellar
 * and gas mass to the bulge. Returns the unstable cold gas fraction for downstream
 * starburst and AGN modules.
 *
 * @param halo                    Halo containing the galaxy to update
 * @param ctx                     Module context with gravitational constant G
 * @param star_forming_disk_factor  Disk radius scale factor (StarFormingDiskFactor parameter)
 * @return Unstable cold gas fraction [0.0, 1.0]; 0.0 if stable or invalid input
 */
static inline double mimic_sage_apply_disk_instability(struct Halo *halo,
                                                       const struct ModuleContext *ctx,
                                                       double star_forming_disk_factor) {
  if (halo == NULL || halo->galaxy == NULL || ctx == NULL || ctx->params == NULL ||
      ctx->params->G <= 0.0) {
    return 0.0;
  }

  struct GalaxyData *gal = halo->galaxy;
  const double diskmass = gal->ColdGas + (gal->StellarMass - gal->BulgeMass);
  if (diskmass <= 0.0) {
    return 0.0;
  }

  double mcrit =
      halo->Vmax * halo->Vmax * (star_forming_disk_factor * gal->DiskScaleRadius) / ctx->params->G;
  if (mcrit > diskmass) {
    mcrit = diskmass;
  }

  const double gas_fraction = gal->ColdGas / diskmass;
  const double unstable_gas = gas_fraction * (diskmass - mcrit);
  const double star_fraction = 1.0 - gas_fraction;
  const double unstable_stars = star_fraction * (diskmass - mcrit);

  if (unstable_stars > 0.0) {
    const double disk_stellar_mass = gal->StellarMass - gal->BulgeMass;
    const double disk_metal_mass = gal->MetalsStellarMass - gal->MetalsBulgeMass;

    if (disk_stellar_mass <= 0.0) {
      return unstable_gas > 0.0 && gal->ColdGas > 0.0 ? unstable_gas / gal->ColdGas : 0.0;
    }

    const double metallicity = mimic_get_metallicity(disk_stellar_mass, disk_metal_mass);
    double transferred_stars = unstable_stars;
    if (transferred_stars > disk_stellar_mass) {
      transferred_stars = disk_stellar_mass;
    }

    double transferred_metals = metallicity * transferred_stars;
    if (disk_metal_mass <= 0.0) {
      transferred_metals = 0.0;
    } else if (transferred_metals > disk_metal_mass) {
      transferred_metals = disk_metal_mass;
    }

    gal->BulgeMass += transferred_stars;
    gal->MetalsBulgeMass += transferred_metals;

    if (gal->BulgeMass > gal->StellarMass) {
      const double excess = gal->BulgeMass - gal->StellarMass;
      const double tolerance = 1.0e-10 + 1.0e-4 * gal->StellarMass;
      if (excess > tolerance) {
        WARNING_LOG("Disk instability: Bulge mass %.4e exceeds stellar mass %.4e in halo %d",
                    gal->BulgeMass, gal->StellarMass, halo->HaloNr);
      }
      gal->BulgeMass = gal->StellarMass;
    }

    if (gal->MetalsBulgeMass > gal->MetalsStellarMass) {
      const double excess = gal->MetalsBulgeMass - gal->MetalsStellarMass;
      const double tolerance = 1.0e-10 + 1.0e-4 * gal->MetalsStellarMass;
      if (excess > tolerance) {
        WARNING_LOG("Disk instability: Bulge metals %.4e exceed stellar metals %.4e in halo %d",
                    gal->MetalsBulgeMass, gal->MetalsStellarMass, halo->HaloNr);
      }
      gal->MetalsBulgeMass = gal->MetalsStellarMass;
    }

    DEBUG_LOG("Halo %d: Disk unstable - transferred %.3e Msun to bulge", halo->HaloNr,
              transferred_stars);
  }

  if (unstable_gas > 0.0 && gal->ColdGas > 0.0) {
    const double unstable_gas_fraction = unstable_gas / gal->ColdGas;

    DEBUG_LOG("Halo %d: Unstable gas fraction = %.4f", halo->HaloNr, unstable_gas_fraction);
    return unstable_gas_fraction;
  }

  return 0.0;
}

#endif /* MIMIC_SHARED_SAGE_DISK_INSTABILITY_PHYSICS_H */
