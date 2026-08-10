/**
 * @file    virial.c
 * @brief   Virial property helpers for halo mass, velocity, and radius
 *
 * Key functions:
 * - get_virial_mass(): Catalog or particle-count mass for one halo
 * - get_virial_velocity(): Circular velocity at the virial radius
 * - get_virial_radius(): 200c virial radius from Mvir and ρcrit(z)
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "config.h"
#include "globals.h"
#include "proto.h"
#include "numeric.h"
#include "generated/tree_property_accessors.h"

/*
 * New halo objects (descendants with no progenitor galaxies) are created by the
 * shared inheritance service (init_new_halo in src/core/inheritance.c), which
 * builds them from a driver-supplied HaloInitPayload rather than from tree
 * indices. The former init_halo() lived here and duplicated that logic plus the
 * UniqueGalaxyID encoding; it was removed when the inheritance service became
 * the single owner of object creation. The virial helpers below remain the
 * source of the descendant virial properties.
 */

/**
 * @brief   Returns the virial mass of a halo
 *
 * @param   view    Explicit view over the raw input halos being processed
 * @param   halonr  Index of the halo in the Halo array
 * @return  Virial mass in 10^10 Msun/h
 *
 * Returns the catalog HaloMass for FOF centrals when it is non-negative
 * (spherical-overdensity mass estimate from the halo finder). Falls back to
 * Len * PartMass for subhalos and centrals without a valid HaloMass entry.
 */
double get_virial_mass(struct HaloInputView view, int halonr) {
  const double halo_mass = mimic_tree_get_HaloMass(view, halonr);

  if (halonr == mimic_tree_get_FirstHaloInFOFgroup(view, halonr) && halo_mass >= 0.0)
    return halo_mass; /* take spherical overdensity mass estimate */
  else
    return mimic_tree_get_Len(view, halonr) * MimicConfig.PartMass;
}

/**
 * @brief   Calculates the virial velocity of a halo
 *
 * @param   view    Explicit view over the raw input halos being processed
 * @param   halonr  Index of the halo in the Halo array
 * @return  Virial velocity in km/s
 *
 * This function calculates the virial velocity of a halo based on its
 * virial mass and radius using the formula:
 * Vvir = sqrt(G * Mvir / Rvir)
 *
 * The virial velocity represents the circular velocity at the virial radius
 * and is an important parameter for many halo formation processes.
 *
 * Returns 0.0 if the virial radius is zero or negative.
 */
double get_virial_velocity(struct HaloInputView view, int halonr) {
  double Rvir;

  Rvir = get_virial_radius(view, halonr);

  if (Rvir > 0.0)
    return sqrt(MimicConfig.G * get_virial_mass(view, halonr) / Rvir);
  else
    return 0.0;
}

/**
 * @brief   Calculates the virial radius of a halo
 *
 * @param   view    Explicit view over the raw input halos being processed
 * @param   halonr  Index of the halo in the Halo array
 * @return  Virial radius in Mpc/h
 *
 * This function calculates the virial radius of a halo based on its
 * virial mass and the critical density of the universe at the halo's redshift.
 * The virial radius is defined as the radius within which the mean density
 * is 200 times the critical density.
 *
 * The calculation uses the formula:
 * Rvir = [3 * Mvir / (4 * π * 200 * ρcrit)]^(1/3)
 *
 * Where ρcrit is the critical density at the halo's redshift, calculated
 * using the cosmological parameters.
 *
 * Note: For certain simulations like Bolshoi, the Rvir property from the
 * halo catalog could be used directly instead of this calculation.
 */
double get_virial_radius(struct HaloInputView view, int halonr) {
  /* Rvir is recomputed from Mvir and the critical density rather than taken
   * from the catalog, so all simulations share one virial definition
   * (catalogs like Bolshoi provide Rvir directly, but with varying
   * conventions). */
  double zplus1, hubble_of_z_sq, rhocrit, fac;

  zplus1 = 1 + MimicConfig.ZZ[mimic_tree_get_SnapNum(view, halonr)];
  hubble_of_z_sq = MimicConfig.Hubble * MimicConfig.Hubble *
                   (MimicConfig.Omega * zplus1 * zplus1 * zplus1 +
                    (1 - MimicConfig.Omega - MimicConfig.OmegaLambda) * zplus1 * zplus1 +
                    MimicConfig.OmegaLambda);

  rhocrit = safe_div(3 * hubble_of_z_sq, 8 * M_PI * MimicConfig.G, 0.0);
  fac = safe_div(1.0, 200 * 4 * M_PI / 3.0 * rhocrit, 0.0);

  return cbrt(get_virial_mass(view, halonr) * fac);
}
