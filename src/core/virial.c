/**
 * @file    model_misc.c
 * @brief   Miscellaneous utility functions for halo tracking
 *
 * This file contains various utility functions used throughout the Mimic code
 * for halo initialization, property calculation, and basic operations.
 * It includes functions for calculating halo properties (mass, velocity,
 * radius) and initializing halo tracking structures.
 *
 * Key functions:
 * - init_halo): Initializes a new halo tracking object
 * - get_virial_mass/velocity/radius(): Calculate halo virial properties
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "allvars.h"
#include "proto.h"
#include "numeric.h"

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
 * @param   halonr  Index of the halo in the Halo array
 * @return  Virial mass in 10^10 Msun/h
 *
 * This function returns the virial mass of a halo, using the spherical
 * overdensity mass if available for central halos. For satellite subhalos
 * or when spherical overdensity mass is not available, it returns the mass
 * estimated from particle counts.
 *
 * For central halos (FirstHaloInFOFgroup), it uses the catalog Mvir when
 * non-negative (>= 0.0). Otherwise, it calculates mass as number of 
 * particles × particle mass.
 */
double get_virial_mass(int halonr) {
  if (halonr == InputTreeHalos[halonr].FirstHaloInFOFgroup &&
      InputTreeHalos[halonr].Mvir >= 0.0)
    return InputTreeHalos[halonr]
        .Mvir; /* take spherical overdensity mass estimate */
  else
    return InputTreeHalos[halonr].Len * MimicConfig.PartMass;
}

/**
 * @brief   Calculates the virial velocity of a halo
 *
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
double get_virial_velocity(int halonr) {
  double Rvir;

  Rvir = get_virial_radius(halonr);

  if (Rvir > 0.0)
    return sqrt(MimicConfig.G * get_virial_mass(halonr) / Rvir);
  else
    return 0.0;
}

/**
 * @brief   Calculates the virial radius of a halo
 *
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
double get_virial_radius(int halonr) {
  // return InputTreeHalos[halonr].Rvir;  // Used for Bolshoi

  double zplus1, hubble_of_z_sq, rhocrit, fac;

  zplus1 = 1 + MimicConfig.ZZ[InputTreeHalos[halonr].SnapNum];
  hubble_of_z_sq =
      MimicConfig.Hubble * MimicConfig.Hubble *
      (MimicConfig.Omega * zplus1 * zplus1 * zplus1 +
       (1 - MimicConfig.Omega - MimicConfig.OmegaLambda) * zplus1 * zplus1 +
       MimicConfig.OmegaLambda);

  rhocrit = safe_div(3 * hubble_of_z_sq, 8 * M_PI * MimicConfig.G, 0.0);
  fac = safe_div(1.0, 200 * 4 * M_PI / 3.0 * rhocrit, 0.0);

  return cbrt(get_virial_mass(halonr) * fac);
}
