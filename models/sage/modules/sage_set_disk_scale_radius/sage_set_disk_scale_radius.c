/**
 * @file    sage_set_disk_scale_radius.c
 * @brief   SAGE disk scale radius - sets disk scale radius from halo spin parameters
 *
 * Computes disk scale radius for the FOF central (Type 0) only, based on Mo, Mao
 * & White (1998). The disk radius is proportional to the spin parameter and
 * virial radius. SAGE parity: SAGE recomputes this only for the FOF central in
 * join_galaxies_of_progenitors; Type 1 satellites freeze the value they last had
 * as a central. Runs in pre_timestep phase before physics modules execute.
 *
 * Reference: Mo, Mao & White (1998), Bullock et al. (2001)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "globals.h"
#include "module_interface.h"
#include "types.h"

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Calculate spin parameter magnitude from 3D spin vector
 */
static inline float get_spin_magnitude(float spin_x, float spin_y, float spin_z) {
  return sqrtf(spin_x * spin_x + spin_y * spin_y + spin_z * spin_z);
}

/**
 * @brief Calculate Bullock-style spin parameter λ
 *
 * Computes the dimensionless spin parameter:
 *   λ = |J| / (√2 * Vvir * Rvir)
 */
static inline float get_spin_parameter(float spin_magnitude, float vvir, float rvir) {
  if (vvir <= EPSILON_SMALL || rvir <= EPSILON_SMALL) {
    return 0.0f;
  }

  return spin_magnitude / (1.414213562f * vvir * rvir);
}

/**
 * @brief Calculate galaxy disk scale radius
 *
 * Computes the disk scale radius based on Mo, Mao & White (1998) model:
 *   Rd = (λ / √2) * Rvir
 *
 * If virial properties are invalid, returns fallback of 0.1 * Rvir.
 */
static float calculate_disk_radius(float spin_x, float spin_y, float spin_z, float vvir,
                                   float rvir) {
  if (vvir > EPSILON_SMALL && rvir > EPSILON_SMALL) {
    const float spin_mag = get_spin_magnitude(spin_x, spin_y, spin_z);
    const float lambda = get_spin_parameter(spin_mag, vvir, rvir);

    // Mo, Mao & White (1998) eq. 12: Rd = (λ / √2) * Rvir
    return (lambda / 1.414213562f) * rvir;
  } else {
    // Fallback: use 10% of virial radius as rough estimate
    return 0.1f * rvir;
  }
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_set_disk_scale_radius_init(void) {
  INFO_LOG("SAGE set disk scale radius module initialized");
  VERBOSE_LOG("  Physics: DiskScaleRadius = (λ / √2) * Rvir (Mo98 model)");

  return 0;
}

int sage_set_disk_scale_radius_process(struct ModuleContext *ctx __attribute__((unused)),
                                       struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  for (int i = 0; i < ngal; i++) {
    // SAGE parity: the disk scale radius is recomputed ONLY for the FOF
    // central (Type 0), inside join_galaxies_of_progenitors. Type 1
    // satellites freeze the value they had as a central; recomputing it
    // here would change their star formation (reff -> tdyn -> cold_crit).
    if (halos[i].galaxy == NULL || halos[i].Type != 0) {
      continue;
    }
    // Calculate disk scale radius from halo spin and virial properties
    halos[i].galaxy->DiskScaleRadius = calculate_disk_radius(
        halos[i].Spin[0], halos[i].Spin[1], halos[i].Spin[2], halos[i].Vvir, halos[i].Rvir);

    DEBUG_LOG("Type=%d: DiskScaleRadius=%.3e, Rvir=%.3e, Vvir=%.3e", halos[i].Type,
              halos[i].galaxy->DiskScaleRadius, halos[i].Rvir, halos[i].Vvir);
  }

  return 0;
}

int sage_set_disk_scale_radius_cleanup(void) {
  VERBOSE_LOG("SAGE set disk scale radius module cleaned up");
  return 0;
}
