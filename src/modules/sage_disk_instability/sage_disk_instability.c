/**
 * @file    sage_disk_instability.c
 * @brief   SAGE disk instability module implementation
 *
 * This module implements disk instability detection and mass redistribution
 * from the SAGE model. Key processes:
 * - Disk stability criterion (Mo, Mao & White 1998)
 * - Direct stellar mass transfer from disk to bulge
 * - Metallicity preservation during mass transfers
 * - Disk scale radius calculation
 *
 * Physics Summary:
 *   diskmass = ColdGas + (StellarMass - BulgeMass)
 *   Mcrit = Vmax^2 * (3 * DiskScaleRadius) / G
 *   If diskmass > Mcrit: transfer (diskmass - Mcrit) to bulge
 *
 * Implementation Notes (v1.0.0):
 * - This is a PARTIAL IMPLEMENTATION providing core stability physics
 * - Stellar mass transfer to bulge is fully implemented
 * - Starburst triggering deferred (needs collisional_starburst_recipe from sage_mergers)
 * - Black hole growth deferred (needs grow_black_hole from sage_mergers)
 * - Unstable gas is tracked but not yet processed into stars
 *
 * When sage_mergers module is implemented, this module will be extended to:
 * 1. Call collisional_starburst_recipe() for unstable gas (mode=1)
 * 2. Call grow_black_hole() if AGN recipe is enabled
 * 3. Complete the full SAGE disk instability physics
 *
 * References:
 *   - SAGE: sage-code/model_disk_instability.c
 *   - Mo, Mao & White (1998) - Disk stability criterion
 *   - Efstathiou et al. (1982) - Disk formation in halos
 *
 * Vision Principles:
 *   - Physics-Agnostic Core: Interacts only through module interface
 *   - Runtime Modularity: Configurable via parameter file
 *   - Single Source of Truth: Updates GalaxyData properties only
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "constants.h"
#include "error.h"
#include "../shared/metallicity.h"  // Shared utility for metallicity calculations
#include "module_interface.h"
#include "module_registry.h"
#include "numeric.h"
#include "sage_disk_instability.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

/**
 * @brief   Enable disk instability physics
 *
 * If enabled (1), disk stability is checked and unstable mass is transferred
 * to the bulge. If disabled (0), module does nothing.
 *
 * Configuration: SageDiskInstability_DiskInstabilityOn
 */
static int DISK_INSTABILITY_ON = 1;

/**
 * @brief   Disk effective radius factor
 *
 * Factor multiplied by DiskScaleRadius to get effective disk radius.
 * Typically 3.0 (approximately 3 scale lengths encloses most disk mass).
 *
 * Configuration: SageDiskInstability_DiskRadiusFactor
 */
static double DISK_RADIUS_FACTOR = 3.0;

// ============================================================================
// HELPER FUNCTIONS (Physics Calculations)
// ============================================================================

// Metallicity calculation provided by shared utility: mimic_get_metallicity()
// See: src/modules/shared/metallicity.h

/**
 * @brief   Calculate disk scale radius using Mo, Mao & White (1998) formula
 *
 * Calculates the exponential disk scale radius based on halo spin parameter
 * and virial radius. This follows the Mo, Mao & White (1998) disk formation
 * model where disk size is determined by angular momentum conservation.
 *
 * For now, we use a simple empirical scaling with Rvir.
 * Future versions may implement full spin-dependent calculation.
 *
 * @param   rvir   Virial radius (Mpc/h)
 * @return  Disk scale radius (Mpc/h)
 */
static double calculate_disk_scale_radius(float rvir) {
  /* Simple empirical scaling: disk scale radius ~ 0.03 * Rvir
   * Typical values: Rvir ~ 0.1-1 Mpc/h → Rd ~ 3-30 kpc/h = 0.003-0.03 Mpc/h
   *
   * Note: Full Mo, Mao & White (1998) formula requires spin parameter:
   *   Rd = (lambda / sqrt(2)) * (j_d / m_d) * Rvir
   * where lambda is spin parameter, j_d is specific angular momentum,
   * and m_d is disk mass fraction.
   *
   * This will be implemented when spin is available from halo properties.
   */
  const double DISK_FRACTION = 0.03; /* Empirical calibration */
  return DISK_FRACTION * rvir;
}

