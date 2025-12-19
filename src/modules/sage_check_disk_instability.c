/**
 * @file    sage_check_disk_instability.c
 * @brief   Evaluate disk stability and calculate unstable fractions
 *
 * This module implements the Mo, Mao & White (1998) disk stability criterion
 * to identify gravitationally unstable disks. It calculates the fraction of
 * disk stars and cold gas that exceed the stability threshold.
 *
 * Physics Summary:
 *   diskmass = ColdGas + (StellarMass - BulgeMass)
 *   Mcrit = Vmax^2 * (3 * DiskScaleRadius) / G
 *   If diskmass > Mcrit:
 *     UnstableDiskGasFraction = (ColdGas / diskmass) * (diskmass - Mcrit) / ColdGas
 *     UnstableDiskStarFraction = (disk_stars / diskmass) * (diskmass - Mcrit) / disk_stars
 *
 * Module Communication:
 *   Reads:  Rvir, Vmax, ColdGas, StellarMass, BulgeMass, DiskScaleRadius
 *   Writes: DiskScaleRadius (if not set), UnstableDiskGasFraction, UnstableDiskStarFraction
 *
 * References:
 *   - SAGE: sage-code/model_disk_instability.c (lines 20-32)
 *   - Mo, Mao & White (1998) - Disk stability criterion
 *   - Efstathiou et al. (1982) - Disk formation in halos
 *
 * Vision Principles:
 *   - Physics-Agnostic Core: Interacts only through module interface
 *   - Runtime Modularity: Configurable via parameter file
 *   - Single Responsibility: Only evaluates stability, doesn't modify masses
 *   - Module Independence: Downstream modules check UnstableDisk*Fraction properties
 */

#include <math.h>
#include <stdio.h>

#include "constants.h"
#include "error.h"
#include "../modules/_system/parameter_helpers.h"
#include "module_interface.h"
#include "numeric.h"
#include "types.h"

/* ============================================================================
 * MODULE PARAMETERS
 * ============================================================================ */

static int DISK_INSTABILITY_ON;
static double STAR_FORMING_DISK_FACTOR;

/* ============================================================================
 * PHYSICS CONSTANTS
 * ============================================================================ */

/* Disk structure (Mo, Mao & White 1998 empirical scaling) */
static const double DISK_FRACTION = 0.03;  /* R_disk ~ 3% of R_vir (typical 3-30 kpc/h) */

/* ============================================================================
 * HELPER FUNCTIONS
 * ============================================================================ */

/**
 * @brief   Calculate disk scale radius using empirical scaling
 *
 * Simple empirical scaling: disk scale radius ~ 0.03 * Rvir
 * Typical values: Rvir ~ 0.1-1 Mpc/h → Rd ~ 3-30 kpc/h
 *
 * @param   rvir   Virial radius (Mpc/h)
 * @return  Disk scale radius (Mpc/h)
 */
static double calculate_disk_scale_radius(float rvir)
{
    return DISK_FRACTION * rvir;
}

/**
 * @brief   Calculate critical disk mass for stability
 *
 * Implements the Mo, Mao & White (1998) stability criterion:
 *   Mcrit = Vmax^2 * Reff / G
 * where Reff = STAR_FORMING_DISK_FACTOR * DiskScaleRadius
 *
 * @param   vmax              Maximum circular velocity (km/s)
 * @param   disk_scale_radius Disk scale radius (Mpc/h)
 * @param   G_code            Gravitational constant in code units
 * @return  Critical disk mass (1e10 Msun/h)
 */
static double calculate_critical_disk_mass(float vmax, float disk_scale_radius, double G_code)
{
    double reff = STAR_FORMING_DISK_FACTOR * disk_scale_radius;
    double mcrit = safe_div(vmax * vmax * reff, G_code, 0.0);
    return mcrit;
}

/* ============================================================================
 * MODULE LIFECYCLE FUNCTIONS
 * ============================================================================ */

/**
 * @brief   Initialize the disk instability check module
 */
int sage_check_disk_instability_init(void)
{
    LOAD_AND_VALIDATE_OPTION("DiskInstabilityOn", DISK_INSTABILITY_ON, 1,
                              "0=disabled, 1=enabled");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR,
                                       0.0, 10.0, "star forming disk factor");

    if (DISK_INSTABILITY_ON) {
        VERBOSE_LOG("SAGE Check Disk Instability initialized");
        VERBOSE_LOG("  StarFormingDiskFactor = %.2f", STAR_FORMING_DISK_FACTOR);
    } else {
        VERBOSE_LOG("SAGE Check Disk Instability initialized but DISABLED");
    }

    return 0;
}

