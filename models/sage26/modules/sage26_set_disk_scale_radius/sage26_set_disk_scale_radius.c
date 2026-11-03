/**
 * @file    sage26_set_disk_scale_radius.c
 * @brief   SAGE26 disk scale radius - sets disk scale radius from halo spin parameters
 *
 * Ports SAGE26's get_disk_radius (model_misc.c:203-228), the Mo, Mao & White (1998)
 * disk radius from the Bullock spin parameter, and its float store into
 * DiskScaleRadius (core_build_model.c:297). Computed for the FOF central (Type 0)
 * only. SAGE parity: SAGE26 recomputes this only for the FOF central in
 * join_galaxies_of_progenitors (core_build_model.c:284-320); Type 1 satellites
 * freeze the value they last had as a central. Runs in the pre_timestep phase
 * before the physics modules execute.
 *
 * Every floating-point expression is transcribed from SAGE26 with its operand
 * order and types, so that compiler contraction treats both alike.
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
// CONSTANTS
// ============================================================================

/* SAGE26 model_misc.c:40-42, same names and values.
 * SAGE parity: SQRT_REPLACEMENT is the truncated literal 1.414 SAGE26 uses in
 * place of sqrt(2) (model_misc.c:42, :207-210); replacing it shifts every disk
 * radius. DISK_CONCENTRATION_FACTOR is f_R frozen at 1.0 (model_misc.c:219-226). */
static const double DISK_RADIUS_FALLBACK_FRAC = 0.1;
static const double DISK_CONCENTRATION_FACTOR = 1.0;
static const double SQRT_REPLACEMENT = 1.414;

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief SAGE26 get_disk_radius (model_misc.c:203-228)
 *
 * @param Spin  The halo's specific angular momentum vector (float, as SAGE26's
 *              halo_data holds it)
 * @param Vvir  The galaxy's virial velocity as SAGE26 holds it, a float
 * @param Rvir  The galaxy's virial radius as SAGE26 holds it, a float
 * @return      The disk scale radius (double; the caller stores it as float)
 */
static double get_disk_radius(const float Spin[3], const float Vvir, const float Rvir) {
  double r_disk;
  if (Vvir > 0.0 && Rvir > 0.0) {
    /* SAGE parity: the spin-component products and their sum are float
     * arithmetic, as SAGE26's float Spin makes them; only the sum is widened to
     * double for sqrt (model_misc.c:212-213). The spin parameter's denominator
     * 1.414 * Vvir * Rvir is double, the float fields widened. */
    double SpinMagnitude = sqrt(Spin[0] * Spin[0] + Spin[1] * Spin[1] + Spin[2] * Spin[2]);

    double SpinParameter = SpinMagnitude / (SQRT_REPLACEMENT * Vvir * Rvir);
    r_disk = (SpinParameter / SQRT_REPLACEMENT) * Rvir;
    return r_disk * DISK_CONCENTRATION_FACTOR;
  } else {
    // SAGE26's fallback is the double product 0.1 * Rvir (model_misc.c:226-227)
    return DISK_RADIUS_FALLBACK_FRAC * Rvir;
  }
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage26_set_disk_scale_radius_init(void) {
  VERBOSE_LOG("SAGE set disk scale radius module initialized");
  VERBOSE_LOG("  Physics: DiskScaleRadius = (λ / √2) * Rvir (Mo98 model)");

  return 0;
}

int sage26_set_disk_scale_radius_process(struct ModuleContext *ctx __attribute__((unused)),
                                         struct Halo *halos, int ngal) {
  if (halos == NULL || ngal <= 0) {
    return 0;
  }

  for (int i = 0; i < ngal; i++) {
    // SAGE parity: the disk scale radius is recomputed ONLY for the FOF
    // central (Type 0), inside join_galaxies_of_progenitors
    // (core_build_model.c:284-298). Type 1 satellites freeze the value they had
    // as a central; recomputing it here would change their star formation
    // (reff -> tdyn -> cold_crit).
    if (halos[i].galaxy == NULL || halos[i].Type != 0) {
      continue;
    }

    // Vvir and Rvir are read as the floats SAGE26 holds (core_allvars.h:131-132);
    // the double result is stored in the float field (core_build_model.c:297)
    halos[i].galaxy->DiskScaleRadius =
        get_disk_radius(halos[i].Spin, (float)halos[i].Vvir, (float)halos[i].Rvir);

    DEBUG_LOG("Type=%d: DiskScaleRadius=%.3e, Rvir=%.3e, Vvir=%.3e", halos[i].Type,
              halos[i].galaxy->DiskScaleRadius, halos[i].Rvir, halos[i].Vvir);
  }

  return 0;
}

int sage26_set_disk_scale_radius_cleanup(void) {
  VERBOSE_LOG("SAGE set disk scale radius module cleaned up");
  return 0;
}