/**
 * @brief   Calculate critical disk mass for stability
 *
 * Implements the Mo, Mao & White (1998) stability criterion:
 *   Mcrit = Vmax^2 * Reff / G
 * where Reff = DISK_RADIUS_FACTOR * DiskScaleRadius
 *
 * Disks with mass exceeding Mcrit are gravitationally unstable and
 * will transfer excess mass to the bulge.
 *
 * @param   vmax              Maximum circular velocity (km/s)
 * @param   disk_scale_radius Disk scale radius (Mpc/h)
 * @param   G_code            Gravitational constant in code units
 * @return  Critical disk mass (1e10 Msun/h)
 */
static double calculate_critical_disk_mass(float vmax, float disk_scale_radius,
                                           double G_code) {
  /* Calculate effective disk radius */
  double reff = DISK_RADIUS_FACTOR * disk_scale_radius;

  /* Stability criterion: Mcrit = Vmax^2 * Reff / G */
  double mcrit = vmax * vmax * reff / G_code;

  return mcrit;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

/**
 * @brief   Initialize the disk instability module
 *
 * Called once at program startup to:
 * 1. Read module parameters from configuration
 * 2. Initialize any module-specific data structures
 * 3. Validate parameter values
 *
 * @return  0 on success, non-zero on error
 */
static int sage_disk_instability_init(void) {
  /* Read module parameters from configuration */
  module_get_int("SageDiskInstability", "DiskInstabilityOn", &DISK_INSTABILITY_ON, 1);
  module_get_double("SageDiskInstability", "DiskRadiusFactor", &DISK_RADIUS_FACTOR, 3.0);

  /* Validate parameters */
  if (DISK_RADIUS_FACTOR < 1.0 || DISK_RADIUS_FACTOR > 10.0) {
    ERROR_LOG("SageDiskInstability_DiskRadiusFactor = %.2f is outside valid range [1.0, 10.0]",
              DISK_RADIUS_FACTOR);
    return -1;
  }

  /* Log initialization */
  if (DISK_INSTABILITY_ON) {
    INFO_LOG("SAGE Disk Instability module initialized (v1.0.0 - PARTIAL IMPLEMENTATION)");
    INFO_LOG("  Physics: Mcrit = Vmax^2 * (3 * Rd) / G, transfer excess to bulge");
    INFO_LOG("  DiskRadiusFactor = %.2f (from config)", DISK_RADIUS_FACTOR);
    INFO_LOG("  Note: Starburst and AGN components deferred pending sage_mergers module");
  } else {
    INFO_LOG("SAGE Disk Instability module initialized but DISABLED");
  }

  return 0;
}

/**
 * @brief   Clean up the disk instability module
 *
 * Called once at program shutdown to release any module-specific resources.
 *
 * @return  0 on success, non-zero on error
 */
static int sage_disk_instability_cleanup(void) {
  INFO_LOG("SAGE Disk Instability module cleaned up");
  return 0;
}

/**
 * @brief   Process disk instability for all halos in FOF group
 *
 * This is the main physics function called for each FOF group to:
 * 1. Calculate disk scale radius if not yet set
 * 2. Evaluate disk stability criterion
 * 3. Transfer unstable stellar mass to bulge
 * 4. Track unstable gas (processing deferred to sage_mergers)
 *
 * Implementation Status (v1.0.0):
 * - ✓ Disk scale radius calculation
 * - ✓ Stability criterion (Mo, Mao & White 1998)
 * - ✓ Stellar mass transfer with metallicity preservation
 * - ⏸ Gas starburst (needs collisional_starburst_recipe from sage_mergers)
 * - ⏸ BH growth (needs grow_black_hole from sage_mergers)
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos in FOF group
 * @param   ngal    Number of halos
 * @return  0 on success, non-zero on error
 */
static int sage_disk_instability_process(struct ModuleContext *ctx,
                                         struct Halo *halos,
                                         int ngal) {
  /* Skip if module is disabled */
  if (!DISK_INSTABILITY_ON) {
    return 0;
  }

  /* Get gravitational constant in code units from context */
  double G_code = ctx->params->G;

  /* Process each galaxy in FOF group */
  for (int i = 0; i < ngal; i++) {
    struct Halo *halo = &halos[i];
    struct GalaxyData *galaxy = halo->galaxy;

    /* Skip if galaxy data is NULL */
    if (galaxy == NULL) {
      continue;
    }

    /* Calculate disk scale radius if not yet initialized
     * (only needs to be done once per galaxy) */
    if (galaxy->DiskScaleRadius <= 0.0) {
      galaxy->DiskScaleRadius = calculate_disk_scale_radius(halo->Rvir);
    }

    /* Calculate total disk mass (cold gas + stellar disk)
     * Stellar disk = StellarMass - BulgeMass */
    double disk_stellar_mass = galaxy->StellarMass - galaxy->BulgeMass;
    double disk_mass = galaxy->ColdGas + disk_stellar_mass;

    /* Only proceed if disk has positive mass */
    if (disk_mass <= 0.0) {
      continue;
    }

    /* Calculate critical disk mass for stability */
    double mcrit = calculate_critical_disk_mass(halo->Vmax, galaxy->DiskScaleRadius,
                                                G_code);

    /* Limit critical mass to actual disk mass (can't have negative unstable mass) */
    if (mcrit > disk_mass) {
      mcrit = disk_mass;
    }

    /* Calculate mass fractions in disk */
    double gas_fraction = galaxy->ColdGas / disk_mass;
    double star_fraction = disk_stellar_mass / disk_mass;

    /* Calculate unstable masses that exceed stability criterion */
    double unstable_gas = gas_fraction * (disk_mass - mcrit);
    double unstable_stars = star_fraction * (disk_mass - mcrit);

    /* ========================================================================
     * HANDLE UNSTABLE STARS - FULLY IMPLEMENTED
     * Transfer stellar mass directly to bulge with metallicity preservation
     * ======================================================================== */
    if (unstable_stars > 0.0) {
      /* Calculate disk stellar metallicity (excluding existing bulge) */
      double disk_metal_mass = galaxy->MetalsStellarMass - galaxy->MetalsBulgeMass;
      double metallicity = mimic_get_metallicity(disk_stellar_mass, disk_metal_mass);

      /* Transfer unstable stars to bulge */
      galaxy->BulgeMass += unstable_stars;
      galaxy->MetalsBulgeMass += metallicity * unstable_stars;

      /* Sanity check: bulge mass should not exceed total stellar mass */
      if (galaxy->BulgeMass > galaxy->StellarMass * 1.0001 ||
          galaxy->MetalsBulgeMass > galaxy->MetalsStellarMass * 1.0001) {
        WARNING_LOG("Disk instability: bulge mass exceeds total stellar mass in halo %d. "
                   "Bulge/Total = %.4f (stars) or %.4f (metals)",
                   halo->HaloNr,
                   galaxy->BulgeMass / galaxy->StellarMass,
                   galaxy->MetalsBulgeMass / galaxy->MetalsStellarMass);
      }
    }

    /* ========================================================================
     * HANDLE UNSTABLE GAS - DEFERRED TO FUTURE IMPLEMENTATION
     *
     * SAGE Physics (to be implemented when sage_mergers exists):
     *
     * 1. If AGNrecipeOn: grow_black_hole(p, unstable_gas_fraction)
     *    - Accretes gas onto black hole
     *    - Triggers quasar-mode feedback
     *
     * 2. collisional_starburst_recipe(unstable_gas_fraction, p, centralgal,
     *                                  time, dt, halonr, mode=1, step)
     *    - Mode 1 indicates disk instability-induced starburst
     *    - Converts unstable gas to stars in bulge
     *    - Triggers supernova feedback
     *    - Handles metal enrichment
     *
     * For now, unstable gas remains in cold gas reservoir until sage_mergers
     * module provides the necessary starburst infrastructure.
     * ======================================================================== */
    if (unstable_gas > 0.0) {
      /* Gas instability detected but processing deferred
       *
       * TODO (when sage_mergers is implemented):
       * 1. Calculate unstable_gas_fraction = unstable_gas / ColdGas
       * 2. If AGN enabled: call grow_black_hole(p, unstable_gas_fraction)
       * 3. Call collisional_starburst_recipe(..., mode=1, ...)
       *
       * See SAGE sage-code/model_disk_instability.c:120-143 for reference
       */
    }
  }

  return 0;
}

// ============================================================================
// MODULE REGISTRATION
// ============================================================================

/**
 * @brief   Module structure for sage_disk_instability module
 */
static struct Module sage_disk_instability_module = {
    .name = "sage_disk_instability",
    .init = sage_disk_instability_init,
    .process_halos = sage_disk_instability_process,
    .cleanup = sage_disk_instability_cleanup};

/**
 * @brief   Register the sage_disk_instability module
 */
void sage_disk_instability_register(void) {
  module_registry_add(&sage_disk_instability_module);
}
