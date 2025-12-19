/**
 * @file    sage_transfer_disk_to_bulge.c
 * @brief   Transfer unstable stellar mass from disk to bulge
 *
 * This module implements the mass transfer component of disk instability,
 * moving stellar mass (and associated metals) from the disk to the bulge
 * when the disk becomes gravitationally unstable.
 *
 * Physics Summary:
 *   - Reads UnstableDiskStarFraction (set by sage_check_disk_instability)
 *   - Calculates unstable stellar mass = UnstableDiskStarFraction * disk_stars
 *   - Transfers unstable_stars to BulgeMass (preserving metallicity)
 *   - Enforces physical constraints (bulge <= total stellar mass)
 *
 * Module Communication:
 *   Reads:  UnstableDiskStarFraction, StellarMass, BulgeMass,
 *           MetalsStellarMass, MetalsBulgeMass
 *   Writes: BulgeMass, MetalsBulgeMass
 *
 * References:
 *   - SAGE: sage-code/model_disk_instability.c (lines 35-54)
 *   - Mo, Mao & White (1998) - Disk stability criterion
 *
 * Vision Principles:
 *   - Single Responsibility: Only transfers stellar mass, nothing else
 *   - Module Independence: Reads trigger property, acts independently
 *   - KISS: Simple mass transfer with metallicity preservation
 */

#include "constants.h"
#include "error.h"
#include "../modules/_shared/metallicity.h"
#include "module_interface.h"
#include "types.h"

/* ============================================================================
 * PHYSICS CONSTANTS
 * ============================================================================ */

/* Numerical tolerance for mass conservation checks */
static const double MASS_TOLERANCE_FACTOR = 1.0001;  /* 0.01% rounding allowance */

/* ============================================================================
 * MODULE LIFECYCLE FUNCTIONS
 * ============================================================================ */

/**
 * @brief   Initialize the transfer disk to bulge module
 */
int sage_transfer_disk_to_bulge_init(void)
{
    VERBOSE_LOG("SAGE Transfer Disk to Bulge initialized");
    return 0;
}

/**
 * @brief   Clean up the transfer disk to bulge module
 */
int sage_transfer_disk_to_bulge_cleanup(void)
{
    return 0;
}

/**
 * @brief   Transfer unstable stellar mass from disk to bulge
 *
 * This function:
 * 1. Checks UnstableDiskStarFraction (set by sage_check_disk_instability)
 * 2. Calculates unstable stellar mass
 * 3. Computes disk metallicity (excluding existing bulge)
 * 4. Transfers unstable stars and metals to bulge
 * 5. Enforces physical constraints (bulge mass sanity checks)
 *
 * Mass Conservation:
 *   - Total stellar mass (StellarMass) remains unchanged
 *   - Only redistribution from disk component to bulge component
 *   - Metallicity is preserved during transfer
 *
 * @param   ctx     Module execution context
 * @param   halos   Array of halos (ngal=1 for process_by_galaxy)
 * @param   ngal    Number of halos (should be 1)
 * @return  0 on success, non-zero on error
 */
int sage_transfer_disk_to_bulge_process(struct ModuleContext *ctx,
                                        struct Halo *halos,
                                        int ngal)
{
    (void)ctx;  /* Unused in this module */

    /* Verify process_by_galaxy mode (ngal should be 1) */
    if (ngal != 1) {
        ERROR_LOG("sage_transfer_disk_to_bulge expects ngal=1, got %d", ngal);
        return -1;
    }

    struct Halo *halo = &halos[0];
    struct GalaxyData *gal = halo->galaxy;

    if (gal == NULL) {
        return 0;
    }

    /* Check trigger: are stars unstable?
     * UnstableDiskStarFraction is set by sage_check_disk_instability */
    if (gal->UnstableDiskStarFraction <= 0.0) {
        return 0;  /* No unstable stars, nothing to transfer */
    }

    /* Calculate disk stellar mass (total stars - bulge)
     * This is the mass available in the disk for potential transfer */
    double disk_stellar_mass = gal->StellarMass - gal->BulgeMass;

    /* Calculate unstable stellar mass
     * This is the amount that needs to be transferred to make disk stable */
    double unstable_stars = gal->UnstableDiskStarFraction * disk_stellar_mass;

    if (unstable_stars <= 0.0) {
        return 0;  /* No mass to transfer */
    }

    /* Calculate disk metallicity (excluding existing bulge)
     * We need the metallicity of just the disk stars being transferred
     * SAGE: lines 37 */
    double disk_metal_mass = gal->MetalsStellarMass - gal->MetalsBulgeMass;
    double metallicity = mimic_get_metallicity(disk_stellar_mass, disk_metal_mass);

    /* Transfer unstable stars to bulge (SAGE: lines 39-40)
     * This is a pure redistribution - total stellar mass is conserved */
    gal->BulgeMass += unstable_stars;
    gal->MetalsBulgeMass += metallicity * unstable_stars;

    /* Sanity checks: enforce physical constraints
     * Due to numerical precision, bulge mass might slightly exceed stellar mass
     * Correct if this happens to maintain physical validity
     * SAGE: lines 47-51 */
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

    DEBUG_LOG("Halo %d: Transferred %.3e Msun from disk to bulge (instability)",
              halo->HaloNr, unstable_stars);

    return 0;
}
