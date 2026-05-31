#ifndef MIMIC_SHARED_SAGE_DISK_INSTABILITY_PHYSICS_H
#define MIMIC_SHARED_SAGE_DISK_INSTABILITY_PHYSICS_H

#include "error.h"
#include "module_interface.h"
#include "types.h"
#include "sage/shared/metallicity.h"

/*
 * Apply SAGE's disk-instability structural response to a live remnant and
 * return the unstable gas fraction for any same-context downstream physics.
 */
static inline double mimic_sage_apply_disk_instability(
    struct Halo *halo, const struct ModuleContext *ctx,
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

  double mcrit = halo->Vmax * halo->Vmax *
                 (star_forming_disk_factor * gal->DiskScaleRadius) /
                 ctx->params->G;
  if (mcrit > diskmass) {
    mcrit = diskmass;
  }

  const double gas_fraction = gal->ColdGas / diskmass;
  const double unstable_gas = gas_fraction * (diskmass - mcrit);
  const double star_fraction = 1.0 - gas_fraction;
  const double unstable_stars = star_fraction * (diskmass - mcrit);

  if (unstable_stars > 0.0) {
    const double disk_stellar_mass = gal->StellarMass - gal->BulgeMass;
    const double disk_metal_mass =
        gal->MetalsStellarMass - gal->MetalsBulgeMass;
    const double metallicity =
        mimic_get_metallicity(disk_stellar_mass, disk_metal_mass);

    gal->BulgeMass += unstable_stars;
    gal->MetalsBulgeMass += metallicity * unstable_stars;

    if (gal->BulgeMass > gal->StellarMass) {
      WARNING_LOG("Disk instability: Bulge mass %.4e exceeds stellar mass %.4e in halo %d",
                  gal->BulgeMass, gal->StellarMass, halo->HaloNr);
      gal->BulgeMass = gal->StellarMass;
    }

    if (gal->MetalsBulgeMass > gal->MetalsStellarMass) {
      WARNING_LOG("Disk instability: Bulge metals %.4e exceed stellar metals %.4e in halo %d",
                  gal->MetalsBulgeMass, gal->MetalsStellarMass, halo->HaloNr);
      gal->MetalsBulgeMass = gal->MetalsStellarMass;
    }

    DEBUG_LOG("Halo %d: Disk unstable - transferred %.3e Msun to bulge",
              halo->HaloNr, unstable_stars);
  }

  if (unstable_gas > 0.0 && gal->ColdGas > 0.0) {
    const double unstable_gas_fraction = unstable_gas / gal->ColdGas;

    DEBUG_LOG("Halo %d: Unstable gas fraction = %.4f",
              halo->HaloNr, unstable_gas_fraction);
    return unstable_gas_fraction;
  }

  return 0.0;
}

#endif /* MIMIC_SHARED_SAGE_DISK_INSTABILITY_PHYSICS_H */
