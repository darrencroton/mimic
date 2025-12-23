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

/**
 * @brief   Initializes a new halo tracking object with default properties
 *
 * @param   p       Index in the FoFWorkspace array
 * @param   halonr  Index of the halo in the InputTreeHalos array
 * @param   tree    Merger tree number within file
 * @param   filenr  File number in multi-file run
 *
 * This function initializes a new halo tracking object with default values.
 * It sets up the object's position, velocity, and halo properties based on
 * the input merger tree halo data. Each object is assigned a persistent
 * unique UniqueGalaxyID for identification and tracking through cosmic time.
 *
 * UniqueGalaxyID encoding: file*10^15 + tree*10^9 + halonr
 * This value persists for the galaxy's entire lifetime and is inherited
 * by descendants through progenitor relationships.
 */
void init_halo(int p, int halonr, int tree, int filenr) {
  /* Verify this is a central halo (FOF group center) */
  assert(halonr == InputTreeHalos[halonr].FirstHaloInFOFgroup);

  /* Custom initialization: HaloNr is set directly from parameter */
  FoFWorkspace[p].HaloNr = halonr;

  /* AUTO-GENERATED: Initialize all halo properties from metadata */
  #include "../include/generated/init_halo_properties.inc"

  /* Custom override: SnapNum needs -1 adjustment for internal indexing */
  FoFWorkspace[p].SnapNum = InputTreeHalos[halonr].SnapNum - 1;

  /* Set dT to snapshot interval for newly formed halos (grew from zero mass) */
  int current_snap = InputTreeHalos[halonr].SnapNum;
  if (current_snap > 0) {
    FoFWorkspace[p].dT = Age[current_snap - 1] - Age[current_snap];
  }
  /* else: keep sentinel -1.0 from auto-init (first snapshot has no prior) */

  /* Assign persistent UniqueGalaxyID at creation (AFTER auto-init to avoid overwrite) */
  long long file_mul_fac = (MimicConfig.LastFile >= 10000) ?
                            (FILENR_MUL_FAC / 10) : FILENR_MUL_FAC;
  long long tree_mul = TREE_MUL_FAC * tree;
  long long file_mul = file_mul_fac * filenr;

  FoFWorkspace[p].UniqueGalaxyID = (long long)halonr + tree_mul + file_mul;

  /* Allocate galaxy data (physics-agnostic core always allocates)
   * Modules will populate these properties; if no modules run, values stay at zero */
  FoFWorkspace[p].galaxy = mymalloc_cat(sizeof(struct GalaxyData), MEM_HALOS);

  /* AUTO-GENERATED: Initialize all galaxy properties from metadata */
  #include "../include/generated/init_galaxy_properties.inc"
}

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
 * For central halos (FirstHaloInFOFgroup), it uses the Mvir property if
 * positive (> 0.0). If Mvir=0 but Len>0 (edge case: unresolved or artifact),
 * it falls back to particle-based mass calculation.
 * Otherwise, it calculates mass as number of particles × particle mass.
 */
double get_virial_mass(int halonr) {
  if (halonr == InputTreeHalos[halonr].FirstHaloInFOFgroup &&
      InputTreeHalos[halonr].Mvir > 0.0)
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
