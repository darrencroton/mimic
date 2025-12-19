/**
 * @file    sage_disk_instability.c
 * @brief   SAGE disk instability module - stability check and mass transfer
 *
 * Implements the Mo, Mao & White (1998) disk stability criterion to identify
 * gravitationally unstable disks and transfers unstable stellar mass from disk
 * to bulge. This module combines stability evaluation and morphological
 * transformation into a single coherent process.
 *
 * Physics Summary:
 *   Step 1 - Evaluate Stability:
 *     disk_mass = ColdGas + (StellarMass - BulgeMass)
 *     Mcrit = Vmax^2 * (3 * DiskScaleRadius) / G
 *     If disk_mass > Mcrit:
 *       UnstableDiskGasFraction = (ColdGas / disk_mass) * (disk_mass - Mcrit) / ColdGas
 *       UnstableDiskStarFraction = (disk_stars / disk_mass) * (disk_mass - Mcrit) / disk_stars
 *
 *   Step 2 - Transfer Unstable Stars:
 *     unstable_stars = UnstableDiskStarFraction * disk_stars
 *     BulgeMass += unstable_stars (with metallicity preservation)
 *
 * Module Communication:
 *   Reads:  Rvir, Vmax, ColdGas, StellarMass, BulgeMass, DiskScaleRadius,
 *           MetalsStellarMass, MetalsBulgeMass
 *   Writes: DiskScaleRadius (if not set), UnstableDiskGasFraction,
 *           UnstableDiskStarFraction, BulgeMass, MetalsBulgeMass
 *
 * Downstream Modules:
 *   - sage_quasar_mode: Checks UnstableDiskGasFraction for BH growth trigger
 *   - sage_collisional_starburst: Checks UnstableDiskGasFraction for starburst trigger
 *
 * References:
 *   - SAGE: sage-code/model_disk_instability.c (lines 20-54)
 *   - Mo, Mao & White (1998) - Disk stability criterion
 *   - Efstathiou et al. (1982) - Disk formation in halos
 *
 * Vision Principles:
 *   - Physics-Agnostic Core: Interacts only through module interface
 *   - Runtime Modularity: Configurable via parameter file
 *   - Single Responsibility: Evaluates stability and transfers mass, nothing else
 *   - KISS: Simple physics, clear implementation
 *
 * Reference: Croton et al. (2006, 2016)
 */

#include <math.h>

#include "constants.h"
#include "error.h"
#include "_shared/metallicity.h"
#include "_system/parameter_helpers.h"
#include "module_interface.h"
#include "numeric.h"
#include "types.h"

// ============================================================================
// MODULE PARAMETERS
// ============================================================================

static int DISK_INSTABILITY_ON;
static double STAR_FORMING_DISK_FACTOR;

// ============================================================================
// PHYSICS CONSTANTS
// ============================================================================

/* Disk structure (Mo, Mao & White 1998 empirical scaling) */
static const double DISK_FRACTION = 0.03;  /* R_disk ~ 3% of R_vir (typical 3-30 kpc/h) */

/* Numerical tolerance for mass conservation checks */
static const double MASS_TOLERANCE_FACTOR = 1.0001;  /* 0.01% rounding allowance */

// ============================================================================
// HELPER FUNCTIONS
// ============================================================================

/**
 * @brief Calculate disk scale radius using empirical scaling
 *
 * Simple empirical scaling: disk scale radius ~ 0.03 * Rvir
 * Typical values: Rvir ~ 0.1-1 Mpc/h → Rd ~ 3-30 kpc/h
 *
 * @param rvir Virial radius (Mpc/h)
 * @return Disk scale radius (Mpc/h)
 */
static double calculate_disk_scale_radius(float rvir)
{
    return DISK_FRACTION * rvir;
}

/**
 * @brief Calculate critical disk mass for stability
 *
 * Implements the Mo, Mao & White (1998) stability criterion:
 *   Mcrit = Vmax^2 * Reff / G
 * where Reff = STAR_FORMING_DISK_FACTOR * DiskScaleRadius
 *
 * @param vmax Maximum circular velocity (km/s)
 * @param disk_scale_radius Disk scale radius (Mpc/h)
 * @param G_code Gravitational constant in code units
 * @return Critical disk mass (1e10 Msun/h)
 */
static double calculate_critical_disk_mass(float vmax, float disk_scale_radius, double G_code)
{
    double reff = STAR_FORMING_DISK_FACTOR * disk_scale_radius;
    double mcrit = safe_div(vmax * vmax * reff, G_code, 0.0);
    return mcrit;
}

// ============================================================================
// MODULE LIFECYCLE FUNCTIONS
// ============================================================================