/**
 * @brief   Clean up the disk instability check module
 */
int sage_check_disk_instability_cleanup(void)
{
    return 0;
}

/**
 * @brief   Evaluate disk stability and calculate unstable fractions
 *
 * This function:
 * 1. Initializes DiskScaleRadius if not yet set (once per galaxy)
 * 2. Calculates total disk mass and critical mass
 * 3. Computes unstable fractions for stars and gas
 * 4. Sets UnstableDiskStarFraction and UnstableDiskGasFraction properties
 *
 * Downstream modules check these fractions to trigger physics:
 *   - sage_transfer_disk_to_bulge: Checks UnstableDiskStarFraction
 *   - sage_grow_black_hole: Checks UnstableDiskGasFraction
 *   - sage_trigger_starburst: Checks UnstableDiskGasFraction
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos (ngal=1 for process_by_galaxy)
 * @param   ngal    Number of halos (should be 1)
 * @return  0 on success, non-zero on error
 */
int sage_check_disk_instability_process(struct ModuleContext *ctx,
                                        struct Halo *halos,
                                        int ngal)
{
    /* Skip if module is disabled */
    if (!DISK_INSTABILITY_ON) {
        return 0;
    }

    /* Verify process_by_galaxy mode (ngal should be 1) */
    if (ngal != 1) {
        ERROR_LOG("sage_check_disk_instability expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    if (gal == NULL) {
        return 0;
    }

    /* Initialize disk scale radius if not yet set (once per galaxy)
     * This ensures DiskScaleRadius is available for stability calculation */
    if (gal->DiskScaleRadius <= 0.0) {
        gal->DiskScaleRadius = calculate_disk_scale_radius(halo->Rvir);
    }

    /* Calculate total disk mass (cold gas + stellar disk)
     * Stellar disk = StellarMass - BulgeMass */
    double disk_stellar_mass = gal->StellarMass - gal->BulgeMass;
    double disk_mass = gal->ColdGas + disk_stellar_mass;

    /* If disk mass is zero or negative, disk is stable (no instability) */
    if (disk_mass <= 0.0) {
        gal->UnstableDiskGasFraction = 0.0;
        gal->UnstableDiskStarFraction = 0.0;
        return 0;
    }

    /* Calculate critical disk mass for stability (Mo, Mao & White 1998) */
    double mcrit = calculate_critical_disk_mass(halo->Vmax, gal->DiskScaleRadius,
                                                 ctx->params->G);

    /* Limit critical mass to actual disk mass (disk is stable if mcrit > disk_mass) */
    if (mcrit > disk_mass) {
        mcrit = disk_mass;
    }

    /* Calculate unstable total mass */
    double unstable_total = disk_mass - mcrit;

    /* If unstable mass is zero or negative, disk is stable */
    if (unstable_total <= 0.0) {
        gal->UnstableDiskGasFraction = 0.0;
        gal->UnstableDiskStarFraction = 0.0;
        return 0;
    }

    /* Calculate unstable fractions (SAGE lines 29-32)
     * These fractions represent what proportion of each component is unstable
     *
     * SAGE calculation:
     *   gas_fraction = ColdGas / disk_mass
     *   unstable_gas = gas_fraction * (disk_mass - mcrit)
     *   UnstableDiskGasFraction = unstable_gas / ColdGas
     *                            = (ColdGas / disk_mass) * (disk_mass - mcrit) / ColdGas
     *
     * Similarly for stars:
     *   star_fraction = disk_stellar_mass / disk_mass
     *   unstable_stars = star_fraction * (disk_mass - mcrit)
     *   UnstableDiskStarFraction = unstable_stars / disk_stellar_mass
     */
    gal->UnstableDiskGasFraction = safe_div(gal->ColdGas, disk_mass, 0.0) *
                                   safe_div(unstable_total, gal->ColdGas, 0.0);

    gal->UnstableDiskStarFraction = safe_div(disk_stellar_mass, disk_mass, 0.0) *
                                    safe_div(unstable_total, disk_stellar_mass, 0.0);

    DEBUG_LOG("Halo %d: Disk unstable - gas_frac=%.4f, star_frac=%.4f",
              halo->HaloNr, gal->UnstableDiskGasFraction, gal->UnstableDiskStarFraction);

    return 0;
}