int sage_disk_instability_init(void)
{
    LOAD_AND_VALIDATE_OPTION("DiskInstabilityOn", DISK_INSTABILITY_ON, 1,
                              "0=disabled, 1=enabled");
    LOAD_AND_VALIDATE_RANGE_INCLUSIVE("StarFormingDiskFactor", STAR_FORMING_DISK_FACTOR,
                                       0.0, 10.0, "star forming disk factor");

    INFO_LOG("SAGE disk instability module initialized");
    if (DISK_INSTABILITY_ON) {
        VERBOSE_LOG("  DiskInstabilityOn = 1 (enabled)");
        VERBOSE_LOG("  StarFormingDiskFactor = %.2f", STAR_FORMING_DISK_FACTOR);
    } else {
        VERBOSE_LOG("  DiskInstabilityOn = 0 (disabled)");
    }

    return 0;
}

int sage_disk_instability_process(struct ModuleContext *ctx, struct Halo *halos, int ngal)
{
    /* Skip if module is disabled */
    if (!DISK_INSTABILITY_ON) {
        return 0;
    }

    /* Verify process_by_galaxy mode */
    if (ngal != 1) {
        ERROR_LOG("sage_disk_instability expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    if (gal == NULL) {
        return 0;
    }

    /* Initialize disk scale radius if not yet set */
    if (gal->DiskScaleRadius <= 0.0) {
        gal->DiskScaleRadius = calculate_disk_scale_radius(halo->Rvir);
    }

    /* Calculate total disk mass (cold gas + stellar disk) */
    double disk_stellar_mass = gal->StellarMass - gal->BulgeMass;
    double disk_mass = gal->ColdGas + disk_stellar_mass;

    /* If disk mass is zero or negative, disk is stable */
    if (disk_mass <= 0.0) {
        gal->UnstableDiskGasFraction = 0.0;
        gal->UnstableDiskStarFraction = 0.0;
        return 0;
    }

    /* Calculate critical disk mass for stability (Mo, Mao & White 1998) */
    double mcrit = calculate_critical_disk_mass(halo->Vmax, gal->DiskScaleRadius,
                                                 ctx->params->G);

    /* Limit critical mass to actual disk mass */
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

    /* Calculate unstable fractions
     * gas_fraction = ColdGas / disk_mass
     * unstable_gas = gas_fraction * (disk_mass - mcrit)
     * UnstableDiskGasFraction = unstable_gas / ColdGas */
    gal->UnstableDiskGasFraction = safe_div(gal->ColdGas, disk_mass, 0.0) *
                                   safe_div(unstable_total, gal->ColdGas, 0.0);

    gal->UnstableDiskStarFraction = safe_div(disk_stellar_mass, disk_mass, 0.0) *
                                    safe_div(unstable_total, disk_stellar_mass, 0.0);

    /* Transfer unstable stellar mass to bulge if stars are unstable */
    if (gal->UnstableDiskStarFraction > 0.0 && disk_stellar_mass > 0.0) {
        /* Calculate unstable stellar mass */
        double unstable_stars = gal->UnstableDiskStarFraction * disk_stellar_mass;

        if (unstable_stars > 0.0) {
            /* Calculate disk metallicity (excluding existing bulge) */
            double disk_metal_mass = gal->MetalsStellarMass - gal->MetalsBulgeMass;
            double metallicity = mimic_get_metallicity(disk_stellar_mass, disk_metal_mass);

            /* Transfer unstable stars to bulge (preserving metallicity) */
            gal->BulgeMass += unstable_stars;
            gal->MetalsBulgeMass += metallicity * unstable_stars;

            /* Enforce physical constraints (bulge cannot exceed total stellar mass) */
            if (gal->BulgeMass > gal->StellarMass * MASS_TOLERANCE_FACTOR) {
                WARNING_LOG("Disk instability: Bulge mass %.4e exceeds stellar mass %.4e in halo %d, correcting",
                           gal->BulgeMass, gal->StellarMass, halo->HaloNr);
                gal->BulgeMass = gal->StellarMass;
            }

            if (gal->MetalsBulgeMass > gal->MetalsStellarMass * MASS_TOLERANCE_FACTOR) {
                WARNING_LOG("Disk instability: Bulge metals %.4e exceed stellar metals %.4e in halo %d, correcting",
                           gal->MetalsBulgeMass, gal->MetalsStellarMass, halo->HaloNr);
                gal->MetalsBulgeMass = gal->MetalsStellarMass;
            }

            DEBUG_LOG("Halo %d: Disk unstable - transferred %.3e Msun to bulge (gas_frac=%.4f, star_frac=%.4f)",
                      halo->HaloNr, unstable_stars, gal->UnstableDiskGasFraction, gal->UnstableDiskStarFraction);
        }
    }

    return 0;
}

int sage_disk_instability_cleanup(void)
{
    VERBOSE_LOG("SAGE disk instability module cleaned up");
    return 0;
}
